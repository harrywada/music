#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include "cmds.h"
#include "ebml.h"
#include "matroska.h"
#include "matroska_utils.h"
#include "tags.h"
#include "mpris.h"
#include "queue.h"
#include "song.h"
#include "utils.h"

#define MPRIS_OBJECT  "/org/mpris/MediaPlayer2"
#define MPRIS_ROOT    "org.mpris.MediaPlayer2"
#define MPRIS_PLAYER  "org.mpris.MediaPlayer2.Player"
#define MPRIS_NAME    "org.mpris.MediaPlayer2.music"
#define MPRIS_NOTRACK "/org/mpris/MediaPlayer2/TrackList/NoTrack"
#define MPRIS_TRACK   "/org/mpris/MediaPlayer2/Track"

struct mpris {
	sd_bus          *bus;
	struct state     state;    /* snapshot of the last committed state */
	struct state    *dispatch; /* non-NULL only inside mpris_process() */
	struct song_tags tags;     /* Matroska tags for the current song */
	bool             have_tags;
	unsigned long    cur_uid;  /* uid whose tags are currently cached */
	char            *art_path; /* absolute path to extracted cover art, or NULL */
};

/* Forward declarations. */
static const sd_bus_vtable mpris_root_vtable[];
static const sd_bus_vtable mpris_player_vtable[];

/* ------------------------------------------------------------------ */
/* Matroska tag helpers                                                 */

static int
open_mkv(const char *path, struct mkv_seekinfo *si)
{
	int fd = open(path, O_RDONLY);
	if (fd == -1) {
		debug(errno, "mpris: %s", path);
		return -1;
	}
	*si = (struct mkv_seekinfo){0};
	if (ebml_skip(fd, EBML_HEADER) == -1
	||  ebml_descend(fd, MKV_SEGMENT) == -1
	||  !mkv_readseekinfo(fd, si)) {
		debug(0, "mpris: can't read Matroska headers in %s", path);
		close(fd);
		return -1;
	}
	return fd;
}

static void
cleanup_art(struct mpris *m)
{
	if (!m->art_path)
		return;
	if (unlink(m->art_path) == -1 && errno != ENOENT)
		debug(errno, "mpris: unlink %s", m->art_path);
	free(m->art_path);
	m->art_path = NULL;
}

static void
refresh_tags(struct mpris *m, const struct song *s)
{
	cleanup_art(m);

	if (m->have_tags) {
		song_tags_free(&m->tags);
		m->have_tags = false;
	}

	struct mkv_seekinfo si;
	int fd = open_mkv(s->path, &si);
	if (fd == -1)
		return;

	if (si.tags) {
		struct mkv_chapter chapter = {0};
		struct mkv_track track = {0};

		if (si.chapters) {
			seek(fd, si.segment + si.chapters);
			mkv_findchapter(fd, (uint64_t)s->uid, &chapter);
		}
		if (si.tracks && chapter.uid) {
			seek(fd, si.segment + si.tracks);
			mkv_findtrack(fd, chapter.track_uids, &track);
		}

		seek(fd, si.segment + si.tags);
		m->tags = (struct song_tags){0};
		mkv_readsongtags(fd, (uint64_t)s->uid, track.uid,
		                 chapter.edition_uid, &m->tags);
		m->have_tags = true;
		m->cur_uid   = s->uid;
	}

	if (si.attachments) {
		seek(fd, si.segment + si.attachments);
		struct mkv_attachment att;
		if (mkv_findcoverart(fd, &att)) {
			/* Reject filenames with path separators (path traversal). */
			if (strchr(att.filename, '/'))
				goto done;

			char dir[PATH_MAX];
			const char *xdg = getenv("XDG_CACHE_HOME");
			if (xdg && xdg[0]) {
				if (snprintf(dir, sizeof dir, "%s/music", xdg)
				    >= (int)sizeof dir)
					goto done;
			} else {
				const char *home = getenv("HOME");
				if (!home || !home[0])
					goto done;
				if (snprintf(dir, sizeof dir, "%s/.cache/music", home)
				    >= (int)sizeof dir)
					goto done;
			}

			if (mkdir(dir, 0700) == -1 && errno != EEXIST) {
				debug(errno, "mpris: mkdir %s", dir);
				goto done;
			}

			char path[PATH_MAX];
			if (snprintf(path, sizeof path, "%s/%s", dir, att.filename)
			    >= (int)sizeof path)
				goto done;

			int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
			if (out == -1) {
				debug(errno, "mpris: open %s", path);
				goto done;
			}

			seek(fd, att.data_off);
			char buf[8192];
			size_t remaining = att.data_sz;
			bool ok = true;
			while (remaining > 0) {
				size_t chunk = remaining < sizeof buf
				             ? remaining : sizeof buf;
				ssize_t nr = read(fd, buf, chunk);
				if (nr <= 0) { ok = false; break; }
				if (write(out, buf, (size_t)nr) != nr)
					{ ok = false; break; }
				remaining -= (size_t)nr;
			}
			close(out);

			if (ok) {
				m->art_path = strdup(path);
				if (!m->art_path)
					debug(errno, "mpris: strdup art_path");
			} else {
				unlink(path);
			}
		}
	}

done:
	close(fd);
}

/* ------------------------------------------------------------------ */
/* sd-bus dict entry helpers                                            */

static int
append_dict_string(sd_bus_message *m, const char *key, const char *val)
{
	int r;
	r = sd_bus_message_open_container(m, 'e', "sv");  if (r < 0) return r;
	r = sd_bus_message_append_basic(m, 's', key);     if (r < 0) return r;
	r = sd_bus_message_open_container(m, 'v', "s");   if (r < 0) return r;
	r = sd_bus_message_append_basic(m, 's', val);     if (r < 0) return r;
	r = sd_bus_message_close_container(m);            if (r < 0) return r;
	return sd_bus_message_close_container(m);
}

static int
append_dict_objpath(sd_bus_message *m, const char *key, const char *val)
{
	int r;
	r = sd_bus_message_open_container(m, 'e', "sv");  if (r < 0) return r;
	r = sd_bus_message_append_basic(m, 's', key);     if (r < 0) return r;
	r = sd_bus_message_open_container(m, 'v', "o");   if (r < 0) return r;
	r = sd_bus_message_append_basic(m, 'o', val);     if (r < 0) return r;
	r = sd_bus_message_close_container(m);            if (r < 0) return r;
	return sd_bus_message_close_container(m);
}

static int
append_dict_int32(sd_bus_message *m, const char *key, int32_t val)
{
	int r;
	r = sd_bus_message_open_container(m, 'e', "sv");  if (r < 0) return r;
	r = sd_bus_message_append_basic(m, 's', key);     if (r < 0) return r;
	r = sd_bus_message_open_container(m, 'v', "i");   if (r < 0) return r;
	r = sd_bus_message_append_basic(m, 'i', &val);    if (r < 0) return r;
	r = sd_bus_message_close_container(m);            if (r < 0) return r;
	return sd_bus_message_close_container(m);
}

static int
append_dict_strv(sd_bus_message *m, const char *key,
                 const struct tag_values *tv)
{
	int r;
	r = sd_bus_message_open_container(m, 'e', "sv");  if (r < 0) return r;
	r = sd_bus_message_append_basic(m, 's', key);     if (r < 0) return r;
	r = sd_bus_message_open_container(m, 'v', "as");  if (r < 0) return r;
	r = sd_bus_message_open_container(m, 'a', "s");   if (r < 0) return r;
	for (size_t i = 0; i < tv->count; i++) {
		r = sd_bus_message_append_basic(m, 's', tv->vals[i]);
		if (r < 0) return r;
	}
	r = sd_bus_message_close_container(m);            if (r < 0) return r;
	r = sd_bus_message_close_container(m);            if (r < 0) return r;
	return sd_bus_message_close_container(m);
}

/* ------------------------------------------------------------------ */
/* Root interface properties and methods                                */

static int
prop_bool_true(sd_bus *, const char *, const char *,
               const char *, sd_bus_message *reply,
               void *, sd_bus_error *)
{
	return sd_bus_message_append(reply, "b", 1);
}

static int
prop_bool_false(sd_bus *, const char *, const char *,
                const char *, sd_bus_message *reply,
                void *, sd_bus_error *)
{
	return sd_bus_message_append(reply, "b", 0);
}

static int
prop_identity(sd_bus *, const char *, const char *,
              const char *, sd_bus_message *reply,
              void *, sd_bus_error *)
{
	return sd_bus_message_append(reply, "s", "music");
}

static int
prop_empty_strv(sd_bus *, const char *, const char *,
                const char *, sd_bus_message *reply,
                void *, sd_bus_error *)
{
	int r = sd_bus_message_open_container(reply, 'a', "s");
	if (r < 0) return r;
	return sd_bus_message_close_container(reply);
}

static int
method_noop(sd_bus_message *msg, void *, sd_bus_error *)
{
	return sd_bus_reply_method_return(msg, "");
}

static int
method_quit(sd_bus_message *msg, void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	if (m->dispatch)
		*m->dispatch = cmd_exit(*m->dispatch, 0, nullptr);
	return sd_bus_reply_method_return(msg, "");
}

/* ------------------------------------------------------------------ */
/* Player interface properties and methods                              */

static int
prop_rate_one(sd_bus *, const char *, const char *,
              const char *, sd_bus_message *reply,
              void *, sd_bus_error *)
{
	return sd_bus_message_append(reply, "d", (double)1.0);
}

static int
prop_volume(sd_bus *, const char *, const char *,
            const char *, sd_bus_message *reply,
            void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	return sd_bus_message_append(reply, "d", m->state.volume / 100.0);
}

static int
set_volume(sd_bus *, const char *, const char *,
           const char *, sd_bus_message *value,
           void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	if (!m->dispatch)
		return 0;

	double v;
	int r = sd_bus_message_read(value, "d", &v);
	if (r < 0)
		return r;
	if (!(v >= 0.0))
		v = 0.0;
	if (v > 1.0)
		v = 1.0;

	m->dispatch->volume = (unsigned int)(v * 100.0 + 0.000000001);
	return 0;
}

static int
prop_position_zero(sd_bus *, const char *, const char *,
                   const char *, sd_bus_message *reply,
                   void *, sd_bus_error *)
{
	return sd_bus_message_append(reply, "x", (int64_t)0);
}

static int
prop_playback_status(sd_bus *, const char *, const char *,
                     const char *, sd_bus_message *reply,
                     void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	const char *status;
	switch (m->state.play) {
	case PLAYING: status = "Playing"; break;
	case PAUSED:  status = "Paused";  break;
	default:      status = "Stopped"; break;
	}
	return sd_bus_message_append(reply, "s", status);
}

static int
prop_loop_status(sd_bus *, const char *, const char *,
                 const char *, sd_bus_message *reply,
                 void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	const char *ls = (m->state.mode == LOOP) ? "Playlist" : "None";
	return sd_bus_message_append(reply, "s", ls);
}

static int
set_loop_status(sd_bus *, const char *, const char *,
                const char *, sd_bus_message *value,
                void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	if (!m->dispatch)
		return 0;
	const char *s;
	int r = sd_bus_message_read(value, "s", &s);
	if (r < 0) return r;
	if (strcmp(s, "Playlist") == 0 || strcmp(s, "Track") == 0)
		*m->dispatch = cmd_loop(*m->dispatch, 0, nullptr);
	else
		*m->dispatch = cmd_consume(*m->dispatch, 0, nullptr);
	return 0;
}

static int
prop_can_go_next(sd_bus *, const char *, const char *,
                 const char *, sd_bus_message *reply,
                 void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	return sd_bus_message_append(reply, "b",
	    qsize(m->state.queue) > 1 ? 1 : 0);
}

static int
prop_can_play(sd_bus *, const char *, const char *,
              const char *, sd_bus_message *reply,
              void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	return sd_bus_message_append(reply, "b",
	    qsize(m->state.queue) > 0 ? 1 : 0);
}

static int
prop_can_pause(sd_bus *, const char *, const char *,
               const char *, sd_bus_message *reply,
               void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	return sd_bus_message_append(reply, "b",
	    m->state.play == PLAYING ? 1 : 0);
}

static int
prop_metadata(sd_bus *, const char *, const char *,
              const char *, sd_bus_message *reply,
              void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	int r;

	r = sd_bus_message_open_container(reply, 'a', "{sv}");
	if (r < 0) return r;

	if (!qsize(m->state.queue)) {
		r = append_dict_objpath(reply, "mpris:trackid", MPRIS_NOTRACK);
		if (r < 0) return r;
	} else {
		const struct song *s = &m->state.queue.data[
		    m->state.queue.head % m->state.queue.size];

		char trackid[64];
		snprintf(trackid, sizeof trackid,
		         MPRIS_TRACK "/%lu", s->uid);

		r = append_dict_objpath(reply, "mpris:trackid", trackid);
		if (r < 0) return r;
		r = append_dict_string(reply, "xesam:url", s->path);
		if (r < 0) return r;

		if (m->have_tags) {
			const struct tag_values *tv;

			tv = &m->tags.fields[TAG_TITLE];
			if (tv->count > 0) {
				r = append_dict_string(reply, "xesam:title",
				                       tv->vals[0]);
				if (r < 0) return r;
			}

			tv = &m->tags.fields[TAG_ARTIST];
			if (tv->count > 0) {
				r = append_dict_strv(reply, "xesam:artist", tv);
				if (r < 0) return r;
			}

			tv = &m->tags.fields[TAG_ALBUM];
			if (tv->count > 0) {
				r = append_dict_string(reply, "xesam:album",
				                       tv->vals[0]);
				if (r < 0) return r;
			}

			tv = &m->tags.fields[TAG_TRACK];
			if (tv->count > 0) {
				int32_t tn = (int32_t)strtol(tv->vals[0],
				                             nullptr, 10);
				r = append_dict_int32(reply,
				                      "xesam:trackNumber", tn);
				if (r < 0) return r;
			}

			{
				int32_t dn = 1;
				tv = &m->tags.fields[TAG_DISC];
				if (tv->count > 0)
					dn = (int32_t)strtol(tv->vals[0],
					                     nullptr, 10);
				r = append_dict_int32(reply,
				                      "xesam:discNumber", dn);
				if (r < 0) return r;
			}

			tv = &m->tags.fields[TAG_GENRE];
			if (tv->count > 0) {
				r = append_dict_strv(reply, "xesam:genre", tv);
				if (r < 0) return r;
			}

			tv = &m->tags.fields[TAG_DATE];
			if (tv->count > 0) {
				r = append_dict_string(reply,
				                       "xesam:contentCreated",
				                       tv->vals[0]);
				if (r < 0) return r;
			}
		}

		if (m->art_path) {
			char uri[sizeof("file://") + PATH_MAX];
			snprintf(uri, sizeof uri, "file://%s", m->art_path);
			r = append_dict_string(reply, "mpris:artUrl", uri);
			if (r < 0) return r;
		}
	}

	return sd_bus_message_close_container(reply);
}

static int
method_next(sd_bus_message *msg, void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	if (m->dispatch && qsize(m->dispatch->queue) > 0)
		*m->dispatch = cmd_skip(*m->dispatch, 0, nullptr);
	return sd_bus_reply_method_return(msg, "");
}

static int
method_pause(sd_bus_message *msg, void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	if (m->dispatch)
		*m->dispatch = cmd_pause(*m->dispatch, 0, nullptr);
	return sd_bus_reply_method_return(msg, "");
}

static int
method_play_pause(sd_bus_message *msg, void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	if (m->dispatch)
		*m->dispatch = cmd_toggle(*m->dispatch, 0, nullptr);
	return sd_bus_reply_method_return(msg, "");
}

static int
method_stop(sd_bus_message *msg, void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	if (m->dispatch)
		*m->dispatch = cmd_stop(*m->dispatch, 0, nullptr);
	return sd_bus_reply_method_return(msg, "");
}

static int
method_play(sd_bus_message *msg, void *userdata, sd_bus_error *)
{
	struct mpris *m = userdata;
	if (m->dispatch)
		*m->dispatch = cmd_play(*m->dispatch, 0, nullptr);
	return sd_bus_reply_method_return(msg, "");
}

/* ------------------------------------------------------------------ */
/* Vtables                                                              */

static const sd_bus_vtable mpris_root_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("CanQuit",    "b",  prop_bool_true,   0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("CanRaise",   "b",  prop_bool_false,  0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("HasTrackList", "b", prop_bool_false, 0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Identity",   "s",  prop_identity,    0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("SupportedUriSchemes", "as", prop_empty_strv, 0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("SupportedMimeTypes",  "as", prop_empty_strv, 0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_METHOD("Raise", "", "", method_noop, 0),
	SD_BUS_METHOD("Quit",  "", "", method_quit, 0),
	SD_BUS_VTABLE_END,
};

static const sd_bus_vtable mpris_player_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_PROPERTY("PlaybackStatus", "s",
	    prop_playback_status, 0, 0),
	SD_BUS_WRITABLE_PROPERTY("LoopStatus", "s",
	    prop_loop_status, set_loop_status, 0, 0),
	SD_BUS_PROPERTY("Rate",        "d", prop_rate_one,    0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Shuffle",     "b", prop_bool_false,  0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("Metadata",    "a{sv}", prop_metadata, 0, 0),
	SD_BUS_WRITABLE_PROPERTY("Volume", "d",
	    prop_volume, set_volume, 0, 0),
	SD_BUS_PROPERTY("Position",    "x", prop_position_zero, 0, 0),
	SD_BUS_PROPERTY("MinimumRate", "d", prop_rate_one,    0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("MaximumRate", "d", prop_rate_one,    0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("CanGoNext",     "b", prop_can_go_next, 0, 0),
	SD_BUS_PROPERTY("CanGoPrevious", "b", prop_bool_false,  0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("CanPlay",  "b", prop_can_play,  0, 0),
	SD_BUS_PROPERTY("CanPause", "b", prop_can_pause, 0, 0),
	SD_BUS_PROPERTY("CanSeek",    "b", prop_bool_false, 0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("CanControl", "b", prop_bool_true,  0,
	    SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_METHOD("Next",        "", "",  method_next,       0),
	SD_BUS_METHOD("Previous",    "", "",  method_noop,       0),
	SD_BUS_METHOD("Pause",       "", "",  method_pause,      0),
	SD_BUS_METHOD("PlayPause",   "", "",  method_play_pause, 0),
	SD_BUS_METHOD("Stop",        "", "",  method_stop,       0),
	SD_BUS_METHOD("Play",        "", "",  method_play,       0),
	SD_BUS_METHOD("Seek",        "x",  "", method_noop,      0),
	SD_BUS_METHOD("SetPosition", "ox", "", method_noop,      0),
	SD_BUS_METHOD("OpenUri",     "s",  "", method_noop,      0),
	SD_BUS_VTABLE_END,
};

/* ------------------------------------------------------------------ */
/* Public API                                                           */

struct mpris *
mpris_open(struct state initial)
{
	struct mpris *m = calloc(1, sizeof *m);
	if (!m) {
		debug(errno, "mpris_open");
		return nullptr;
	}
	m->state = initial;

	int r = sd_bus_open_user(&m->bus);
	if (r < 0) {
		debug(-r, "sd_bus_open_user");
		free(m);
		return nullptr;
	}

	r = sd_bus_add_object_vtable(m->bus, nullptr,
	    MPRIS_OBJECT, MPRIS_ROOT, mpris_root_vtable, m);
	if (r < 0) {
		debug(-r, "sd_bus_add_object_vtable (root)");
		goto err;
	}

	r = sd_bus_add_object_vtable(m->bus, nullptr,
	    MPRIS_OBJECT, MPRIS_PLAYER, mpris_player_vtable, m);
	if (r < 0) {
		debug(-r, "sd_bus_add_object_vtable (player)");
		goto err;
	}

	r = sd_bus_request_name(m->bus, MPRIS_NAME, 0);
	if (r < 0) {
		debug(-r, "sd_bus_request_name");
		goto err;
	}

	return m;

err:
	sd_bus_unref(m->bus);
	free(m);
	return nullptr;
}

int
mpris_fd(const struct mpris *m)
{
	return sd_bus_get_fd(m->bus);
}

short
mpris_events(const struct mpris *m)
{
	int r = sd_bus_get_events(m->bus);
	return r < 0 ? 0 : (short)r;
}

int
mpris_process(struct mpris *m, struct state *st)
{
	m->dispatch = st;
	int r = sd_bus_process(m->bus, nullptr);
	m->dispatch = nullptr;
	return r;
}

void
mpris_notify(struct mpris *m, struct state old, struct state new)
{
	/* Detect song change. */
	bool song_changed = false;
	if (!qsize(new.queue)) {
		if (m->have_tags) {
			song_tags_free(&m->tags);
			m->have_tags = false;
		}
		cleanup_art(m);
		song_changed = qsize(old.queue) > 0;
	} else {
		const struct song *s = &new.queue.data[
		    new.queue.head % new.queue.size];
		if (!m->have_tags || m->cur_uid != s->uid) {
			song_changed = true;
			refresh_tags(m, s);
		}
	}

	m->state = new;

	/* Collect names of changed Player properties. */
	const char *props[8];
	unsigned int np = 0;
	if (old.play != new.play)
		props[np++] = "PlaybackStatus";
	if (old.mode != new.mode)
		props[np++] = "LoopStatus";
	if (old.volume != new.volume)
		props[np++] = "Volume";
	if (song_changed) {
		props[np++] = "Metadata";
		props[np++] = "CanGoNext";
		props[np++] = "CanPlay";
		props[np++] = "CanPause";
	}
	if (np == 0)
		return;

	props[np] = nullptr;
	sd_bus_emit_properties_changed_strv(m->bus,
	    MPRIS_OBJECT, MPRIS_PLAYER, (char **)props);
}

void
mpris_close(struct mpris *m)
{
	if (!m)
		return;
	if (m->have_tags)
		song_tags_free(&m->tags);
	cleanup_art(m);
	sd_bus_unref(m->bus);
	free(m);
}
