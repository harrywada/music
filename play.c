#include <alloca.h> /* snd_pcm_hw_params_alloca. */
#include <errno.h> /* errno(3). */
#include <fcntl.h> /* open(2). */
#include <poll.h> /* poll(2). */
#include <signal.h> /* sigaddset(3), sigemptyset(3), sigprocmask(3), sigset_t(3type). */
#include <stdint.h> /* intN_t(3type). */
#include <stdlib.h> /* EXIT_FAILURE(3const), EXIT_SUCCESS(3const), exit(3), strtoul(3). */
#include <string.h> /* strlen(3). */
#include <sys/signalfd.h> /* signalfd(2). */

#include <alsa/asoundlib.h>

#include "utils.h"

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
	uint32_t track;
	uint64_t ts_scale;
	double   rate;
	struct { uint64_t start, end; } times;
	off_t start;
};

/* Mutable playback cursor: where we are in the stream. */
struct playback {
	struct mkv_cluster cluster;
	size_t pending;  /* bytes from last block not yet committed */
	bool done;       /* chapter end reached */
};

struct state {
	snd_pcm_t     *pcm;
	int            src;
	struct playback play;
	struct cfg     cfg;
	struct pollfd *fds;
	bool           run;
};

static int err = 0;

/* Get the head address of the buffer. */
[[gnu::const]]
static void *head(struct buf);
/* Get the remaining space in the buffer. */
[[gnu::const]]
static size_t rem(struct buf);

/* Advances to the next frame for the configured track within the chapter.
   Returns frame size (fd positioned at data), 0 at chapter end, -1 on error. */
[[nodiscard, gnu::fd_arg_read(1)]]
static ssize_t next_frame(int, struct playback *, const struct cfg *);

/* Configuration parsing functions. */

/* Extracts necessary configuration data from Matroska data. */
[[gnu::cold, gnu::fd_arg_read(1)]]
static bool read_cfg(int, struct cfg *, uint64_t);

/* Finds a chapter with a given UID. */
[[gnu::cold, gnu::fd_arg_read(1)]]
static bool find_chapter(int, uint64_t, struct mkv_chapter *);

/* Finds a track with ... TODO should only be one track; filter out non-audio ones beforehand. */
[[gnu::cold, gnu::fd_arg_read(1)]]
static bool find_track(int, uint64_t [static TRACKS_MAX], struct mkv_track *);

/* From the start time, use cues to find the position of the first chapter. */
[[gnu::cold, gnu::fd_arg_read(1)]]
static bool find_cue(int, uint64_t, uint64_t, unsigned int, off_t *);

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
	/* XXX Locks up process, should be brief on low period sizes. */
	snd_pcm_drain(*pcm);
	snd_pcm_close(*pcm);
	*pcm = NULL;
}

static ssize_t
next_frame(int fd, struct playback *play, const struct cfg *cfg)
{
	const off_t begin = pos(fd);

	for (;;) switch (ebml_peek(fd)) {
	case MKV_CLUSTER:
		if (ebml_descend(fd, MKV_CLUSTER) == -1)
			goto err;
		if (!ebml_readuint(fd, MKV_CLUSTERTIMESTAMP, &play->cluster.ts))
			goto err;
		break;

	case MKV_BLOCKGROUP:
		ebml_descend(fd, MKV_BLOCKGROUP);
		break;

	case MKV_SIMPLEBLOCK:
		[[fallthrough]];
	case MKV_BLOCK: {
		struct mkv_block b;

		if (!mkv_readblock(fd, &b))
			goto err;

		if (vint_value(b.track) != cfg->track) {
			seek(fd, pos(fd) + b.frames_sz);
			break;
		}

		const uint64_t ts = cfg->ts_scale * (play->cluster.ts + b.timecode);
		if (ts < cfg->times.start) {
			seek(fd, pos(fd) + b.frames_sz);
			break;
		}
		if (cfg->times.end && ts >= cfg->times.end) {
			seek(fd, pos(fd) + b.frames_sz);
			return 0;
		}

		return (ssize_t) b.frames_sz;
	}

	case 0:
		[[fallthrough]];
	default:
		return -1;
	}

err:
	seek(fd, begin);
	return -1;
}

static bool
handle_alsa(struct state *s)
{
	const snd_pcm_channel_area_t *areas;
	snd_pcm_sframes_t res;
	snd_pcm_uframes_t avail, off;

	struct buf buf; /* TODO struct buf buf[cfg.chans]. */

	unsigned short revents;

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

		const ssize_t sz = next_frame(s->src, &s->play, &s->cfg);
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

	if (s->play.done) {
		snd_pcm_drain(s->pcm);
		s->run = false;
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
	if (!find_chapter(fd, id, &chapter)) {
		return false;
	}

	seek(fd, si.segment + si.tracks);
	if (!find_track(fd, chapter.track_uids, &track)) {
		return false;
	}

	seek(fd, si.segment + si.cues);
	if (!find_cue(fd, chapter.start, info.ts_scale, track.num, &start_pos)) {
		return false;
	}

	cfg->ts_scale = info.ts_scale;
	cfg->times.start = chapter.start;
	cfg->times.end = chapter.end;
	cfg->chans = track.channels;
	cfg->bps = track.bps;
	cfg->track = track.num;
	cfg->rate = track.rate;
	cfg->start = si.segment + start_pos;

	return true;
}

static bool
find_chapter(int fd, uint64_t id, struct mkv_chapter *c)
{
	if (ebml_descend(fd, MKV_CHAPTERS) == -1) {
		return false;
	}

	for (;;) { /* Find relevant chapter. */
		off_t end;

		if ((end = ebml_descend(fd, MKV_EDITIONENTRY)) == -1)
			return false;
		while (pos(fd) < end) {
			if (ebml_peek(fd) != MKV_CHAPTERATOM) {
				ebml_skip(fd, EBML_ANY_ELEMENT);
				continue;
			}
			if (!mkv_readchapteratom(fd, c))
				return false; /* Should never happen. */
			if (c->uid == id)
				return true;
		}
	}
}

static bool
find_track(int fd, uint64_t uids[static TRACKS_MAX], struct mkv_track *t)
{
	if (ebml_descend(fd, MKV_TRACKS) == -1)
		return false;
	for (;;) { /* Find relevant track. */
		if (!mkv_readtrackentry(fd, t))
			return false;
		if (!t->enabled || t->type != AUDIO)
			continue;
		for (int i = 0; i < TRACKS_MAX; i += 1) {
			if (t->uid == uids[i])
				return true;
		}
	}
}

static bool
find_cue(int fd, uint64_t start, uint64_t scale, unsigned int track, off_t *pos)
{
	struct mkv_cue cue = {0}, next;
	bool found = false;

	if (ebml_descend(fd, MKV_CUES) == -1)
		return false;

	while (mkv_readcuepoint(fd, &next)) {
		if (next.time * scale > start)
			break;
		cue = next;
		found = true;
	}

	if (!found)
		return false;

	for (int i = 0; i < TRACKS_MAX; i += 1) {
		if (cue.tracks[i].num == track) {
			/* XXX Ignore relative position. */
			*pos = cue.tracks[i].pos;
			return true;
		}
	}

	return false;
}

static size_t
rem(struct buf buf)
{
	return buf.size - buf.n;
}

int
main(int argc, char *argv[])
{
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
	while (state.run && (readyfd_n = poll(state.fds, FD_END, -1)) != -1) {
		handle_alsa(&state);
		handle_sigs(&state);
	}

	if (close(state.src) == -1)
		debug(errno, "Can't close %s", path);

	return readyfd_n == -1 ? EXIT_FAILURE : EXIT_SUCCESS;
}
