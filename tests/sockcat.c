/*
 * sockcat SOCK [TIMEOUT_MS]: send stdin to a UNIX socket, half-close, copy the
 * reply to stdout until the peer closes. Exits 3 on timeout (peer hung).
 */
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv) {
	if (argc < 2) return 2;
	int to = argc > 2 ? atoi(argv[2]) : 5000;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	strncpy(a.sun_path, argv[1], sizeof a.sun_path - 1);
	if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) return 1;
	char b[65536];
	ssize_t n;
	while ((n = read(0, b, sizeof b)) > 0)
		if (write(fd, b, n) != n) return 1;
	shutdown(fd, SHUT_WR);
	for (;;) {
		struct pollfd p = {fd, POLLIN, 0};
		if (poll(&p, 1, to) == 0) return 3;
		if ((n = read(fd, b, sizeof b)) <= 0) return 0;
		if (write(1, b, n) != n) return 1;
	}
}
