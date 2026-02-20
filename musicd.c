#include <errno.h> /* errno(3p). */
#include <poll.h> /* poll(2). */
#include <signal.h> /* SIGHUP. */
#include <stdio.h> /* snprintf(3p). */
#include <stdlib.h> /* malloc(3p). */
#include <string.h> /* strcmp(3p), strncpy(3p). */
#include <sys/socket.h> /* bind(3p), socket(3p), struct sock_addr(3type). */
#include <sys/un.h> /* sockaddr_un. */
#include <sys/wait.h> /* wait(2). */
#include <unistd.h> /* close(2p), readlink(2). */

#include "queue.h"
#include "song.h"
#include "utils.h"

enum mode {
	CONSUME,
	EXITING,
	LOOP,
	SHUFFLE,
	STOPPED,
};

enum fdtype {
	FD_SOCK = 0,
	FD_MPRIS,
	FD_END,
};

struct state {
	enum mode mode;
	bool play;
	struct queue queue;
	struct song cur;
};

struct fds {
	unsigned int size, n;
	int ready;
	[[clang::counted_by(n), clang::sized_by(size)]]
	struct pollfd *fds;
};

static pid_t player_pid;

[[gnu::const]]
static struct state cmd(struct state, unsigned int, const char *[]);
[[gnu::const]]
static struct state cmd_exit(struct state, unsigned int, const char *[]);
[[gnu::const]]
static struct state cmd_queue(struct state, unsigned int, const char *[]);
[[gnu::const]]
static struct state cmd_pause(struct state, unsigned int, const char *[]);
[[gnu::const]]
static struct state cmd_play(struct state, unsigned int, const char *[]);
[[gnu::const]]
static struct state cmd_push(struct state, unsigned int, const char *[]);
[[gnu::const]]
static struct state cmd_skip(struct state, unsigned int, const char *[]);
[[gnu::const]]
static struct state cmd_stop(struct state, unsigned int, const char *[]);
[[gnu::const]]
static struct state cmd_toggle(struct state, unsigned int, const char *[]);

static bool effect(const struct state, const struct state);

[[gnu::nonnull(1)]]
static int mksocket(const char *);

[[gnu::nonnull(1)]]
static void cleanup_state(struct state *);
[[gnu::nonnull(1)]]
static void cleanup_fds(struct fds *);

static const char *
fdpath(int fd)
{
	char *path = nullptr;
	size_t size = 0;

	if (fd < 0)
		return nullptr;

	char procpath[__builtin_strlen("/proc/self/fd/") + 5];
	snprintf(procpath, sizeof procpath, "/proc/self/fd/%d", fd);

	ssize_t rl_size; /* chars written by readlink(2). */
	while ((rl_size = readlink(procpath, path, size)) <= (ssize_t) size) {
		if (rl_size == -1)
			die(errno, "readlink");

		path = realloc(path, 2 * size * sizeof(char));
		if (!path)
			die(errno, "realloc");
	}
	path[rl_size] = '\0';

	return path;
}

static void
cleanup_fds(struct fds *fds)
{
	const char *sockpath = fdpath(fds->fds[FD_SOCK].fd);
	if (sockpath) {
		unlink(sockpath);
		free((char *) sockpath);
	}
	
	for (int i = 0; fds->n; i += 1) {
		if (fds->fds[i].fd >= 0) {
			close(fds->fds[i].fd);
		}
	}

	free(fds->fds);
}

static void
cleanup_state(struct state *s)
{
	free(s->queue.q);
}

static struct state
cmd(struct state s, unsigned int argc, const char *args[])
{
#define CMD(name)                        \
	if (strcmp(args[0], #name) == 0) \
		return cmd_ ## name (s, argc - 1, &args[1])

	CMD(exit);
	CMD(queue);
	CMD(pause);
	CMD(play);
	CMD(push);
	CMD(skip);
	CMD(stop);
	CMD(toggle);

	/* Invalid command. */
	return s;
}

static struct state
cmd_exit(                struct state s,
         [[gnu::unused]] unsigned int argc,
         [[gnu::unused]] const char *args[])
{
	s.mode = EXITING;
	return s;
}

static struct state
cmd_queue(struct state s, unsigned int argc, const char *args[])
{
	if (argc != 2) {
		/* TODO Log error. */
		return s;
	}

	char *end;
	const unsigned long uid = strtol(args[1], &end, 10);
	if (end != args[1] + strlen(args[1])) {
		/* TODO Log error. */
		return s;
	}

	const struct song song = { .path = args[0], .uid = uid };
	s.queue = push(s.queue, song);
	/* TODO Error handling. */

	return s;
}

static struct state
cmd_pause(                struct state s,
          [[gnu::unused]] unsigned int argc,
          [[gnu::unused]] const char *args[])
{
	s.play = false;
	return s;
}

static struct state
cmd_play(                struct state s,
         [[gnu::unused]] unsigned int argc,
         [[gnu::unused]] const char *args[])
{
	s.play = true;
	return s;
}

static struct state
cmd_push(struct state s, unsigned int argc, const char *args[])
{
	if (argc != 2) {
		/* TODO Log error. */
		return s;
	}

	return s;
}

static struct state
cmd_skip(                struct state s,
         [[gnu::unused]] unsigned int argc,
         [[gnu::unused]] const char *args[])
{
	if (qsize(s.queue))
		s.queue = pop(s.queue, &s.cur);
	else
		s.mode = STOPPED;

	return s;
}

static struct state
cmd_stop(                struct state s,
         [[gnu::unused]] unsigned int argc,
         [[gnu::unused]] const char *args[])
{
	s.mode = STOPPED;
	return s;
}

static struct state
cmd_toggle(                struct state s,
           [[gnu::unused]] unsigned int argc,
           [[gnu::unused]] const char *args[])
{
	s.play = !s.play;
	return s;
}

static bool
effect(const struct state old, const struct state new)
{
	int sig = 0;

	if (qsize(new.queue) < qsize(old.queue)) {
		if (kill(player_pid, SIGHUP) == -1) {
			debug(errno, "kill");
			return false;
		}

		if (wait(&sig) == -1) {
			
		}
	}

	if (old.mode != new.mode) {
		switch (new.mode) {
		case STOPPED:
			sig = SIGHUP;
			goto end;
		default:
			break;
		}
	}

	if (old.play != new.play) {
		sig = new.play ? SIGUSR1 : SIGUSR2;
	}

end:
	if (sig && kill(player_pid, sig) == -1) {
		debug(0, errno, "kill");
	}
}

static int
mksocket(const char *path)
{
	int sockfd;
	struct sockaddr_un addr;

	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sockfd == -1) {
		debug(errno, "socket");
		return -1;
	}

	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);

	if (bind(sockfd, (struct sockaddr *) &addr, sizeof addr) == -1) {
		debug(errno, "bind");
		close(sockfd);
		return -1;
	}

	return sockfd;
}

int
main(int argc, char *argv[])
{
	[[gnu::cleanup(cleanup_state)]]
	struct state s;

	[[gnu::cleanup(cleanup_fds)]]
	struct fds fds;

	if (argc < 2)
		die(0, "Usage: %s path", argv[0]);

	fds.fds = malloc(3 * sizeof(struct pollfd));
	if (!fds.fds)
		die(errno, "malloc");

	fds.fds[FD_SOCK].fd = mksocket(argv[1]);
	if (fds.fds[FD_SOCK].fd == -1)
		die(0, "Can't create socket");

	memset(&s.queue, 0, sizeof s.queue);
	s.queue = requeue(s.queue, 64);
	if (s.queue.size < 1)
		die(0, "Can't allocate the queue");

	while (s.mode != EXITING && (fds.ready = poll(fds.fds, FD_END, -1)) != -1) {
		struct state newstate;

                if (effect(s, newstate))
                	s = newstate;
        }
}
