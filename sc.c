#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "utils.h"

static int
connect_socket(const char *path)
{
	struct sockaddr_un addr = {0};
	int fd;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		warn(errno, "socket");
		return -1;
	}
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == -1) {
		warn(errno, "connect: %s", path);
		close(fd);
		return -1;
	}
	return fd;
}

int
main(int argc, char *argv[])
{
	const char *sockpath = NULL;
	const char *command  = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
			sockpath = argv[++i];
		} else if (!command) {
			command = argv[i];
		} else {
			fprintf(stderr,
			    "Usage: sc -s <socket-path> <command>\n"
			    "Commands: exit list loop consume pause play skip"
			    " stop toggle status clear\n");
			return 1;
		}
	}
	if (!sockpath || !command) {
		fprintf(stderr,
		    "Usage: sc -s <socket-path> <command>\n"
		    "Commands: exit list loop consume pause play skip"
		    " stop toggle status\n");
		return 1;
	}

	int sock = connect_socket(sockpath);
	if (sock == -1) return 1;

	dprintf(sock, "%s\n", command);

	if (strcmp(command, "list") == 0 || strcmp(command, "status") == 0) {
		char buf[4096];
		ssize_t n;
		while ((n = read(sock, buf, sizeof buf)) > 0) {
			if (write(STDOUT_FILENO, buf, (size_t) n) < 0) {
				warn(errno, "write");
				break;
			}
		}
	}

	close(sock);
	return 0;
}
