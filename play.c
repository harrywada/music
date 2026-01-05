#include <errno.h> /* errno(3). */
#include <fcntl.h> /* open(2). */
#include <poll.h> /* poll(2). */
#include <signal.h> /* sigaddset(3), sigemptyset(3), sigprocmask(3), sigset_t(3type). */
#include <stdint.h> /* intN_t(3type). */
#include <stdlib.h> /* abs(3), EXIT_FAILURE(3const), EXIT_SUCCESS(3const), exit(3), strtoul(3). */
#include <string.h> /* strlen(3). */
#include <sys/signalfd.h> /* signalfd(2). */

#include <alsa/asoundlib.h>

#include <sys/types.h>
#include "utils.h" /* pos, seek. */

#include <stddef.h>
#include "utils_le.h" /* rev_octets. */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include "ebml.h"

#include <stdint.h>
#include <sys/types.h>
#include "matroska.h"

/* Short helper functions to reduce a bit of redundancy. */
#define SND(cmp, exit, fn, ...) \
	if (!((err = snd_ ## fn (__VA_ARGS__)) cmp)) \
		error(exit, 0, "snd_" #fn ": %s", snd_strerror(err)); \
	if (!(err cmp)) /* Requires -Wno-empty-body. */
#define SND_ERR(cmp, fn, ...) SND(cmp, EXIT_FAILURE, fn, __VA_ARGS__)
#define SND_WARN(cmp, fn, ...) SND(cmp, 0, fn, __VA_ARGS__)

enum fds {
	FD_SRC = 0,
	FD_PCM,
	FD_SIG,
#ifdef MPRIS
	/* TODO. */
#endif
	FD_END,
};

/* Convenient wrapper for accessing `snd_pcm_channel_area_t`s. */
struct buf {
	void *addr;
	size_t n, size;
};

/* Audio configuration. */
struct cfg {
	unsigned int chans, bps, track, ts_scale;
	double rate;
	struct ts_range { unsigned long start, end; } times;
	off_t start;
};

/* For leftover audio frames from reading a cluster into the Alsa buffer. */
struct leftover {
	struct mkv_cluster clust;
	size_t rem;
};

/* Global, messy state. */
struct state {
	snd_pcm_t *pcm;
	struct leftover lo;
	struct cfg cfg;
	struct pollfd fds[FD_END];
	int run;
};

static int err = 0;

/* Get the head address of the buffer. */
static void *head(struct buf);
/* Get the remaining space in the buffer. */
static size_t rem(struct buf);

/* Reads data from the file descriptor into a buffer. */
static ssize_t fill_data(int, struct state *, struct buf *);

/* Extracts necessary configuration data from Matroska data. */
static int read_cfg(int, struct cfg *, unsigned long);
static int find_chapter(int, unsigned long, struct mkv_chapter *);
static int find_track(int, uint64_t [static TRACKS_MAX], struct mkv_track *);
static int find_cue(int, uint64_t, uint64_t, unsigned int, off_t *);

/* Handle file descriptor events. */
static int handle_alsa(struct state *);
static int handle_sigs(struct state *);
static int handle_src(struct state *);

static ssize_t
fill_data(int fd, struct state *s, struct buf *buf)
{
	const off_t begin = pos(fd);
	const size_t pre_n = buf->n;

	if (s->lo.rem) {
		const size_t avail = MIN(rem(*buf), s->lo.rem);
		if (read(fd, head(*buf), avail) != (ssize_t) avail) {
			error(0, errno, "read");
			goto err;
		}
		buf->n += avail;
		s->lo.rem -= avail;
	}

	while (rem(*buf)) switch (ebml_peek(fd)) {
	case MKV_CLUSTER:
		if (ebml_descend(fd, MKV_CLUSTER) == -1) {
			goto err;
		}

		if (!ebml_readuint(fd, MKV_CLUSTERTIMESTAMP, &s->lo.clust.ts)) {
			goto err;
		}

		break;

	case MKV_BLOCKGROUP:
		ebml_descend(fd, MKV_BLOCKGROUP);
		break;

	case MKV_SIMPLEBLOCK:
		/* Fallthrough. */
	case MKV_BLOCK:
		struct mkv_block b;

		if (!mkv_readblock(fd, &b)) {
			goto err;
		}

		if (vint_value(b.track) != s->cfg.track) {
			seek(fd, pos(fd) + b.frames_sz);
			break;
		}

		const unsigned long ts = s->cfg.ts_scale * (s->lo.clust.ts + b.timecode);
		if (ts < s->cfg.times.start) {
			seek(fd, pos(fd) + b.frames_sz);
			break;
		} else if (ts >= s->cfg.times.end) {
			s->run = 0;
			s->fds[FD_PCM].fd = -1 * s->fds[FD_PCM].fd;

			seek(fd, pos(fd) + b.frames_sz);
			goto end;
		}

		if (rem(*buf) < b.frames_sz) {
			s->lo.rem += b.frames_sz - rem(*buf);
		}

		const size_t avail = MIN(rem(*buf), b.frames_sz);
		if (read(fd, head(*buf), avail) != (ssize_t) avail) {
			error(0, errno, "read");
			goto err;
		}
		buf->n += avail;

		break;

	case EBML_ANY_ELEMENT:
		/* Fallthrough. */
	default:
		goto end;
	}

end:
	return buf->n - pre_n;
err:
	seek(fd, begin);
	return -1;
}

static int
handle_alsa(struct state *s)
{
	const snd_pcm_channel_area_t *areas;
	snd_pcm_sframes_t res;
	snd_pcm_uframes_t avail, off;

	struct buf buf; /* TODO struct buf buf[cfg.chans]. */

	unsigned short revents;

	/* Convert revents. */

	SND_WARN(== 0, pcm_poll_descriptors_revents, s->pcm, &s->fds[FD_PCM], 1, &revents) {
		return 1;
	}

	/* Early return conditions. */

	if (revents & ~POLLOUT) {
		error(0, 0, "PCM descriptor errored");
		return 0;
	} else if (!(revents & POLLOUT)) {
		return 1;
	}

	/* Check if audio data is ready. */

	if (s->fds[FD_SRC].fd < 0) {
		s->fds[FD_SRC].fd = abs(s->fds[FD_SRC].fd);
		switch (poll(&s->fds[FD_SRC], 1, 0)) {
		case -1:
			error(0, errno, "poll");
			/* Fallthrough. */
		case  0:
			/* Wait for source. */
			s->fds[FD_PCM].fd = -1 * abs(s->fds[FD_PCM].fd);
			return 1;
		default:
			s->fds[FD_SRC].fd = -1 * abs(s->fds[FD_SRC].fd);
			break;
		}
	}

	if (s->fds[FD_SRC].revents & ~POLLIN) {
		error(0, 0, "Audio file descriptor errored");
		return 0;
	} else if (!(s->fds[FD_SRC].revents & POLLIN)) {
		return 1;
	}

	/* Request memory-mapped frames. */

	SND_WARN(>= 0, pcm_avail_update, s->pcm) {
		return 1;
	}
	avail = err;

	SND_WARN(== 0, pcm_mmap_begin, s->pcm, &areas, &off, &avail) {
		return 1;
	}

	buf.size = snd_pcm_frames_to_bytes(s->pcm, avail);
	buf.addr = areas[0].addr + (areas[0].first / 8)
	         + off * (areas[0].step / 8);
	buf.n = 0;

	/* Read data. */

	if (fill_data(abs(s->fds[FD_SRC].fd), s, &buf) == -1) {
		error(0, 0, "Can't read audio data");
	}

	/* Commit data. */

	/* TODO Maybe avoid state, use `snd_pcm_mmap_commit_partial` instead? */
	res = snd_pcm_mmap_commit(s->pcm, off, snd_pcm_bytes_to_frames(s->pcm, buf.n));
	if (res != snd_pcm_bytes_to_frames(s->pcm, buf.n))
		error(EXIT_FAILURE, 0, "snd_pcm_mmap_commit: %s", snd_strerror(res));

	/* Autoplay doesn't work with mmap, for some reason.
	   https://github.com/alsa-project/alsa-lib/commit/bd53892 */
	if (snd_pcm_state(s->pcm) == SND_PCM_STATE_PREPARED) {
		SND_WARN(== 0, pcm_start, s->pcm);
	}
	
	return 1;
}

static int
handle_sigs(struct state *s)
{
	struct signalfd_siginfo siginfo;

	/* Early return conditions. */

	if (s->fds[FD_SIG].revents & ~POLLIN) {
		error(0, 0, "Signal descriptor errored");
		return 0;
	} else if (!(s->fds[FD_SIG].revents & POLLIN)) {
		return 1;
	}

	/* Read signal info. */

	if (read(s->fds[FD_SIG].fd, &siginfo, sizeof siginfo) != sizeof siginfo) {
		error(0, errno, "read");
		return 1;
	}

	/* Handle signals. */

	switch (siginfo.ssi_signo) {
	case SIGHUP:
		s->run = 0;
		SND_WARN(== 0, pcm_drop, s->pcm);
		return 0;
	case SIGUSR1:
		SND_WARN(== 0, pcm_pause, s->pcm, 0);
		break;
	case SIGUSR2:
		SND_WARN(== 0, pcm_pause, s->pcm, 1);
		break;
	default:
		error(0, 0, "Invalid signal %d", siginfo.ssi_signo);
		return 0;
	}

	return 1;
}

static int
handle_src(struct state *s)
{
	if (s->fds[FD_SRC].fd > 0 && s->fds[FD_SRC].revents & POLLIN) {
		/* The descriptor can only be positive if the PCM is waiting. */
		s->fds[FD_PCM].fd = abs(s->fds[FD_PCM].fd);
		memset(&s->fds[FD_PCM].revents, 0, sizeof s->fds[FD_PCM].revents);
	}

	return 1;
}

static void *
head(struct buf buf)
{
	return buf.addr + buf.n;
}

static int
read_cfg(int fd, struct cfg *cfg, unsigned long id)
{
	struct mkv_seekinfo si;
	struct mkv_info info;
	struct mkv_chapter chapter;
	struct mkv_track track;
	off_t start_pos;

	memset(cfg, 0, sizeof *cfg);

	if (ebml_skip(fd, EBML_HEADER) == -1) {
		return 0;
	}
	if (ebml_descend(fd, MKV_SEGMENT) == -1) {
		return 0;
	}

	if (!mkv_readseekinfo(fd, &si)) {
		return 0;
	}

	seek(fd, si.segment + si.info);
	if (!mkv_readinfo(fd, &info)) {
		return 0;
	}

	seek(fd, si.segment + si.chapters);
	if (!find_chapter(fd, id, &chapter)) {
		return 0;
	}

	seek(fd, si.segment + si.tracks);
	if (!find_track(fd, chapter.track_uids, &track)) {
		return 0;
	}

	seek(fd, si.segment + si.cues);
	if (!find_cue(fd, chapter.start, info.ts_scale, track.num, &start_pos)) {
		return 0;
	}

	cfg->ts_scale = info.ts_scale;
	cfg->times.start = chapter.start;
	cfg->times.end = chapter.end;
	cfg->chans = track.channels;
	cfg->bps = track.bps;
	cfg->track = track.num;
	cfg->rate = track.rate;
	cfg->start = si.segment + start_pos;

	return 1;
}

static int
find_chapter(int fd, unsigned long id, struct mkv_chapter *c)
{
	if (ebml_descend(fd, MKV_CHAPTERS) == -1) {
		return 0;
	}

	for (;;) { /* Find relevant chapter. */
		off_t end;

		if ((end = ebml_descend(fd, MKV_EDITIONENTRY)) == -1)
			return 0;
		while (pos(fd) < end) {
			if (ebml_peek(fd) != MKV_CHAPTERATOM) {
				ebml_skip(fd, EBML_ANY_ELEMENT);
				continue;
			}
			if (!mkv_readchapteratom(fd, c))
				return 0; /* Should never happen. */
			if (c->uid == id)
				return 1;
		}
	}
}

static int
find_track(int fd, uint64_t uids[static TRACKS_MAX], struct mkv_track *t)
{
	if (ebml_descend(fd, MKV_TRACKS) == -1)
		return 0;
	for (;;) { /* Find relevant track. */
		int i;

		if (!mkv_readtrackentry(fd, t))
			return 0;
		if (!t->enabled || t->type != AUDIO)
			continue;
		for (i = 0; i < TRACKS_MAX; i += 1) {
			if (t->uid == uids[i])
				return 1;
		}
	}
}

static int
find_cue(int fd, uint64_t start, uint64_t scale, unsigned int track, off_t *pos)
{
	struct mkv_cue cue1, cue2;

	if (ebml_descend(fd, MKV_CUES) == -1)
		return 0;

	while (mkv_readcuepoint(fd, &cue2)) {
		int i;

		if (cue2.time * scale <= start) {
			cue1 = cue2;
			continue;
		}

		for (i = 0; i < TRACKS_MAX; i += 1) {
			if (cue1.tracks[i].num == track) {
				/* XXX Ignore relative position. */
				*pos = cue1.tracks[i].pos;
				return 1;
			}
		}
	}

	return 0;
}

static size_t
rem(struct buf buf)
{
	return buf.size - buf.n;
}

int
main(int argc, char *argv[])
{
	struct state state;

	char *path;
	unsigned long chapt_id;

	int readyfd_n;

	{ /* Before anything, register signal handlers. */
		sigset_t sigmask;

		sigemptyset(&sigmask);
		sigaddset(&sigmask, SIGUSR1);
		sigaddset(&sigmask, SIGUSR2);
		sigaddset(&sigmask, SIGHUP);

		if (sigprocmask(SIG_BLOCK, &sigmask, NULL) == -1)
			error(EXIT_FAILURE, errno, "sigprocmask");
		if ((state.fds[FD_SIG].fd = signalfd(-1, &sigmask, 0)) == -1)
			error(EXIT_FAILURE, errno, "signalfd");
		state.fds[FD_SIG].events = POLLIN;
	}

	{ /* Assign and do some basic validation on arguments. */
		char *end;

		if (argc != 3)
			error(EXIT_FAILURE, 0, "Usage: %s path chapter_id", argv[0]);

		path = argv[1];
		if (strlen(path) < 1)
			error(EXIT_FAILURE, 0, "Non-zero path length");

		chapt_id = strtoul(argv[2], &end, 10);
		if (end != argv[2] + strlen(argv[2]))
			error(EXIT_FAILURE, 0, "Invalid chapter ID");
	}

	{ /* Get relevant audio metadata. */
		if ((state.fds[FD_SRC].fd = open(path, O_RDONLY)) == -1)
			error(EXIT_FAILURE, errno, "Can'track open %s", path);
		state.fds[FD_SRC].events = POLLIN;

		if (!read_cfg(state.fds[FD_SRC].fd, &state.cfg, chapt_id))
			error(EXIT_FAILURE, 0, "Can't get audio config from %s", path);

		seek(state.fds[FD_SRC].fd, state.cfg.start);

		/* Should mostly be available, so by default don't poll it. */
		state.fds[FD_SRC].fd = -1 * abs(state.fds[FD_SRC].fd);
	}

	{ /* Set up Alsa PCM. */
		snd_pcm_hw_params_t *hwparams;
		snd_pcm_sw_params_t *swparams;

		snd_pcm_access_t access;
		snd_pcm_format_t format;
		int dir = 0;

		SND_ERR(== 0, pcm_open, &state.pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);

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
			error(EXIT_FAILURE, 0, "Unrecognized BPS %d", state.cfg.bps);
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

	memset(&state.lo, 0, sizeof state.lo);
	state.run = 1;
	while (state.run && (readyfd_n = (poll(state.fds, FD_END, -1)) != -1)) {
		handle_alsa(&state);
		handle_sigs(&state);
		handle_src(&state);
	}

	/* Clean up. */

	/* XXX Locks up process, should be brief on low period sizes. */
	SND_WARN(== 0, pcm_drain, state.pcm);

	if (close(abs(state.fds[FD_SRC].fd)) == -1)
		error(0, errno, "Can'track close %s", path);
	if (close(abs(state.fds[FD_PCM].fd)) == -1)
		error(0, errno, "Can'track close Alsa PCM");
	if (close(abs(state.fds[FD_SIG].fd)) == -1)
		error(0, errno, "Can'track close signal descriptor");

	exit(readyfd_n == -1 ? EXIT_FAILURE : EXIT_SUCCESS);
}
