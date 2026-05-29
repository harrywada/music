#include <alloca.h> /* snd_pcm_hw_params_alloca. */
#include <errno.h> /* errno(3). */
#include <fcntl.h> /* open(2). */
#include <poll.h> /* poll(2). */
#include <signal.h> /* sigaddset(3), sigemptyset(3), sigprocmask(3), sigset_t(3type). */
#include <stdint.h> /* intN_t(3type). */
#include <stdlib.h> /* EXIT_FAILURE(3const), EXIT_SUCCESS(3const), exit(3), strtoul(3). */
#include <string.h> /* strlen(3). */
#include <sys/signalfd.h> /* signalfd(2). */
#include <syslog.h>      /* openlog(3), LOG_*(3). */

#include <alsa/asoundlib.h>

#include "utils.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include "ebml.h"

#include <stdint.h>
#include <sys/types.h>
#include "matroska.h"
#include "matroska_utils.h"

/* Short helper functions to reduce a bit of redundancy. */
#define SND(cmp, exit, fn, ...) \
	if (!((err = snd_ ## fn (__VA_ARGS__)) cmp)) \
		die(0, "snd_" #fn ": %s", snd_strerror(err)); \
	if (!(err cmp)) /* Requires -Wno-empty-body. */
#define SND_ERR(cmp, fn, ...) SND(cmp, EXIT_FAILURE, fn, __VA_ARGS__)
#define SND_WARN(cmp, fn, ...) SND(cmp, 0, fn, __VA_ARGS__)

enum fds {
	FD_PCM = 0,
	FD_SIG,
	FD_END,
};

/* Convenient wrapper for accessing `snd_pcm_channel_area_t`s. */
struct buf {
	size_t n, size;
	void *addr;
};

/* Audio configuration. */
struct cfg {
	uint8_t  chans, bps;
	double   rate;
	struct mkv_range range;
	off_t start;
};

/* Mutable playback cursor: where we are in the stream. */
struct playback {
	struct mkv_cursor cursor;
	size_t pending;
	bool done;
};

struct state {
	snd_pcm_t     *pcm;
	int            src;
	struct playback play;
	struct cfg     cfg;
	struct pollfd *fds;
	bool           run;
	bool           draining;
};

static int err = 0;

/* Get the head address of the buffer. */
[[gnu::const]]
static void *head(struct buf);
/* Get the remaining space in the buffer. */
[[gnu::const]]
static size_t rem(struct buf);

/* Configuration parsing functions. */

/* Extracts necessary configuration data from Matroska data. */
[[gnu::cold, gnu::fd_arg_read(1)]]
static bool read_cfg(int, struct cfg *, uint64_t);

/* Event handling functions. */

static bool handle_alsa(struct state *);
static bool handle_sigs(struct state *);

/* Cleanup functions (used with [[cleanup(...)]]). */

void cleanup_pollfds(struct pollfd (*)[FD_END]);
void cleanup_pcm(snd_pcm_t **);

void
cleanup_pollfds(struct pollfd (*fds)[FD_END])
{
	for (int i = FD_SIG; i < FD_END; i += 1)
		close((*fds)[i].fd), errno = 0;
}

void
cleanup_pcm(snd_pcm_t **pcm)
{
	if (!*pcm) return;
	snd_pcm_close(*pcm);
	*pcm = NULL;
}

static bool
handle_alsa(struct state *s)
{
	const snd_pcm_channel_area_t *areas;
	snd_pcm_sframes_t res;
	snd_pcm_uframes_t avail, off;

	struct buf buf; /* TODO struct buf buf[cfg.chans]. */

	unsigned short revents;

	/*
	 * While draining, skip the mmap path entirely and just check
	 * whether the PCM has finished.  The poll loop uses a short
	 * timeout so we end up here regularly even if the FD never fires.
	 */
	if (s->draining) {
		snd_pcm_state_t st = snd_pcm_state(s->pcm);
		if (st == SND_PCM_STATE_SETUP || st == SND_PCM_STATE_DISCONNECTED)
			s->run = false;
		return true;
	}

	SND_WARN(== 0, pcm_poll_descriptors_revents, s->pcm, &s->fds[FD_PCM], 1, &revents) {
		return false;
	}

	if (revents & ~POLLOUT) {
		debug(0, "PCM descriptor errored");
		return false;
	} else if (!(revents & POLLOUT)) {
		return true;
	}

	SND_WARN(>= 0, pcm_avail_update, s->pcm) {
		return true;
	}
	avail = err;

	SND_WARN(== 0, pcm_mmap_begin, s->pcm, &areas, &off, &avail) {
		return true;
	}

	buf.size = snd_pcm_frames_to_bytes(s->pcm, avail);
	buf.addr = areas[0].addr + (areas[0].first / 8)
	         + off * (areas[0].step / 8);
	buf.n = 0;

	while (rem(buf)) {
		if (s->play.pending) {
			const size_t n = MIN(rem(buf), s->play.pending);
			if (read(s->src, head(buf), n) != (ssize_t) n) {
				debug(errno, "read");
				break;
			}
			buf.n += n;
			s->play.pending -= n;
			continue;
		}

		const ssize_t sz = mkv_nextframe(s->src, &s->play.cursor, &s->cfg.range);
		if (sz <= 0) {
			s->play.done = true;
			break;
		}

		const size_t n = MIN(rem(buf), (size_t) sz);
		if (read(s->src, head(buf), n) != (ssize_t) n) {
			debug(errno, "read");
			break;
		}
		buf.n += n;
		s->play.pending = (size_t) sz - n;
	}

	/* TODO Maybe avoid state, use `snd_pcm_mmap_commit_partial` instead? */
	res = snd_pcm_mmap_commit(s->pcm, off, snd_pcm_bytes_to_frames(s->pcm, buf.n));
	if (res != snd_pcm_bytes_to_frames(s->pcm, buf.n))
		die(0, "snd_pcm_mmap_commit: %s", snd_strerror(res));

	/* Autoplay doesn't work with mmap, for some reason.
	   https://github.com/alsa-project/alsa-lib/commit/bd53892 */
	if (snd_pcm_state(s->pcm) == SND_PCM_STATE_PREPARED) {
		SND_WARN(== 0, pcm_start, s->pcm);
	}

	if (s->play.done) {
		/*
		 * Switch to non-blocking mode so snd_pcm_drain returns
		 * immediately: -EAGAIN if frames are still pending (PCM
		 * enters DRAINING state and the FD fires on completion),
		 * or 0 if the buffer was already empty.  Either way we
		 * re-enter the poll loop and can still process signals.
		 */
		snd_pcm_nonblock(s->pcm, 1);
		if (snd_pcm_drain(s->pcm) == 0) {
			s->run = false;
		} else {
			s->draining = true;
		}
	}

	return true;
}

static bool
handle_sigs(struct state *s)
{
	struct signalfd_siginfo siginfo;

	/* Early return conditions. */

	if (s->fds[FD_SIG].revents & ~POLLIN) {
		debug(0, "Signal descriptor errored");
		return false;
	} else if (!(s->fds[FD_SIG].revents & POLLIN)) {
		return true;
	}

	/* Read signal info. */

	if (read(s->fds[FD_SIG].fd, &siginfo, sizeof siginfo) != sizeof siginfo) {
		debug(errno, "read");
		return true;
	}

	/* Handle signals. */

	switch (siginfo.ssi_signo) {
	case SIGHUP:
		s->run = false;
		SND_WARN(== 0, pcm_drop, s->pcm);
		return false;
	case SIGUSR1:
		SND_WARN(== 0, pcm_pause, s->pcm, 0);
		break;
	case SIGUSR2:
		SND_WARN(== 0, pcm_pause, s->pcm, 1);
		break;
	default:
		debug(0, "Invalid signal %d", siginfo.ssi_signo);
		return false;
	}

	return true;
}


static void *
head(struct buf buf)
{
	return buf.addr + buf.n;
}

static bool
read_cfg(int fd, struct cfg *cfg, uint64_t id)
{
	struct mkv_seekinfo si;
	struct mkv_info info;
	struct mkv_chapter chapter;
	struct mkv_track track;
	off_t start_pos;

	memset(cfg, 0, sizeof *cfg);

	if (ebml_skip(fd, EBML_HEADER) == -1) {
		return false;
	}
	if (ebml_descend(fd, MKV_SEGMENT) == -1) {
		return false;
	}

	if (!mkv_readseekinfo(fd, &si)) {
		return false;
	}

	seek(fd, si.segment + si.info);
	if (!mkv_readinfo(fd, &info)) {
		return false;
	}

	seek(fd, si.segment + si.chapters);
	if (!mkv_findchapter(fd, id, &chapter)) {
		return false;
	}

	seek(fd, si.segment + si.tracks);
	if (!mkv_findtrack(fd, chapter.track_uids, &track)) {
		return false;
	}

	seek(fd, si.segment + si.cues);
	if (!mkv_findcue(fd, chapter.start, info.ts_scale, track.num, &start_pos)) {
		return false;
	}

	cfg->range.ts_scale = info.ts_scale;
	cfg->range.start    = chapter.start;
	cfg->range.end      = chapter.end;
	cfg->range.track    = track.num;
	cfg->chans          = track.channels;
	cfg->bps            = track.bps;
	cfg->rate           = track.rate;
	cfg->start          = si.segment + start_pos;

	return true;
}

static size_t
rem(struct buf buf)
{
	return buf.size - buf.n;
}

int
main(int argc, char *argv[])
{
	openlog("play", LOG_PID, LOG_DAEMON);

	[[gnu::cleanup(cleanup_pollfds)]] struct pollfd fds[FD_END] = {};
	[[gnu::cleanup(cleanup_pcm)]]    snd_pcm_t    *pcm = NULL;

	struct state state = {.src = -1, .fds = fds};

	char *path;
	uint64_t chapt_id;

	int readyfd_n;

	{ /* Before anything, register signal handlers. */
		sigset_t sigmask;

		sigemptyset(&sigmask);
		sigaddset(&sigmask, SIGUSR1);
		sigaddset(&sigmask, SIGUSR2);
		sigaddset(&sigmask, SIGHUP);

		if (sigprocmask(SIG_BLOCK, &sigmask, NULL) == -1)
			die(errno, "sigprocmask");
		if ((state.fds[FD_SIG].fd = signalfd(-1, &sigmask, 0)) == -1)
			die(errno, "signalfd");
		state.fds[FD_SIG].events = POLLIN;
	}

	{ /* Assign and do some basic validation on arguments. */
		char *end;

		if (argc != 3)
			die(0, "Usage: %s path chapter_id", argv[0]);

		path = argv[1];
		if (strlen(path) < 1)
			die(0, "Non-zero path length");

		chapt_id = strtoull(argv[2], &end, 10);
		if (end != argv[2] + strlen(argv[2]))
			die(0, "Invalid chapter ID");
	}

	{ /* Get relevant audio metadata. */
		if ((state.src = open(path, O_RDONLY)) == -1)
			die(errno, "Can't open %s", path);

		if (!read_cfg(state.src, &state.cfg, chapt_id))
			die(0, "Can't get audio config from %s", path);

		seek(state.src, state.cfg.start);
	}

	{ /* Set up Alsa PCM. */
		snd_pcm_hw_params_t *hwparams;
		snd_pcm_sw_params_t *swparams;

		snd_pcm_access_t access;
		snd_pcm_format_t format;
		int dir = 0;

		SND_ERR(== 0, pcm_open, &pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
		state.pcm = pcm;

		snd_pcm_hw_params_alloca(&hwparams);
		snd_pcm_hw_params_any(state.pcm, hwparams);

		switch (state.cfg.bps) {
		case 8:
			access = SND_PCM_ACCESS_MMAP_NONINTERLEAVED;
			format = SND_PCM_FORMAT_S8;
			break;
		case 16:
			access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
			format = SND_PCM_FORMAT_S16;
			break;
		default:
			die(0, "Unrecognized BPS %d", state.cfg.bps);
			break;
		}

		SND_ERR(== 0, pcm_hw_params_set_access, state.pcm, hwparams, access);
		SND_ERR(== 0, pcm_hw_params_set_format, state.pcm, hwparams, format);
		SND_ERR(== 0, pcm_hw_params_set_channels, state.pcm, hwparams, state.cfg.chans);
		SND_ERR(== 0, pcm_hw_params_set_rate, state.pcm, hwparams, state.cfg.rate, dir);
		SND_ERR(== 0, pcm_hw_params, state.pcm, hwparams);

		snd_pcm_sw_params_alloca(&swparams);
		SND_ERR(== 0, pcm_sw_params_current, state.pcm, swparams);
		SND_ERR(== 0, pcm_sw_params_set_period_event, state.pcm, swparams, 1);
		SND_ERR(== 0, pcm_sw_params, state.pcm, swparams);
		
		SND_ERR(== 1, pcm_poll_descriptors_count, state.pcm);
		SND_ERR(== 1, pcm_poll_descriptors, state.pcm, &state.fds[FD_PCM], 1);
		state.fds[FD_PCM].revents = POLLOUT;
	}

	/* Event loop. */

	memset(&state.play, 0, sizeof state.play);
	state.run = true;
	while (state.run
	    && (readyfd_n = poll(state.fds, FD_END,
	                         state.draining ? 10 : -1)) != -1) {
		handle_alsa(&state);
		handle_sigs(&state);
	}

	if (close(state.src) == -1)
		debug(errno, "Can't close %s", path);

	return readyfd_n == -1 ? EXIT_FAILURE : EXIT_SUCCESS;
}
