#include <errno.h> /* errno(3). */
#include <fcntl.h> /* open(2). */
#include <limits.h> /* UINT32_MAX. */
#include <signal.h> /* SIGHUP, SIGUSR1, SIGUSR2. */
#include <stdio.h> /* dprintf(3). */
#include <stdint.h> /* intN_t(3type). */
#include <stdlib.h> /* EXIT_FAILURE, EXIT_SUCCESS, exit(3), strtoul(3). */
#include <string.h> /* strlen(3). */
#include <syslog.h> /* openlog(3), LOG_*. */
#include <unistd.h> /* close(2), read(2). */

#include <pulse/pulseaudio.h>
#include <pulse/mainloop-signal.h>

#undef MAX
#undef MIN

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
#include "replaygain.h"
#include "tags.h"

enum {
	PULSE_LATENCY_US = 50000,
	PULSE_MINREQ_US  = 10000,
};

/* Convenient wrapper for writing decoded audio into a byte buffer. */
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
	struct replaygain rg;
};

/* Mutable playback cursor: where we are in the stream. */
struct playback {
	struct mkv_cursor cursor;
	size_t pending;
	bool done;
};

struct pulse {
	pa_mainloop *ml;
	pa_context *ctx;
	pa_stream *stream;
	pa_operation *drain;
	pa_sample_spec spec;
	bool signals;
	bool reported;
};

struct state {
	struct pulse pulse;
	int src;
	struct playback play;
	struct cfg cfg;
	bool paused;
};

static void *head(struct buf);
static size_t rem(struct buf);

static bool read_cfg(int, struct cfg *, uint64_t);
static bool pulse_open(struct state *);
static bool write_audio(struct state *, size_t);
static void apply_replaygain(struct state *, void *, size_t);
static void convert_s8_to_u8(void *, size_t);
static void report_sink_input(const struct state *);

void cleanup_pulse(struct pulse *);

static void
pulse_quit(struct state *s, int ret)
{
	if (s->pulse.ml)
		pa_mainloop_quit(s->pulse.ml, ret);
}

static void
pulse_fail(struct state *s, const char *what)
{
	debug(0, "%s", what);
	pulse_quit(s, EXIT_FAILURE);
}

static void
pulse_drain_cb(pa_stream *stream, int success, void *userdata)
{
	(void)stream;
	struct state *s = userdata;
	if (!success)
		pulse_fail(s, "pa_stream_drain failed");
	else
		pulse_quit(s, EXIT_SUCCESS);
}

static void
pulse_cork_cb(pa_stream *stream, int success, void *userdata)
{
	(void)stream;
	(void)userdata;
	if (!success)
		debug(0, "pa_stream_cork failed");
}

static void
pulse_cork(struct state *s, bool cork)
{
	if (!s->pulse.stream)
		return;

	pa_operation *op = pa_stream_cork(s->pulse.stream, cork,
	    pulse_cork_cb, s);
	if (op)
		pa_operation_unref(op);
	else
		debug(0, "pa_stream_cork: no Pulse operation");
}

static void
signal_cb(pa_mainloop_api *api, pa_signal_event *e, int sig, void *userdata)
{
	(void)api;
	(void)e;
	struct state *s = userdata;

	switch (sig) {
	case SIGHUP:
		pulse_quit(s, EXIT_SUCCESS);
		break;
	case SIGUSR1:
		s->paused = false;
		pulse_cork(s, false);
		break;
	case SIGUSR2:
		s->paused = true;
		pulse_cork(s, true);
		break;
	default:
		pulse_fail(s, "Invalid signal");
		break;
	}
}

static void
stream_underflow_cb(pa_stream *stream, void *userdata)
{
	(void)userdata;
	int64_t index = pa_stream_get_underflow_index(stream);
	debug(0, "Pulse stream underflow at index %lld", (long long)index);
}

static void
stream_write_cb(pa_stream *stream, size_t nbytes, void *userdata)
{
	(void)stream;
	struct state *s = userdata;
	if (!write_audio(s, nbytes))
		pulse_quit(s, EXIT_FAILURE);
}

static void
stream_state_cb(pa_stream *stream, void *userdata)
{
	(void)stream;
	struct state *s = userdata;

	switch (pa_stream_get_state(s->pulse.stream)) {
	case PA_STREAM_READY:
		if (!s->pulse.reported) {
			report_sink_input(s);
			s->pulse.reported = true;
		}
		if (s->paused)
			pulse_cork(s, true);
		break;
	case PA_STREAM_FAILED:
		[[fallthrough]];
	case PA_STREAM_TERMINATED:
		pulse_fail(s, "Pulse stream failed");
		break;
	default:
		break;
	}
}

static bool
pulse_connect_stream(struct state *s)
{
	struct pulse *pulse = &s->pulse;

	pa_channel_map map;
	if (!pa_channel_map_init_auto(&map, s->cfg.chans, PA_CHANNEL_MAP_DEFAULT)
	    && !pa_channel_map_init_extend(&map, s->cfg.chans,
	    PA_CHANNEL_MAP_DEFAULT))
		return false;

	pa_proplist *props = pa_proplist_new();
	if (!props)
		return false;
	pa_proplist_sets(props, PA_PROP_MEDIA_NAME, "music");
	pa_proplist_sets(props, PA_PROP_MEDIA_ROLE, "music");
	pulse->stream = pa_stream_new_with_proplist(
	    pulse->ctx, "music", &pulse->spec, &map, props);
	pa_proplist_free(props);
	if (!pulse->stream)
		return false;

	pa_stream_set_state_callback(pulse->stream, stream_state_cb, s);
	pa_stream_set_write_callback(pulse->stream, stream_write_cb, s);
	pa_stream_set_underflow_callback(pulse->stream, stream_underflow_cb, s);

	pa_buffer_attr attr = {
		.maxlength = (uint32_t)-1,
		.tlength = (uint32_t)pa_usec_to_bytes(PULSE_LATENCY_US,
		    &pulse->spec),
		.prebuf = (uint32_t)-1,
		.minreq = (uint32_t)pa_usec_to_bytes(PULSE_MINREQ_US,
		    &pulse->spec),
		.fragsize = (uint32_t)-1,
	};
	pa_stream_flags_t flags = PA_STREAM_ADJUST_LATENCY
	    | PA_STREAM_AUTO_TIMING_UPDATE;
	return pa_stream_connect_playback(pulse->stream, NULL, &attr, flags,
	    NULL, NULL) >= 0;
}

static void
context_state_cb(pa_context *ctx, void *userdata)
{
	(void)ctx;
	struct state *s = userdata;

	switch (pa_context_get_state(s->pulse.ctx)) {
	case PA_CONTEXT_READY:
		if (!s->pulse.stream && !pulse_connect_stream(s))
			pulse_fail(s, "Can't open Pulse stream");
		break;
	case PA_CONTEXT_FAILED:
		[[fallthrough]];
	case PA_CONTEXT_TERMINATED:
		pulse_fail(s, "Pulse context failed");
		break;
	default:
		break;
	}
}

static bool
pulse_open(struct state *s)
{
	struct pulse *pulse = &s->pulse;

	if (s->cfg.rate < 1.0 || s->cfg.rate > (double)UINT32_MAX)
		return false;

	pa_sample_format_t format;
	switch (s->cfg.bps) {
	case 8:
		format = PA_SAMPLE_U8;
		break;
	case 16:
		format = PA_SAMPLE_S16NE;
		break;
	default:
		return false;
	}

	pulse->spec = (pa_sample_spec) {
		.format = format,
		.rate = (uint32_t)s->cfg.rate,
		.channels = s->cfg.chans,
	};
	if (!pa_sample_spec_valid(&pulse->spec))
		return false;

	pulse->ml = pa_mainloop_new();
	if (!pulse->ml)
		return false;
	pa_mainloop_api *api = pa_mainloop_get_api(pulse->ml);

	if (pa_signal_init(api) < 0)
		return false;
	pulse->signals = true;
	if (!pa_signal_new(SIGHUP, signal_cb, s)
	    || !pa_signal_new(SIGUSR1, signal_cb, s)
	    || !pa_signal_new(SIGUSR2, signal_cb, s))
		return false;

	pa_proplist *props = pa_proplist_new();
	if (!props)
		return false;
	pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "music");
	pa_proplist_sets(props, PA_PROP_APPLICATION_ID, "music");
	pa_proplist_sets(props, PA_PROP_MEDIA_ROLE, "music");

	pulse->ctx = pa_context_new_with_proplist(
	    api, "music", props);
	pa_proplist_free(props);
	if (!pulse->ctx)
		return false;

	pa_context_set_state_callback(pulse->ctx, context_state_cb, s);
	return pa_context_connect(pulse->ctx, NULL, 0, NULL) >= 0;
}

void
cleanup_pulse(struct pulse *pulse)
{
	if (pulse->drain) {
		pa_operation_cancel(pulse->drain);
		pa_operation_unref(pulse->drain);
	}
	if (pulse->stream) {
		pa_stream_disconnect(pulse->stream);
		pa_stream_unref(pulse->stream);
	}
	if (pulse->ctx) {
		pa_context_disconnect(pulse->ctx);
		pa_context_unref(pulse->ctx);
	}
	if (pulse->ml) {
		if (pulse->signals)
			pa_signal_done();
		pa_mainloop_free(pulse->ml);
	}
}

static bool
write_audio(struct state *s, size_t requested)
{
	if (s->play.done)
		return true;

	size_t writable = pa_stream_writable_size(s->pulse.stream);
	if (writable == (size_t)-1) {
		debug(0, "pa_stream_writable_size failed");
		return false;
	}
	writable = MIN(writable, requested);
	if (writable == 0)
		return true;
	writable -= writable % pa_frame_size(&s->pulse.spec);
	if (writable == 0)
		return true;

	struct buf buf = {
		.addr = malloc(writable),
		.size = writable,
		.n = 0,
	};
	if (!buf.addr) {
		debug(errno, "malloc");
		return false;
	}

	while (rem(buf)) {
		if (s->play.pending) {
			const size_t n = MIN(rem(buf), s->play.pending);
			if (read(s->src, head(buf), n) != (ssize_t) n) {
				debug(errno, "read");
				break;
			}
			apply_replaygain(s, head(buf), n);
			if (s->cfg.bps == 8)
				convert_s8_to_u8(head(buf), n);
			buf.n += n;
			s->play.pending -= n;
			continue;
		}

		const ssize_t sz = mkv_nextframe(s->src, &s->play.cursor,
		    &s->cfg.range);
		if (sz <= 0) {
			s->play.done = true;
			break;
		}

		if (s->play.cursor.leading_skip > 0) {
			seek(s->src, pos(s->src)
			    + (off_t)s->play.cursor.leading_skip);
			s->play.cursor.leading_skip = 0;
		}

		const size_t n = MIN(rem(buf), (size_t)sz);
		if (read(s->src, head(buf), n) != (ssize_t)n) {
			debug(errno, "read");
			break;
		}
		apply_replaygain(s, head(buf), n);
		if (s->cfg.bps == 8)
			convert_s8_to_u8(head(buf), n);
		buf.n += n;
		s->play.pending = (size_t)sz - n;
	}

	if (buf.n > 0) {
		int err = pa_stream_write(s->pulse.stream, buf.addr, buf.n,
		    NULL, 0, PA_SEEK_RELATIVE);
		if (err < 0) {
			free(buf.addr);
			debug(0, "pa_stream_write failed");
			return false;
		}
	}

	free(buf.addr);

	if (s->play.done && !s->pulse.drain) {
		s->pulse.drain = pa_stream_drain(s->pulse.stream,
		    pulse_drain_cb, s);
		if (!s->pulse.drain) {
			debug(0, "pa_stream_drain: no Pulse operation");
			return false;
		}
	}

	return true;
}

static void
apply_replaygain(struct state *s, void *addr, size_t n)
{
	switch (s->cfg.bps) {
	case 8:
		replaygain_apply_s8(addr, n, s->cfg.rg);
		break;
	case 16:
		replaygain_apply_s16(addr, n / sizeof(int16_t), s->cfg.rg);
		break;
	default:
		break;
	}
}

static void
convert_s8_to_u8(void *addr, size_t n)
{
	uint8_t *samples = addr;
	for (size_t i = 0; i < n; i++)
		samples[i] = (uint8_t)(samples[i] + 128);
}

static void
report_sink_input(const struct state *s)
{
	const char *fd_env = getenv("MUSIC_CONTROL_FD");
	if (!fd_env || !fd_env[0])
		return;

	char *end;
	unsigned long fd = strtoul(fd_env, &end, 10);
	if (*end != '\0' || fd > INT_MAX)
		return;

	uint32_t index = pa_stream_get_index(s->pulse.stream);
	if (index == PA_INVALID_INDEX)
		return;

	dprintf((int)fd, "pulse-sink-input %u %u\n", index, s->cfg.chans);
	close((int)fd);
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
	struct track_tags tags;
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

	cfg->range.ts_scale    = info.ts_scale;
	cfg->range.start       = chapter.start;
	cfg->range.end         = chapter.end;
	cfg->range.track       = track.num;
	cfg->range.sample_rate = (uint64_t) track.rate;
	cfg->range.frame_sz    = track.channels * (track.bps / 8);
	cfg->chans             = track.channels;
	cfg->bps               = track.bps;
	cfg->rate              = track.rate;
	cfg->start             = si.segment + start_pos;
	cfg->rg                = (struct replaygain){ .gain = 1.0 };

	if (si.tags) {
		seek(fd, si.segment + si.tags);
		if (mkv_readtracktags(fd, track.uid, &tags) &&
		    tags.has_replaygain_gain)
			cfg->rg = replaygain_from_tags(tags.replaygain_gain_db,
			    tags.replaygain_peak, tags.has_replaygain_peak);
	}

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

	struct state state = {.src = -1};

	char *path;
	uint64_t chapt_id;

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

	if (!pulse_open(&state))
		die(0, "Can't open Pulse stream");

	memset(&state.play, 0, sizeof state.play);
	int ret = EXIT_FAILURE;
	if (pa_mainloop_run(state.pulse.ml, &ret) < 0)
		ret = EXIT_FAILURE;

	if (close(state.src) == -1)
		debug(errno, "Can't close %s", path);
	cleanup_pulse(&state.pulse);

	return ret;
}
