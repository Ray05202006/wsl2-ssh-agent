/*
 * wsl2-ssh-agent -- expose the Windows OpenSSH agent to WSL2 as a UNIX socket.
 *
 * Design (see DESIGN.md):
 *   ssh <-unix socket-> wsl2-ssh-agent --(posix_spawn per connection)-->
 *     wsl2-ssh-agent-helper.exe (stdin/stdout = the client socket, via WSL
 *     interop) <-named pipe-> ssh-agent.exe
 *
 * The daemon never touches agent traffic: it accepts a connection, spawns the
 * native helper with the accepted socket as its stdin/stdout, closes its copy
 * and goes back to a blocking accept(). It is single-threaded, has no timers
 * and no polling, so it does not wake up at all while idle.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "(development version)"
#endif
#define MAX_CHILDREN 64 /* concurrent client connections */

extern char **environ;

/* the Windows helper, embedded at build time (see Makefile) */
#ifdef HELPER_EXE
/* the assembler computes the size, so C never subtracts pointers into two
 * different objects (undefined behaviour even when they are adjacent) */
__asm__(".section .rodata\n.balign 16\n.globl helper_exe\nhelper_exe:\n.incbin \"" HELPER_EXE "\"\n"
        "helper_exe_end:\n.balign 8\n.globl helper_exe_size\nhelper_exe_size:\n.quad helper_exe_end - helper_exe\n"
        ".previous\n");
extern const char helper_exe[];
extern const unsigned long long helper_exe_size;
#endif

static struct {
	const char *socket, *helper, *pipename, *log, *format;
	int foreground, verbose, stop, version, prespawn;
} opt = {.pipename = "openssh-ssh-agent", .format = "auto"};

static int log_fd = -1;     /* -1 = logging disabled */
static int child_err = -1;  /* stderr for helpers */
static volatile sig_atomic_t quit, nchild;

static void logf_(const char *fmt, ...) {
	if (log_fd < 0) return;
	char b[1024];
	time_t t = time(NULL);
	struct tm tm;
	int n = (int)strftime(b, sizeof b, "[L] %Y/%m/%d %H:%M:%S ", localtime_r(&t, &tm));
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(b + n, sizeof b - n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof b - 2) n = sizeof b - 2;
	b[n++] = '\n';
	if (write(log_fd, b, n) < 0) {}
}

static _Noreturn void die(const char *fmt, ...) {
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	logf_("%s", b);
	if (log_fd != STDERR_FILENO) fprintf(stderr, "wsl2-ssh-agent: %s\n", b);
	exit(1);
}

/* ---- command line: same flags as the original Go version (-x, --x, -x=v, -x v) ---- */

static void usage(FILE *f) {
	fputs("usage: wsl2-ssh-agent\n"
	      "  -socket PATH     UNIX domain socket to listen on (default ~/.ssh/wsl2-ssh-agent.sock)\n"
	      "  -pipename NAME   Windows named pipe to connect to (default openssh-ssh-agent)\n"
	      "  -helper PATH     use this wsl2-ssh-agent-helper.exe instead of the embedded one\n"
	      "  -prespawn        keep one helper started ahead of time (lower connection\n"
	      "                   latency, ~1 MB resident on Windows; see DESIGN.md)\n"
	      "  -foreground      run in foreground mode\n"
	      "  -verbose         verbose mode\n"
	      "  -log FILE        append the log to FILE\n"
	      "  -format FMT      output format: auto, bash, zsh, csh, tcsh, or fish (default auto)\n"
	      "  -stop            stop the running daemon and exit\n"
	      "  -version         print version and exit\n",
	      f);
}

static void parse_args(int argc, char **argv) {
	static const struct { const char *name; const char **s; int *b; } t[] = {
		{"socket", &opt.socket, 0},       {"helper", &opt.helper, 0},
		{"pipename", &opt.pipename, 0},   {"log", &opt.log, 0},
		{"format", &opt.format, 0},       {"powershell-path", 0, 0}, /* obsolete, ignored */
		{"foreground", 0, &opt.foreground}, {"verbose", 0, &opt.verbose},
		{"stop", 0, &opt.stop},           {"version", 0, &opt.version},
		{"prespawn", 0, &opt.prespawn},
	};
	for (int i = 1; i < argc; i++) {
		char *a = argv[i];
		if (a[0] != '-' || !a[1]) { fprintf(stderr, "unexpected argument: %s\n", a); usage(stderr); exit(2); }
		a += a[1] == '-' ? 2 : 1;
		if (!strcmp(a, "h") || !strcmp(a, "help")) { usage(stdout); exit(0); }
		char *eq = strchr(a, '=');
		size_t len = eq ? (size_t)(eq - a) : strlen(a);
		size_t k;
		for (k = 0; k < sizeof t / sizeof *t; k++)
			if (strlen(t[k].name) == len && !strncmp(a, t[k].name, len)) break;
		if (k == sizeof t / sizeof *t) { fprintf(stderr, "flag provided but not defined: %s\n", argv[i]); usage(stderr); exit(2); }
		if (t[k].b) {
			*t[k].b = !eq || !strcmp(eq + 1, "true") || !strcmp(eq + 1, "1");
		} else {
			const char *v = eq ? eq + 1 : (i + 1 < argc ? argv[++i] : NULL);
			if (!v) { fprintf(stderr, "flag needs an argument: %s\n", argv[i]); exit(2); }
			if (t[k].s) *t[k].s = v;
		}
	}
}

static const char *output_line(void) {
	const char *f = opt.format, *sh = getenv("SHELL");
	if (!strcmp(f, "auto")) {
		size_t n = sh ? strlen(sh) : 0;
		f = n >= 4 && !strcmp(sh + n - 4, "fish") ? "fish" : n >= 3 && !strcmp(sh + n - 3, "csh") ? "csh" : "sh";
	}
	static char out[4200];
	if (!strcmp(f, "sh") || !strcmp(f, "bash") || !strcmp(f, "zsh"))
		snprintf(out, sizeof out, "SSH_AUTH_SOCK=%s; export SSH_AUTH_SOCK;", opt.socket);
	else if (!strcmp(f, "csh") || !strcmp(f, "tcsh"))
		snprintf(out, sizeof out, "setenv SSH_AUTH_SOCK %s", opt.socket);
	else if (!strcmp(f, "fish"))
		snprintf(out, sizeof out, "set -x SSH_AUTH_SOCK %s", opt.socket);
	else { printf("output format must be auto, bash, zsh, csh, tcsh, or fish\n"); exit(1); }
	return out;
}

/* snprintf for paths: a truncated path would silently name the wrong file */
static void pathf(char *buf, size_t size, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= size) die("path too long");
}

/* ---- the Windows helper ---- */

/* private, owner-only directory; refuse anything an attacker could have prepared */
static int safe_dir(const char *d) {
	struct stat st;
	if (mkdir(d, 0700) < 0 && errno != EEXIST) return -1;
	if (lstat(d, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 022)) return -1;
	return 0;
}

static const char *prepare_helper(void) {
	if (opt.helper) {
		if (access(opt.helper, X_OK) < 0) die("helper %s is not executable: %s", opt.helper, strerror(errno));
		return opt.helper;
	}
#ifdef HELPER_EXE
	static char path[4200], dir[4096], tmp[4300];
	const char *rt = getenv("XDG_RUNTIME_DIR"), *home = getenv("HOME");
	if (rt && *rt && safe_dir(rt) == 0) {
		pathf(dir, sizeof dir, "%s/wsl2-ssh-agent", rt);
	} else if (home && *home) {
		pathf(dir, sizeof dir, "%s/.cache", home);
		safe_dir(dir);
		pathf(dir, sizeof dir, "%s/.cache/wsl2-ssh-agent", home);
	} else die("neither XDG_RUNTIME_DIR nor HOME is set");
	if (safe_dir(dir) < 0) die("unsafe or unusable directory %s", dir);
	pathf(path, sizeof path, "%s/wsl2-ssh-agent-helper.exe", dir);

	/* reuse an identical copy (the normal case), otherwise write + rename atomically */
	size_t n = (size_t)helper_exe_size;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd >= 0) {
		static char cur[1 << 16];
		struct stat st;
		ssize_t r = n <= sizeof cur && fstat(fd, &st) == 0 && st.st_uid == getuid() &&
		                    (size_t)st.st_size == n && (st.st_mode & 0777) == 0700
		                ? read(fd, cur, sizeof cur) : -1;
		close(fd);
		if (r == (ssize_t)n && !memcmp(cur, helper_exe, n)) return path;
	}
	pathf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0700);
	if (fd < 0 || write(fd, helper_exe, n) != (ssize_t)n || fsync(fd) < 0 || close(fd) < 0 || rename(tmp, path) < 0) {
		unlink(tmp);
		die("cannot install helper to %s: %s", path, strerror(errno));
	}
	logf_("installed helper to %s", path);
	return path;
#else
	die("built without an embedded helper; use -helper PATH");
#endif
}

/* ---- existing server detection ---- */

static int dial(struct sockaddr_un *a) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)a, sizeof *a) < 0) {
		int e = errno;
		if (fd >= 0) close(fd);
		errno = e;
		return -1;
	}
	return fd;
}

/* pid of the server already listening on the socket, or -1 */
static pid_t running_server(struct sockaddr_un *a) {
	int fd = dial(a);
	if (fd < 0) {
		if (errno == ENOENT) return -1;
		if (errno == ECONNREFUSED) { /* stale socket left by a dead server */
			if (unlink(opt.socket) < 0) die("failed to remove %s: %s", opt.socket, strerror(errno));
			return -1;
		}
		die("failed to connect to %s: %s", opt.socket, strerror(errno));
	}
	struct ucred c;
	socklen_t l = sizeof c;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &c, &l) < 0) die("SO_PEERCRED: %s", strerror(errno));
	close(fd);
	if (c.uid != getuid()) die("%s is served by another user (uid %d)", opt.socket, (int)c.uid);
	return c.pid;
}

static void stop_server(pid_t pid) {
	int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
	logf_("kill wsl2-ssh-agent (pid: %d)", (int)pid);
	if (kill(pid, SIGTERM) < 0) die("kill %d: %s", (int)pid, strerror(errno));
	if (pfd >= 0) { /* wait for exit without polling */
		struct pollfd p = {pfd, POLLIN, 0};
		if (poll(&p, 1, 5000) == 0) die("pid %d did not exit", (int)pid);
	}
}

/* ---- serving ---- */

static void on_signal(int s) { (void)s; quit = 1; }

static void on_child(int s) {
	(void)s;
	int e = errno;
	while (waitpid(-1, NULL, WNOHANG) > 0) nchild--;
	errno = e;
}

static posix_spawnattr_t spawn_attr;
static char *spawn_argv[3];  /* helper PIPENAME */
static char *ready_argv[4];  /* helper --ready PIPENAME: standby helpers */

/* start the helper with fd as its stdin/stdout; returns 0 or an errno */
static int spawn_argv_helper(char **argv, int fd, pid_t *pid) {
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fd, 0);
	posix_spawn_file_actions_adddup2(&fa, fd, 1);
	posix_spawn_file_actions_adddup2(&fa, child_err, 2);
	int e = posix_spawn(pid, argv[0], &fa, &spawn_attr, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	if (e == 0) nchild++;
	else logf_("failed to spawn %s: %s", argv[0], strerror(e));
	return e;
}
static int spawn_helper(int fd, pid_t *pid) { return spawn_argv_helper(spawn_argv, fd, pid); }

/* a standby helper on a fresh socketpair; returns our end or -1 */
static int spawn_standby(pid_t *pid) {
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) return -1;
	int e = spawn_argv_helper(ready_argv, sv[1], pid);
	close(sv[1]);
	if (e) { close(sv[0]); return -1; }
	return sv[0];
}

/* ---- -prespawn: one warm standby helper ----
 *
 * The spare helper sits on one end of a socketpair; its Windows process is
 * already running, so a client only pays for a local hand-off instead of the
 * ~40 ms WSL interop process start. On connect, a forked relay process copies
 * bytes between the client and the spare (the accept loop itself never
 * blocks on a client), and the next spare is started immediately.
 *
 * WSL interop can fail to start a Windows process (/init logs
 * "UtilAcceptVsock: accept4 failed 110"); the socketpair then just reports
 * EOF. So standbys run with --ready and write one 0xff byte as soon as the
 * Windows process is up. Until that byte arrives, a standby is not known to
 * exist: the relay holds the client's bytes, and if the launch fails it
 * starts another helper and replays them. Nothing has reached the agent at
 * that point, so the replay is safe. After the byte, nothing is replayed.
 */
#define READY_BYTE 0xff
#define READY_TRIES 3       /* launches per client before giving up */
#define READY_MS 10000      /* per launch */
static int spare_fd = -1, spare_ready;
static pid_t spare_pid;
static struct timespec spare_born, respawn_at;
static long respawn_ms; /* backoff for helpers that die right after start */

static long ms_since(const struct timespec *t) {
	struct timespec n;
	clock_gettime(CLOCK_MONOTONIC, &n);
	return (n.tv_sec - t->tv_sec) * 1000 + (n.tv_nsec - t->tv_nsec) / 1000000;
}

static void schedule_respawn(void) {
	respawn_ms = respawn_ms ? (respawn_ms * 2 > 60000 ? 60000 : respawn_ms * 2) : 100;
	clock_gettime(CLOCK_MONOTONIC, &respawn_at);
	respawn_at.tv_sec += respawn_ms / 1000;
	respawn_at.tv_nsec += (respawn_ms % 1000) * 1000000;
	if (respawn_at.tv_nsec >= 1000000000) respawn_at.tv_sec++, respawn_at.tv_nsec -= 1000000000;
	logf_("standby helper unavailable; retrying in %ld ms", respawn_ms);
}

static void make_spare(void) {
	spare_fd = spawn_standby(&spare_pid);
	spare_ready = 0;
	if (spare_fd < 0) { schedule_respawn(); return; }
	clock_gettime(CLOCK_MONOTONIC, &spare_born);
	logf_("standby helper started (pid %d)", (int)spare_pid);
}

static int write_all(int fd, const char *p, ssize_t n) {
	while (n > 0) {
		ssize_t w = write(fd, p, n);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) return -1;
		p += w, n -= w;
	}
	return 0;
}

/* runs in a forked child: client <-> standby helper until the helper is done.
 * ready: the daemon already got the helper's ready byte. */
static _Noreturn void relay(int client, int h, pid_t hpid, int ready) {
	static char buf[16384], early[16384];
	size_t elen = 0;
	int ceof = 0, tries = 1;
	struct timespec t0;
	prctl(PR_SET_NAME, "wsa-relay");
	clock_gettime(CLOCK_MONOTONIC, &t0);

	/* phase 1: wait for the ready byte, holding (not losing) client bytes */
	while (!ready) {
		long left = READY_MS - ms_since(&t0);
		struct pollfd p[2] = {{h, POLLIN, 0}, {ceof || elen == sizeof early ? -1 : client, POLLIN, 0}};
		int r = left > 0 ? poll(p, 2, (int)left) : 0;
		if (r < 0) {
			if (errno == EINTR) continue;
			_exit(1);
		}
		if (p[1].revents) {
			ssize_t n = read(client, early + elen, sizeof early - elen);
			if (n < 0 && errno == EINTR) continue;
			if (n <= 0) ceof = 1;
			else elen += n;
		}
		if (r > 0 && !p[0].revents) continue;
		unsigned char b = 0;
		if (r > 0 && read(h, &b, 1) == 1 && b == READY_BYTE) { ready = 1; break; }
		/* launch failed (EOF), timed out, or bogus byte: try a fresh helper */
		if (r == 0) kill(hpid, SIGKILL);
		close(h);
		if (tries++ >= READY_TRIES) {
			logf_("helper did not start after %d attempts; dropping the connection", READY_TRIES);
			_exit(1);
		}
		logf_("helper failed to start; retrying (attempt %d of %d)", tries, READY_TRIES);
		if ((h = spawn_standby(&hpid)) < 0) _exit(1);
		clock_gettime(CLOCK_MONOTONIC, &t0);
	}
	if (write_all(h, early, (ssize_t)elen) < 0) _exit(0);
	if (ceof) shutdown(h, SHUT_WR);

	/* phase 2: plain byte copy */
	struct pollfd p[2] = {{ceof ? -1 : client, POLLIN, 0}, {h, POLLIN, 0}};
	for (;;) {
		if (poll(p, 2, -1) < 0) {
			if (errno == EINTR) continue;
			_exit(1);
		}
		for (int i = 0; i < 2; i++) {
			if (!p[i].revents) continue;
			ssize_t n = read(p[i].fd, buf, sizeof buf);
			if (n < 0 && errno == EINTR) continue;
			if (n <= 0) {
				/* helper finished: done. client half-closed: tell the helper,
				 * which answers what it already has and then exits. */
				if (i == 1) _exit(0);
				shutdown(h, SHUT_WR);
				p[0].fd = -1;
				continue;
			}
			if (write_all(i ? client : h, buf, n) < 0) _exit(0);
		}
	}
}

static void serve(int lfd, const char *helper) {
	/* Signals are blocked except while sleeping in ppoll(), so a SIGTERM can
	 * never slip in between the quit check and the wait. No timeout (except
	 * a pending standby respawn): the process sleeps until a client connects
	 * or a signal arrives. */
	sigset_t block, waitmask;
	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGINT);
	sigaddset(&block, SIGHUP);
	sigaddset(&block, SIGCHLD);
	sigprocmask(SIG_BLOCK, &block, &waitmask);

	sigset_t none, dfl;
	sigemptyset(&none);
	sigemptyset(&dfl);
	sigaddset(&dfl, SIGPIPE);
	sigaddset(&dfl, SIGCHLD);
	posix_spawnattr_init(&spawn_attr);
	posix_spawnattr_setsigmask(&spawn_attr, &none);
	posix_spawnattr_setsigdefault(&spawn_attr, &dfl);
	posix_spawnattr_setflags(&spawn_attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
	spawn_argv[0] = (char *)helper;
	spawn_argv[1] = (char *)opt.pipename;
	ready_argv[0] = (char *)helper;
	ready_argv[1] = "--ready";
	ready_argv[2] = (char *)opt.pipename;
	int throttle = 0;
	/* with -prespawn each connection is a relay process + a helper process */
	int max_children = opt.prespawn ? 2 * MAX_CHILDREN + 1 : MAX_CHILDREN;

	if (opt.prespawn) make_spare();

	while (!quit) {
		/* at the connection limit (or out of fds) only wait for children to exit */
		int lim = nchild >= max_children || throttle;
		struct pollfd p[2] = {{lim ? -1 : lfd, POLLIN, 0}, {spare_fd, POLLIN, 0}};
		struct timespec to, *top = NULL;
		if (opt.prespawn && spare_fd < 0) { /* respawn pending */
			long ms = -ms_since(&respawn_at);
			if (ms < 0) ms = 0;
			to.tv_sec = ms / 1000, to.tv_nsec = (ms % 1000) * 1000000, top = &to;
		}
		if (ppoll(p, 2, top, &waitmask) < 0 && errno != EINTR) die("ppoll: %s", strerror(errno));
		if (quit) break;
		throttle = 0;

		if (spare_fd >= 0 && p[1].revents) {
			/* the first byte is the ready byte; after that an idle helper
			 * never writes, so readable/HUP means it exited (or never started) */
			unsigned char b = 0;
			if (!spare_ready && read(spare_fd, &b, 1) == 1 && b == READY_BYTE) {
				spare_ready = 1;
			} else {
				long age = ms_since(&spare_born);
				if (!spare_ready) logf_("standby helper failed to start");
				close(spare_fd);
				spare_fd = -1;
				if (age < 2000) schedule_respawn();
				else respawn_ms = 0, make_spare();
			}
		} else if (opt.prespawn && spare_fd < 0 && ms_since(&respawn_at) >= 0) {
			make_spare();
		}
		if (!(p[0].revents & POLLIN)) continue;

		int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC); /* lfd is non-blocking */
		if (c < 0) {
			if (errno == EAGAIN || errno == EINTR || errno == ECONNABORTED) continue;
			if ((errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) && nchild > 0) {
				logf_("accept: %s; waiting for a connection to finish", strerror(errno));
				throttle = 1;
				continue;
			}
			die("accept: %s", strerror(errno));
		}
		/* defence in depth on top of the 0600 socket: only our own uid (or root) */
		struct ucred cr = {0};
		socklen_t l = sizeof cr;
		if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &l) < 0) {
			logf_("rejected connection: SO_PEERCRED: %s", strerror(errno));
			close(c);
			continue;
		}
		if (cr.uid != getuid() && cr.uid != 0) {
			logf_("rejected connection from uid %d", (int)cr.uid);
			close(c);
			continue;
		}

		pid_t pid;
		if (spare_fd >= 0) {
			pid = fork();
			if (pid == 0) {
				struct sigaction sa = {0};
				sa.sa_handler = SIG_DFL;
				sigaction(SIGTERM, &sa, NULL);
				sigaction(SIGINT, &sa, NULL);
				sigaction(SIGHUP, &sa, NULL);
				sigaction(SIGCHLD, &sa, NULL);
				sigprocmask(SIG_SETMASK, &none, NULL);
				close(lfd);
				relay(c, spare_fd, spare_pid, spare_ready);
			}
			if (pid > 0) {
				nchild++;
				close(spare_fd);
				spare_fd = -1;
				close(c);
				respawn_ms = 0;
				logf_("ssh: connected (standby helper, relay pid %d)", (int)pid);
				make_spare();
				continue;
			}
			logf_("fork: %s; starting a helper directly", strerror(errno));
		}
		if (spawn_helper(c, &pid) == 0) logf_("ssh: connected (helper pid %d, %d active)", (int)pid, (int)nchild);
		close(c);
	}
	logf_("shutdown");
}

/* before listen(): once a client (or -stop) can reach us, a SIGTERM must
 * already mean a clean shutdown that removes the socket */
static void install_signals(void) {
	struct sigaction sa = {0};
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sa.sa_handler = on_child;
	sa.sa_flags = SA_NOCLDSTOP;
	sigaction(SIGCHLD, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

int main(int argc, char **argv) {
	static char sockbuf[4096];
	parse_args(argc, argv);
	if (opt.version) { printf("wsl2-ssh-agent %s\n", VERSION); return 0; }

	/* make sure fds 0-2 exist so accepted sockets never land on them */
	int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
	while (nul >= 0 && nul <= 2) nul = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (nul < 0) return 1;

	if (!opt.socket) {
		const char *home = getenv("HOME");
		if (!home || !*home) die("HOME is not set; use -socket");
		pathf(sockbuf, sizeof sockbuf, "%s/.ssh/wsl2-ssh-agent.sock", home);
		opt.socket = sockbuf;
	}
	const char *out = output_line();

	if (opt.log) {
		log_fd = open(opt.log, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
		if (log_fd < 0) die("failed to open a log file: %s", strerror(errno));
	} else if (opt.verbose) {
		log_fd = opt.foreground ? STDOUT_FILENO : -1;
	}
	child_err = log_fd >= 0 ? log_fd : nul;

	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	if (strlen(opt.socket) >= sizeof addr.sun_path) die("socket path too long: %s", opt.socket);
	strcpy(addr.sun_path, opt.socket);

	pid_t running = running_server(&addr);
	if (opt.stop) {
		if (running < 0) die("failed to find wsl2-ssh-agent");
		stop_server(running);
		return 0;
	}
	if (running >= 0) {
		logf_("wsl2-ssh-agent (pid: %d) is already running; exit", (int)running);
		puts(out);
		return 0;
	}

	const char *helper = prepare_helper();

	mode_t old = umask(0177); /* socket file is created 0600 */
	int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0)
		die("failed to listen on %s: %s", opt.socket, strerror(errno));
	umask(old);
	struct stat sock_st; /* identifies our socket file for clean-up at exit */
	if (stat(opt.socket, &sock_st) < 0) die("stat %s: %s", opt.socket, strerror(errno));

	/* listen() is called by the process that will serve: the kernel records
	 * the SO_PEERCRED pid at listen() time, and -stop relies on it. */
	if (!opt.foreground) {
		int rp[2];
		char ok;
		fflush(stdout);
		if (pipe2(rp, O_CLOEXEC) < 0) die("pipe: %s", strerror(errno));
		pid_t p = fork();
		if (p < 0) die("fork: %s", strerror(errno));
		if (p > 0) { /* print the snippet only once the daemon accepts connections */
			close(rp[1]);
			if (read(rp[0], &ok, 1) != 1) { unlink(opt.socket); _exit(1); }
			puts(out);
			fflush(stdout);
			_exit(0);
		}
		close(rp[0]);
		setsid();
		install_signals();
		if (listen(lfd, 64) < 0) die("failed to listen on %s: %s", opt.socket, strerror(errno));
		logf_("start listening on %s", opt.socket);
		if (write(rp[1], "", 1) != 1) _exit(1);
		close(rp[1]);
		dup2(nul, 0);
		dup2(nul, 1);
		dup2(log_fd >= 0 ? log_fd : nul, 2);
		if (log_fd == STDOUT_FILENO) log_fd = -1;
	} else {
		install_signals();
		if (listen(lfd, 64) < 0) die("failed to listen on %s: %s", opt.socket, strerror(errno));
		logf_("start listening on %s", opt.socket);
	}
	if (chdir("/") < 0) {}


	serve(lfd, helper);

	/* remove the socket only if it is still the one we created */
	struct stat now;
	if (stat(opt.socket, &now) == 0 && now.st_ino == sock_st.st_ino && now.st_dev == sock_st.st_dev)
		unlink(opt.socket);
	return 0;
}
