/*
 * Minimal Win32 shim so windows/helper.c compiles and runs on Linux under
 * ASan/UBSan (tests/fuzz/run.sh). Only what helper.c uses is provided.
 *
 *   stdin  -> client requests (fd 0)          stdout -> replies to client (fd 1)
 *   the named pipe is a fake agent:
 *     FAKE_AGENT=echo     replies with the request it got (a valid frame)
 *     FAKE_AGENT=garbage  replies with the bytes of $FAKE_REPLY (file), raw
 *     FAKE_AGENT=missing  CreateFileW fails with ERROR_FILE_NOT_FOUND
 *     FAKE_AGENT=busy     CreateFileW fails with ERROR_PIPE_BUSY
 *   FAKE_OLD_AGENT=1 makes the version check report OpenSSH 8.1.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

typedef int BOOL;
typedef uint32_t DWORD, *LPDWORD;
typedef unsigned int UINT, *PUINT;
typedef unsigned long long ULONGLONG;
typedef wchar_t WCHAR;
typedef const WCHAR *LPCWSTR;
typedef void *HANDLE, *HMODULE, *LPVOID;
typedef const void *LPCVOID;
typedef intptr_t (*FARPROC)(void);
typedef struct { DWORD dwSignature, dwStrucVersion, dwFileVersionMS, dwFileVersionLS; } VS_FIXEDFILEINFO;
#define WINAPI
#define __stdcall
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define HIWORD(x) ((DWORD)(x) >> 16)
#define LOWORD(x) ((DWORD)(x) & 0xffff)
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define STD_INPUT_HANDLE 0
#define STD_OUTPUT_HANDLE 1
#define STD_ERROR_HANDLE 2
#define GENERIC_READ 1
#define GENERIC_WRITE 2
#define OPEN_EXISTING 3
#define SECURITY_SQOS_PRESENT 0
#define SECURITY_IDENTIFICATION 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PIPE_BUSY 231

static DWORD shim_err;
static HANDLE const PIPE_H = (HANDLE)(intptr_t)100;
static unsigned char shim_req[4 + 256 * 1024 + 16];
static size_t shim_req_len, shim_rep_off, shim_rep_len;
static unsigned char *shim_rep;
static int shim_mode = -1; /* 0 echo, 1 garbage, 2 missing, 3 busy */
static ULONGLONG shim_ticks;

static void shim_init(void) {
	if (shim_mode >= 0) return;
	const char *m = getenv("FAKE_AGENT");
	shim_mode = !m || !strcmp(m, "echo") ? 0 : !strcmp(m, "garbage") ? 1 : !strcmp(m, "missing") ? 2 : 3;
	if (shim_mode == 1) {
		FILE *f = fopen(getenv("FAKE_REPLY"), "rb");
		shim_rep = malloc(1 << 20);
		if (!shim_rep) abort();
		shim_rep_len = f ? fread(shim_rep, 1, 1 << 20, f) : 0;
		if (f) fclose(f);
	}
}

static HANDLE GetStdHandle(DWORD n) { return (HANDLE)(intptr_t)n; }
static DWORD GetLastError(void) { return shim_err; }
static void ExitProcess(UINT c) { exit((int)c); }
static const WCHAR *GetCommandLineW(void) { return L"helper.exe"; }
static ULONGLONG GetTickCount64(void) { return shim_ticks += 50; } /* virtual clock */
static void Sleep(DWORD ms) { shim_ticks += ms; }
static BOOL WaitNamedPipeW(LPCWSTR n, DWORD t) { (void)n; shim_ticks += t / 8 + 1; return TRUE; }

static HANDLE CreateFileW(LPCWSTR n, DWORD a, DWORD s, void *sa, DWORD c, DWORD f, HANDLE t) {
	(void)n; (void)a; (void)s; (void)sa; (void)c; (void)f; (void)t;
	shim_init();
	if (shim_mode == 2) { shim_err = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
	if (shim_mode == 3) { shim_err = ERROR_PIPE_BUSY; return INVALID_HANDLE_VALUE; }
	return PIPE_H;
}

static BOOL WriteFile(HANDLE h, const void *p, DWORD n, LPDWORD w, void *o) {
	(void)o;
	if (h == PIPE_H) { /* request to the fake agent */
		if (shim_mode != 0) shim_req_len = 0; /* garbage agent ignores requests */
		if (shim_req_len + n > sizeof shim_req) abort(); /* helper must never send more than one frame */
		memcpy(shim_req + shim_req_len, p, n);
		shim_req_len += n;
		if (shim_mode == 0) shim_rep = shim_req, shim_rep_len = shim_req_len, shim_rep_off = 0;
		*w = n;
		return TRUE;
	}
	ssize_t r = write((int)(intptr_t)h, p, n);
	if (r <= 0) return FALSE;
	*w = (DWORD)r;
	return TRUE;
}

static BOOL ReadFile(HANDLE h, void *p, DWORD n, LPDWORD r, void *o) {
	(void)o;
	if (h == PIPE_H) {
		size_t left = shim_rep_len - shim_rep_off;
		if (left == 0) { *r = 0; return FALSE; } /* agent closed */
		if (n > left) n = (DWORD)left;
		if (n > 7) n = 7; /* deliver in small pieces to exercise partial reads */
		memcpy(p, shim_rep + shim_rep_off, n);
		shim_rep_off += n;
		if (shim_mode == 0 && shim_rep_off == shim_rep_len) shim_req_len = shim_rep_off = shim_rep_len = 0;
		*r = n;
		return TRUE;
	}
	ssize_t k = read((int)(intptr_t)h, p, n);
	if (k < 0) return FALSE;
	*r = (DWORD)k;
	return TRUE;
}

/* version check */
static DWORD SearchPathW(LPCWSTR a, LPCWSTR b, LPCWSTR c, DWORD n, WCHAR *out, WCHAR **f) {
	(void)a; (void)b; (void)c; (void)f;
	wcsncpy(out, L"C:\\ssh-agent.exe", n);
	return 16;
}
static DWORD fake_size(LPCWSTR p, LPDWORD d) { (void)p; *d = 0; return 64; }
static BOOL fake_info(LPCWSTR p, DWORD h, DWORD n, LPVOID d) { (void)p; (void)h; memset(d, 0, n); return TRUE; }
static BOOL fake_query(LPCVOID d, LPCWSTR s, LPVOID *out, PUINT len) {
	static VS_FIXEDFILEINFO fi;
	(void)d; (void)s;
	fi.dwFileVersionMS = getenv("FAKE_OLD_AGENT") ? (8u << 16 | 1) : (9u << 16 | 5);
	*out = &fi;
	*len = sizeof fi;
	return TRUE;
}
static HMODULE LoadLibraryW(LPCWSTR n) { (void)n; return (HMODULE)(intptr_t)1; } /* cppcheck-suppress intToPointerCast */
static BOOL FreeLibrary(HMODULE m) { (void)m; return TRUE; }
static FARPROC GetProcAddress(HMODULE m, const char *n) {
	(void)m;
	return !strcmp(n, "GetFileVersionInfoSizeW") ? (FARPROC)fake_size
	       : !strcmp(n, "GetFileVersionInfoW")   ? (FARPROC)fake_info
	                                             : (FARPROC)fake_query;
}
static HANDLE GetProcessHeap(void) { return NULL; }
static void *HeapAlloc(HANDLE h, DWORD f, size_t n) { (void)h; (void)f; return malloc(n); }
static BOOL HeapFree(HANDLE h, DWORD f, void *p) { (void)h; (void)f; free(p); return TRUE; }
