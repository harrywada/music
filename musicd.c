#include <errno.h>       /* errno(3p). */
#include <poll.h>        /* poll(2). */
#include <signal.h>      /* SIGHUP, SIGCHLD. */
#include <stdio.h>       /* snprintf(3p). */
#include <stdlib.h>      /* malloc(3p), free(3p). */
#include <string.h>      /* memchr(3), memmove(3), strtok(3). */
#include <sys/signalfd.h> /* signalfd(2). */
#include <sys/socket.h>  /* accept(3p), bind(3p), listen(3p), socket(3p). */
#include <sys/un.h>      /* sockaddr_un. */
#include <sys/wait.h>    /* waitpid(2). */
#include <syslog.h>      /* openlog(3), LOG_*(3). */
#include <unistd.h>      /* close(2p), read(2). */

#include "cmds.h"
#include "mpris.h"
#include "queue.h"
#include "state.h"
#include "utils.h"

#ifndef PLAY_PATH
#define PLAY_PATH "./play"
#endif

#define CLIENT_BUF_SIZE 256
#define CMD_ARGV_MAX    8

enum fdtype {
	FD_SOCK = 0,
	FD_SIG,
	FD_MPRIS,
	FD_END,
};

struct client {
	int fd;
	char buf[CLIENT_BUF_SIZE];
	unsigned int len;
};

struct fds {
	unsigned int size, n;
	int ready;
	struct pollfd *fds;
	struct client *clients;
	unsigned int nclients, client_cap;
	const char *sockpath;
};

static pid_t player_pid;

static bool effect(const struct state, const struct state);

static void
cleanup_mpris(struct mpris **m)
{
	mpris_close(*m);
}

[[gnu::nonnull(1)]]
static int mksocket(const char *);

[[gnu::nonnull(1)]]
static void cleanup_fds(struct fds *);

static void
cleanup_fds(struct fds *fds)
{
	if (fds->sockpath)
		unlink(fds->sockpath);

	for (unsigned int i = 0; i < fds->n; i++) {
		if (i == FD_MPRIS) continue; /* fd owned by sd-bus */
		if (fds->fds[i].fd >= 0)
			close(fds->fds[i].fd);
	}
	for (unsigned int i = 0; i < fds->nclients; i++)
		close(fds->clients[i].fd);

	free(fds->fds);
	free(fds->clients);
}

static pid_t
spawn_player(const struct song *song)
{
	pid_t pid = fork();
	if (pid == -1) {
		debug(errno, "fork");
		return -1;
	}
	if (pid == 0) {
		char uid_str[21]; /* Enough for ULONG_MAX. */
		snprintf(uid_str, sizeof uid_str, "%lu", song->uid);
		execl(PLAY_PATH, "play", song->path, uid_str, (char *) nullptr);
		die(errno, "execl");
	}
	return pid;
}

static bool
effect(const struct state old, const struct state new)
{
	bool song_changed = (old.queue.head != new.queue.head);

	/* Kill the running player if stopping, the song changed, or exiting. */
	if (player_pid != -1
	    && (new.play == STOPPED || song_changed || new.mode == EXITING)) {
		if (kill(player_pid, SIGHUP) == -1) {
			debug(errno, "kill");
			return false;
		}
		/* Reaping happens asynchronously in the SIGCHLD handler. */
		player_pid = -1;
	}

	if (new.play == PLAYING
	    && (old.play != PLAYING || song_changed)) {
		if (!qsize(new.queue))
			return true;
		if (player_pid != -1) {
			/* Was paused — resume without respawning. */
			if (kill(player_pid, SIGUSR1) == -1) {
				debug(errno, "kill");
				return false;
			}
		} else {
			player_pid = spawn_player(
			    &new.queue.data[new.queue.head % new.queue.size]);
			if (player_pid == -1)
				return false;
		}
	} else if (new.play == PAUSED && old.play == PLAYING) {
		if (player_pid != -1 && kill(player_pid, SIGUSR2) == -1) {
			debug(errno, "kill");
			return false;
		}
	}

	return true;
}

static bool
add_client(struct fds *fds, int fd)
{
	if (fds->nclients == fds->client_cap) {
		unsigned int newcap = fds->client_cap == 0 ? 4 : 2 * fds->client_cap;
		struct client *newclients = realloc(fds->clients,
		                                    newcap * sizeof(struct client));
		if (!newclients) {
			debug(errno, "realloc");
			return false;
		}
		struct pollfd *newfds = realloc(fds->fds,
		    (FD_END + newcap) * sizeof(struct pollfd));
		if (!newfds) {
			debug(errno, "realloc");
			return false;
		}
		fds->fds = newfds;
		fds->clients = newclients;
		fds->client_cap = newcap;
	}

	unsigned int i = fds->nclients++;
	fds->clients[i] = (struct client) { .fd = fd, .len = 0 };
	fds->fds[FD_END + i] = (struct pollfd) { .fd = fd, .events = POLLIN };
	return true;
}

static void
remove_client(struct fds *fds, unsigned int i)
{
	unsigned int last = fds->nclients - 1;
	if (i != last) {
		fds->clients[i]         = fds->clients[last];
		fds->fds[FD_END + i]    = fds->fds[FD_END + last];
	}
	fds->nclients--;
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

	if (listen(sockfd, SOMAXCONN) == -1) {
		debug(errno, "listen");
		close(sockfd);
		return -1;
	}

	return sockfd;
}

int
main(int argc, char *argv[])
{
	openlog("musicd", LOG_PID, LOG_DAEMON);

	[[gnu::cleanup(cleanup_state)]]
	struct state state;

	[[gnu::cleanup(cleanup_fds)]]
	struct fds fds = { 0 };

	if (argc < 2)
		die(0, "Usage: %s path", argv[0]);

	if (!mkstate(&state))
		die(0, "Can't make state");

	fds.fds = malloc(FD_END * sizeof(struct pollfd));
	if (!fds.fds)
		die(errno, "malloc");
	fds.n    = FD_END;
	fds.size = FD_END;

	fds.fds[FD_SOCK].fd = mksocket(argv[1]);
	if (fds.fds[FD_SOCK].fd == -1)
		die(0, "Can't create socket");
	fds.fds[FD_SOCK].events = POLLIN;
	fds.sockpath = argv[1];

	{
		sigset_t mask;
		sigemptyset(&mask);
		sigaddset(&mask, SIGCHLD);
		if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1)
			die(errno, "sigprocmask");
		fds.fds[FD_SIG].fd = signalfd(-1, &mask, SFD_CLOEXEC);
		if (fds.fds[FD_SIG].fd == -1)
			die(errno, "signalfd");
		fds.fds[FD_SIG].events = POLLIN;
	}

	fds.fds[FD_MPRIS].fd     = -1; /* poll(2) ignores negative fds. */
	fds.fds[FD_MPRIS].events = 0;

	[[gnu::cleanup(cleanup_mpris)]]
	struct mpris *mpris = mpris_open(state);
	if (mpris) {
		fds.fds[FD_MPRIS].fd     = mpris_fd(mpris);
		fds.fds[FD_MPRIS].events = mpris_events(mpris);
	}

	player_pid = -1;

	while (state.mode != EXITING) {
		/* Drain sd-bus before blocking so that any messages buffered
		   internally (e.g. NameOwnerChanged after request_name) are
		   processed before sd_bus_get_events() is consulted.  Without
		   this, sd_bus_get_events() may return 0 while rqueue_size > 0,
		   causing poll(2) to never watch FD_MPRIS for POLLIN. */
		if (mpris) {
			int r;
			while ((r = mpris_process(mpris, &state)) > 0)
				;
			if (r < 0)
				debug(-r, "mpris_process");
		}

		if (mpris)
			fds.fds[FD_MPRIS].events = mpris_events(mpris);
		fds.ready = poll(fds.fds, FD_END + fds.nclients, -1);
		if (fds.ready == -1)
			break;

		struct state newstate = state;

		/* New client connection. */
		if (fds.fds[FD_SOCK].revents & POLLIN) {
			int cfd = accept(fds.fds[FD_SOCK].fd, nullptr, nullptr);
			if (cfd == -1)
				debug(errno, "accept");
			else if (!add_client(&fds, cfd))
				close(cfd);
		}

		/* SIGCHLD — player process exited. */
		if (fds.fds[FD_SIG].revents & POLLIN) {
			struct signalfd_siginfo si;
			if (read(fds.fds[FD_SIG].fd, &si, sizeof si) == (ssize_t) sizeof si
			    && si.ssi_signo == SIGCHLD) {
				int wstatus;
				if (waitpid(player_pid, &wstatus, WNOHANG) == player_pid) {
					player_pid = -1;
					if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0)
						debug(0, "player exited with status %d",
						      WEXITSTATUS(wstatus));
					/* Song finished naturally — advance per queue mode. */
					if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
						switch (newstate.mode) {
						case CONSUME:
							newstate = cmd_skip(newstate, 0, nullptr);
							break;
						case LOOP: {
							struct song cur;
							newstate.queue = pop(newstate.queue, &cur);
							newstate.queue = push(newstate.queue, cur);
							break;
						}
						case EXITING:
							break;
						}
					}
				}
			}
		}

		/* Client commands. */
		for (unsigned int i = 0; i < fds.nclients; ) {
			struct pollfd *pfd    = &fds.fds[FD_END + i];
			struct client *client = &fds.clients[i];

			if (pfd->revents & (POLLERR | POLLNVAL)) {
				close(client->fd);
				remove_client(&fds, i);
				continue;
			}

			if (pfd->revents & (POLLIN | POLLHUP)) {
				ssize_t n = read(pfd->fd,
				                 client->buf + client->len,
				                 CLIENT_BUF_SIZE - 1 - client->len);
				if (n <= 0) {
					close(client->fd);
					remove_client(&fds, i);
					continue;
				}
				client->len += (unsigned int) n;

				char *start = client->buf;
				char *nl;
				while ((nl = memchr(start, '\n',
				    client->len - (unsigned int)(start - client->buf)))) {
					*nl = '\0';

					if (*start)
						debug(0, "%s", start);

					const char *args[CMD_ARGV_MAX];
					unsigned int nargs = 0;
					char *tok = strtok(start, " ");
					while (tok && nargs < CMD_ARGV_MAX) {
						args[nargs++] = tok;
						tok = strtok(nullptr, " ");
					}
					if (nargs > 0) {
					if (strcmp(args[0], "list") == 0)
						cmd_list(newstate, client->fd);
					else if (strcmp(args[0], "status") == 0)
						cmd_status(newstate, client->fd);
					else
						newstate = cmd(newstate, nargs, args);
					close(client->fd);
					remove_client(&fds, i);
					goto next_client;
				}

					start = nl + 1;
				}

				unsigned int remaining = client->len
				    - (unsigned int)(start - client->buf);
				memmove(client->buf, start, remaining);
				client->len = remaining;
			}

			i++;
next_client:;
		}

		/* MPRIS — handle new incoming D-Bus traffic. */
		if (mpris && fds.fds[FD_MPRIS].revents) {
			int r;
			while ((r = mpris_process(mpris, &newstate)) > 0)
				;
			if (r < 0)
				debug(-r, "mpris_process");
		}

		struct state prev = state;
		if (effect(state, newstate))
			state = newstate;
		if (mpris)
			mpris_notify(mpris, prev, state);
	}
}
