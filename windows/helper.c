/*
 * wsl2-ssh-agent-helper.exe -- relay ONE ssh-agent connection between
 * stdin/stdout (connected by WSL interop to a UNIX socket client) and a
 * Windows named pipe (default \\.\pipe\openssh-ssh-agent).
 *
 *   wsl2-ssh-agent-helper.exe [PIPENAME]
 *
 * One process per client connection (started on connect, or ahead of time
 * with the daemon's -prespawn): the process lives exactly as long as
 * the ssh/ssh-add connection, so (without -prespawn) nothing stays resident on Windows when no
 * client is connected, and every client gets its own pipe connection (so
 * per-connection agent state such as session-bind is never shared).
 *
 * Built without the C runtime (kernel32 only); see windows/msvc.sh.
 */
#include <windows.h>

#define MAX_MSG (256 * 1024) /* same limit as OpenSSH's AGENT_MAX_LEN */
#define SSH_AGENT_FAILURE 5
#define SSH_AGENTC_EXTENSION 27

static unsigned char buf[4 + MAX_MSG];
static HANDLE err_h;

static void say(const char *s) {
	DWORD n = 0, w;
	while (s[n]) n++;
	WriteFile(err_h, s, n, &w, NULL);
}

static void say_num(const char *s, DWORD v) {
	char d[12];
	int i = 11;
	d[i] = 0;
	do d[--i] = (char)('0' + v % 10); while ((v /= 10) && i);
	say(s);
	say(d + i);
	say("\n");
}

static BOOL rd(HANDLE h, unsigned char *p, DWORD n) {
	while (n) {
		DWORD r;
		if (!ReadFile(h, p, n, &r, NULL) || r == 0) return FALSE;
		p += r, n -= r;
	}
	return TRUE;
}

static BOOL wr(HANDLE h, const unsigned char *p, DWORD n) {
	while (n) {
		DWORD w;
		if (!WriteFile(h, p, n, &w, NULL) || w == 0) return FALSE;
		p += w, n -= w;
	}
	return TRUE;
}

/* 1 = message in buf (total length in *len), 0 = clean EOF / error, -1 = bad length */
static int read_msg(HANDLE h, DWORD *len) {
	DWORD n;
	if (!rd(h, buf, 4)) return 0;
	n = (DWORD)buf[0] << 24 | (DWORD)buf[1] << 16 | (DWORD)buf[2] << 8 | buf[3];
	if (n == 0 || n > MAX_MSG) return -1;
	if (!rd(h, buf + 4, n)) return 0;
	*len = n + 4;
	return 1;
}

/*
 * Windows OpenSSH before 8.9 breaks on SSH_AGENTC_EXTENSION (sent by newer
 * ssh clients for session-bind). Same policy as the original PowerShell
 * relay: look up ssh-agent.exe on PATH and, if it is older than 8.9 or cannot
 * be found, answer extension requests locally with SSH_AGENT_FAILURE (which
 * clients treat as "extension not supported"). Checked lazily, only when an
 * extension message actually arrives.
 */
typedef DWORD(WINAPI *GFVISW)(LPCWSTR, LPDWORD);
typedef BOOL(WINAPI *GFVIW)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI *VQVW)(LPCVOID, LPCWSTR, LPVOID *, PUINT);

static BOOL agent_lacks_extensions(void) {
	WCHAR path[MAX_PATH];
	HMODULE v;
	GFVISW size_f;
	GFVIW info_f;
	VQVW query_f;
	DWORD sz, dummy;
	void *data;
	VS_FIXEDFILEINFO *fi;
	UINT fl;
	BOOL old = TRUE;

	if (!SearchPathW(NULL, L"ssh-agent.exe", NULL, MAX_PATH, path, NULL)) return TRUE;
	v = LoadLibraryW(L"version.dll");
	if (!v) return TRUE;
	size_f = (GFVISW)GetProcAddress(v, "GetFileVersionInfoSizeW");
	info_f = (GFVIW)GetProcAddress(v, "GetFileVersionInfoW");
	query_f = (VQVW)GetProcAddress(v, "VerQueryValueW");
	if (size_f && info_f && query_f && (sz = size_f(path, &dummy)) != 0 &&
	    (data = HeapAlloc(GetProcessHeap(), 0, sz)) != NULL) {
		if (info_f(path, 0, sz, data) && query_f(data, L"\\", (void **)&fi, &fl) && fl >= sizeof *fi) {
			DWORD major = HIWORD(fi->dwFileVersionMS), minor = LOWORD(fi->dwFileVersionMS);
			old = major < 8 || (major == 8 && minor < 9);
		}
		HeapFree(GetProcessHeap(), 0, data);
	}
	FreeLibrary(v);
	return old;
}

static HANDLE open_pipe(const WCHAR *name) {
	/* give up after 10 s, the same limit the original relay used for replies */
	ULONGLONG deadline = GetTickCount64() + 10000;
	DWORD backoff = 1;
	for (;;) {
		/* SECURITY_IDENTIFICATION: the pipe server may identify us but never
		 * impersonate us, which limits the damage if something squats the
		 * pipe name. */
		HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		                       SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
		ULONGLONG now = GetTickCount64();
		if (h != INVALID_HANDLE_VALUE || now >= deadline) return h;
		switch (GetLastError()) {
		case ERROR_PIPE_BUSY:
			/* every listening instance is taken: sleep until one is free
			 * (event-driven; several waiters may race for it, so loop) */
			WaitNamedPipeW(name, (DWORD)(deadline - now));
			break;
		case ERROR_FILE_NOT_FOUND:
			/* ssh-agent.exe creates its next pipe instance only after it has
			 * accepted the previous client, so under concurrent connects there
			 * are short windows with no instance at all. Retry with bounded
			 * exponential backoff (1+2+...+256 ms, ~0.5 s) before deciding the
			 * agent is not running. */
			if (backoff > 256) return h;
			Sleep(backoff);
			backoff *= 2;
			break;
		default:
			return h;
		}
	}
}

void __stdcall start(void) {
	static WCHAR name[300] = L"\\\\.\\pipe\\";
	const WCHAR *cl = GetCommandLineW();
	HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE), pipe;
	int extcheck = -1, r;
	DWORD len, i = 9;

	err_h = GetStdHandle(STD_ERROR_HANDLE);

	/* skip argv[0] (possibly quoted), then the rest of the line is the pipe name */
	if (*cl == L'"') {
		for (cl++; *cl && *cl != L'"'; cl++);
		if (*cl) cl++;
	} else {
		while (*cl && *cl != L' ' && *cl != L'\t') cl++;
	}
	while (*cl == L' ' || *cl == L'\t') cl++;
	if (*cl == L'"') cl++;
	if (!*cl) cl = L"openssh-ssh-agent";
	while (*cl && *cl != L'"' && i < 299) name[i++] = *cl++;
	while (i > 9 && (name[i - 1] == L' ' || name[i - 1] == L'\t')) i--;
	name[i] = 0;

	/* The pipe is opened when the first message arrives, not at start-up: a
	 * standby helper (-prespawn) may wait for hours, and must not hold a pipe
	 * connection that goes stale if the agent restarts meanwhile. */
	pipe = INVALID_HANDLE_VALUE;

	while ((r = read_msg(in, &len)) == 1) {
		if (buf[4] == SSH_AGENTC_EXTENSION) {
			if (extcheck < 0) extcheck = agent_lacks_extensions();
			if (extcheck) {
				static const unsigned char fail[5] = {0, 0, 0, 1, SSH_AGENT_FAILURE};
				if (!wr(out, fail, 5)) break;
				continue;
			}
		}
		if (pipe == INVALID_HANDLE_VALUE && (pipe = open_pipe(name)) == INVALID_HANDLE_VALUE) {
			say_num("[W] cannot connect to the agent's named pipe (is the ssh-agent service running?): error ", GetLastError());
			ExitProcess(2);
		}
		if (!wr(pipe, buf, len)) { say("[W] write to agent failed\n"); ExitProcess(3); }
		r = read_msg(pipe, &len);
		if (r != 1) { say("[W] read from agent failed\n"); ExitProcess(3); }
		if (!wr(out, buf, len)) break;
	}
	if (r < 0) say("[W] invalid message length from client\n");
	ExitProcess(r < 0 ? 4 : 0);
}
