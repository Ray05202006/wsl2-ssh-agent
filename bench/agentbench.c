/*
 * agentbench: measure SSH agent request latency over a UNIX socket.
 *
 *   agentbench SOCK OP N [CONC] [RAWFILE]
 *     OP   = list   SSH_AGENTC_REQUEST_IDENTITIES (what `ssh-add -l` sends)
 *            sign   SSH_AGENTC_SIGN_REQUEST with the first key (what ssh
 *                   sends during public-key authentication)
 *            conn   connect + list + close (one ssh invocation = one connection)
 *     N    = requests per worker
 *     CONC = parallel workers (each with its own connection), default 1
 *
 * Prints min/p50/p95/p99/max in microseconds. Raw per-request latencies (us)
 * are appended to RAWFILE if given.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static const char *sock_path;
static int op_kind; /* 0 list, 1 sign, 2 conn */
static int n_req;
static uint8_t key_blob[8192];
static uint32_t key_len;

static double now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int dial(void) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	strncpy(a.sun_path, sock_path, sizeof(a.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { perror("connect"); exit(1); }
	return fd;
}

static int rfull(int fd, void *b, size_t n) {
	size_t o = 0;
	while (o < n) {
		ssize_t r = read(fd, (char *)b + o, n - o);
		if (r <= 0) return -1;
		o += r;
	}
	return 0;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* send msg, read reply into rep (returns reply length, body only) */
static int64_t rpc(int fd, const uint8_t *msg, size_t len, uint8_t *rep, size_t cap) {
	if (write(fd, msg, len) != (ssize_t)len) return -1;
	uint8_t h[4];
	if (rfull(fd, h, 4)) return -1;
	uint32_t n = be32(h);
	if (n > cap) return -1;
	if (rfull(fd, rep, n)) return -1;
	return n;
}

static uint8_t list_msg[] = {0, 0, 0, 1, 11};
static uint8_t sign_msg[16384];
static size_t sign_len;

static void build_sign(void) {
	static const char data[] = "agentbench challenge data 0123456789abcdef0123456789abcdef";
	uint8_t *p = sign_msg + 4;
	*p++ = 13;
	put32(p, key_len); p += 4;
	memcpy(p, key_blob, key_len); p += key_len;
	put32(p, sizeof data - 1); p += 4;
	memcpy(p, data, sizeof data - 1); p += sizeof data - 1;
	put32(p, 0); p += 4; /* flags */
	sign_len = p - sign_msg;
	put32(sign_msg, sign_len - 4);
}

static void fetch_key(void) {
	int fd = dial();
	static uint8_t rep[65536];
	int64_t n = rpc(fd, list_msg, sizeof list_msg, rep, sizeof rep);
	close(fd);
	if (n < 9 || rep[0] != 12 || be32(rep + 1) == 0) { fprintf(stderr, "no identities in agent\n"); exit(1); }
	key_len = be32(rep + 5);
	if (key_len > sizeof key_blob) exit(1);
	memcpy(key_blob, rep + 9, key_len);
}

typedef struct { double *lat; int fails; } work;

static void *worker(void *arg) {
	work *w = arg;
	static __thread uint8_t rep[65536];
	int fd = op_kind == 2 ? -1 : dial();
	for (int i = 0; i < n_req; i++) {
		double t0 = now_us();
		int64_t r;
		if (op_kind == 2) {
			fd = dial();
			r = rpc(fd, list_msg, sizeof list_msg, rep, sizeof rep);
			close(fd);
		} else if (op_kind == 0) {
			r = rpc(fd, list_msg, sizeof list_msg, rep, sizeof rep);
		} else {
			r = rpc(fd, sign_msg, sign_len, rep, sizeof rep);
			if (r > 0 && rep[0] != 14) r = -1;
		}
		w->lat[i] = now_us() - t0;
		if (r < 0) {
			w->fails++;
			if (op_kind != 2) { close(fd); fd = dial(); }
		}
	}
	if (fd >= 0 && op_kind != 2) close(fd);
	return NULL;
}

static int cmp(const void *a, const void *b) {
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
	if (argc < 4) { fprintf(stderr, "usage: %s SOCK list|sign|conn N [CONC] [RAWFILE]\n", argv[0]); return 2; }
	sock_path = argv[1];
	op_kind = !strcmp(argv[2], "sign") ? 1 : !strcmp(argv[2], "conn") ? 2 : 0;
	n_req = atoi(argv[3]);
	int conc = argc > 4 ? atoi(argv[4]) : 1;
	if (op_kind == 1) { fetch_key(); build_sign(); }

	pthread_t th[256];
	work w[256];
	if (conc > 256) conc = 256;
	double t0 = now_us();
	for (int i = 0; i < conc; i++) {
		w[i].lat = calloc(n_req, sizeof(double));
		w[i].fails = 0;
		pthread_create(&th[i], NULL, worker, &w[i]);
	}
	int total = n_req * conc, fails = 0;
	double *all = malloc(total * sizeof(double));
	for (int i = 0; i < conc; i++) {
		pthread_join(th[i], NULL);
		memcpy(all + i * n_req, w[i].lat, n_req * sizeof(double));
		fails += w[i].fails;
	}
	double wall = now_us() - t0;
	if (argc > 5) {
		FILE *f = fopen(argv[5], "a");
		for (int i = 0; i < total; i++) fprintf(f, "%.1f\n", all[i]);
		fclose(f);
	}
	qsort(all, total, sizeof(double), cmp);
#define P(q) all[(int)((total - 1) * (q))]
	printf("op=%s n=%d conc=%d fails=%d wall_ms=%.1f min=%.0f p50=%.0f p95=%.0f p99=%.0f max=%.0f (us)\n",
	       argv[2], total, conc, fails, wall / 1e3, all[0], P(0.50), P(0.95), P(0.99), all[total - 1]);
	return fails ? 1 : 0;
}
