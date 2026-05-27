#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp(3). */
#include <unistd.h> /* read(3p). */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "ebml.h"

#include <stdint.h>
#include <sys/types.h>
#include "matroska.h"

#include <sys/types.h>
#include "utils.h"

int
mkv_readseekinfo(int fd, struct mkv_seekinfo *sk_info)
{
	uint_least32_t sk_id = 0;
	off_t sk_pos = 0;

	off_t begin, end;
	unsigned int i;

	begin = sk_info->segment = pos(fd); /* SeekHead _SHOULD_ be at the beginning of the segment. */
	if ((end = ebml_descend(fd, MKV_SEEKHEAD)) == -1)
		goto err;

	while (pos(fd) < end) {
		ebml_descend(fd, MKV_SEEK);

		for (i = 0; i < 2; i += 1) switch (ebml_peek(fd)) { /* XXX Can there be redundant (3+) elements? */
		case MKV_SEEKID: {
			uint64_t tmp;
			if (!ebml_readuint(fd, MKV_SEEKID, &tmp))
				goto err;
			sk_id = (uint_least32_t) tmp;
			break;
		}
		case MKV_SEEKPOSITION:
			if (!ebml_readuint(fd, MKV_SEEKPOSITION, (uint64_t *) &sk_pos))
				goto err;
			break;
		default:
			ebml_skip(fd, EBML_ANY_ELEMENT);
			break;
		}

		switch (sk_id) {
		case MKV_INFO:
			sk_info->info = sk_pos;
			break;
		case MKV_CLUSTER:
			sk_info->clusters = sk_pos;
			break;
		case MKV_TRACKS:
			sk_info->tracks = sk_pos;
			break;
		case MKV_CUES:
			sk_info->cues = sk_pos;
			break;
		case MKV_ATTACHMENTS:
			sk_info->attachments = sk_pos;
			break;
		case MKV_CHAPTERS:
			sk_info->chapters = sk_pos;
			break;
		case MKV_TAGS:
			sk_info->tags = sk_pos;
			break;
		default:
			goto err;
		}
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
mkv_readinfo(int fd, struct mkv_info *info)
{
	off_t begin, end;

	begin = pos(fd);
	if ((end = ebml_descend(fd, MKV_INFO)) == -1)
		goto err;

	info->ts_scale = 1000000;

	while (pos(fd) < end) switch (ebml_peek(fd)) {
	case MKV_TIMESTAMPSCALE:
		if (!ebml_readuint(fd, MKV_TIMESTAMPSCALE, &info->ts_scale))
			goto err;
		return 1;
	default:
		ebml_skip(fd, EBML_ANY_ELEMENT);
		break;
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
mkv_readtrackentry(int fd, struct mkv_track *track)
{
	off_t begin, end;

	begin = pos(fd);
	if ((end = ebml_descend(fd, MKV_TRACKENTRY)) == -1)
		goto err;

	/* Defaults for non-mandatory elements. */
	track->channels = 1;
	track->enabled = 1;
	track->rate = 8000;

	while (pos(fd) < end) switch (ebml_peek(fd)) {
	case MKV_TRACKNUMBER:
		if (!ebml_readuint(fd, MKV_TRACKNUMBER, &track->num))
			goto err;
		break;
	case MKV_TRACKUID:
		if (!ebml_readuint(fd, MKV_TRACKUID, &track->uid))
			goto err;
		break;
	case MKV_TRACKTYPE: {
		uint8_t tmp;
		if (!ebml_readuint(fd, MKV_TRACKTYPE, &tmp))
			goto err;
		track->type = (typeof(track->type)) tmp;
		break;
	}
	case MKV_FLAGENABLED:
		if (!ebml_readuint(fd, MKV_FLAGENABLED, &track->enabled))
			goto err;
		break;
	case MKV_AUDIO:
		ebml_descend(fd, MKV_AUDIO);
		break;
	case MKV_SAMPLINGFREQUENCY:
		if (!ebml_readfloat(fd, MKV_SAMPLINGFREQUENCY, &track->rate))
			goto err;
		break;
	case MKV_CHANNELS:
		if (!ebml_readuint(fd, MKV_CHANNELS, &track->channels))
			goto err;
		break;
	case MKV_BITDEPTH:
		if (!ebml_readuint(fd, MKV_BITDEPTH, &track->bps))
			goto err;
		break;
	default:
		ebml_skip(fd, EBML_ANY_ELEMENT);
		break;
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
mkv_readcuepoint(int fd, struct mkv_cue *cue)
{
	off_t begin, end, track_end;
	unsigned int track_i = 0;

	uint64_t tmp;

	begin = pos(fd);
	if ((end = ebml_descend(fd, MKV_CUEPOINT)) == -1)
		goto err;

	while (pos(fd) < end) switch (ebml_peek(fd)) {
	case MKV_CUETIME:
		if (!ebml_readuint(fd, MKV_CUETIME, &cue->time))
			goto err;
		break;
	case MKV_CUETRACKPOSITIONS:
		if (track_i >= TRACKS_MAX)
			goto err;

		if ((track_end = ebml_descend(fd, MKV_CUETRACKPOSITIONS)) == -1)
			goto err;

		/* Defaults for non-mandatory elements. */
		cue->tracks[track_i].relpos = -1;

		while (pos(fd) < track_end) switch (ebml_peek(fd)) {
		case MKV_CUETRACK:
			if (!ebml_readuint(fd, MKV_CUETRACK, &cue->tracks[track_i].num))
				goto err;
			break;
		case MKV_CUECLUSTERPOSITION:
			if (!ebml_readuint(fd, MKV_CUECLUSTERPOSITION, &tmp))
				goto err;
			cue->tracks[track_i].pos = tmp;
			break;
		case MKV_CUERELATIVEPOSITION:
			if (!ebml_readuint(fd, MKV_CUERELATIVEPOSITION, &tmp))
				goto err;
			cue->tracks[track_i].relpos = tmp;
			break;
		default:
			ebml_skip(fd, EBML_ANY_ELEMENT);
			break;
		}

		track_i += 1;
		break;
	default:
		/* Only CueTime and CueTrackPositions are allowed. */
		goto err;
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
mkv_readblock(int fd, struct mkv_block *block)
{
	off_t begin, end;

	uint16_t framesize;
	struct vint ebml_framesize;
	unsigned int offset;
	int i;

	begin = pos(fd);

	switch (ebml_peek(fd)) {
	case MKV_BLOCK:
		if ((end = ebml_descend(fd, MKV_BLOCK)) == -1)
			goto err;
		break;
	case MKV_SIMPLEBLOCK:
		if ((end = ebml_descend(fd, MKV_SIMPLEBLOCK)) == -1)
			goto err;
		break;
	default:
		goto err;
	}


	if (!vint_read(fd, &block->track))
		goto err;
	if (read(fd, &block->timecode, 2) != 2)
		goto err;
	if (read(fd, &block->flags, 1) != 1)
		goto err;

	switch (block->flags & MKV_BLOCKLACINGMASK) {
	case 0: /* No lacing. */
		block->frames_n = 1;
		block->frames_sz = end - pos(fd);
		break;
	case 2: /* Xiph lacing. */
		if (read(fd, &block->frames_n, 1) != 1)
			goto err;
		block->frames_sz = 0;
		for (i = 0; i < block->frames_n; i += 1) {
			uint8_t b;
			framesize = 0;
			do {
				if (read(fd, &b, 1) != 1)
					goto err;
				framesize += b;
			} while (b == 0xff);
			block->frames_sz += framesize;
		}
		block->frames_sz += end - (pos(fd) + block->frames_sz);
		block->frames_n += 1;
		break;
	case 4: /* Fixed-size lacing. */
		if (read(fd, &block->frames_n, 1) != 1)
			goto err;
		block->frames_n += 1;
		block->frames_sz = end - pos(fd);
		break;
	case 6: /* EBML lacing. */
		if (read(fd, &block->frames_n, 1) != 1)
			goto err;
		if (!vint_read(fd, &ebml_framesize))
			goto err;
		block->frames_sz = framesize = vint_value(ebml_framesize);
		for (i = 0; i < block->frames_n - 1; i += 1) {
			if (!vint_read(fd, &ebml_framesize))
				goto err;
			offset = (1 << ((7 * ebml_framesize.size) - 1)) - 1;
			framesize = vint_value(ebml_framesize) - offset + framesize;
			block->frames_sz += framesize;
		}
		block->frames_sz += end - (pos(fd) + block->frames_sz);
		block->frames_n += 1;
		break;
	default:
		ebml_skip(fd, EBML_ANY_ELEMENT); /* XXX Is this correct? Shouldn't it just seek to the end or error? */
		break;
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
mkv_readchapteratom(int fd, struct mkv_chapter *chapter)
{
	off_t begin, end;
	int track_i;

	for (track_i = 0; track_i < TRACKS_MAX; track_i += 1)
		chapter->track_uids[track_i] = 0;
	chapter->end = 0; /* Default, may not be set. */
	track_i = 0;

	begin = pos(fd);
	if ((end = ebml_descend(fd, MKV_CHAPTERATOM)) == -1)
		goto err;

	while (pos(fd) < end) switch (ebml_peek(fd)) {
	case MKV_CHAPTERUID:
		if (!ebml_readuint(fd, MKV_CHAPTERUID, &chapter->uid))
			goto err;
		break;
	case MKV_CHAPTERTIMESTART:
		if (!ebml_readuint(fd, MKV_CHAPTERTIMESTART, &chapter->start))
			goto err;
		break;
	case MKV_CHAPTERTIMEEND:
		if (!ebml_readuint(fd, MKV_CHAPTERTIMEEND, &chapter->end))
			goto err;
		break;
	case MKV_CHAPTERTRACK:
		ebml_descend(fd, MKV_CHAPTERTRACK);
		while (track_i < TRACKS_MAX && ebml_peek(fd) == MKV_CHAPTERTRACKUID)
			if (!ebml_readuint(fd, MKV_CHAPTERTRACKUID, &chapter->track_uids[track_i++]))
				goto err;
		break;
	default:
		ebml_skip(fd, EBML_ANY_ELEMENT);
		break;
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
mkv_findchapter(int fd, uint64_t id, struct mkv_chapter *c)
{
	if (ebml_descend(fd, MKV_CHAPTERS) == -1)
		return 0;

	for (;;) {
		off_t end;

		if ((end = ebml_descend(fd, MKV_EDITIONENTRY)) == -1)
			return 0;
		while (pos(fd) < end) {
			if (ebml_peek(fd) != MKV_CHAPTERATOM) {
				ebml_skip(fd, EBML_ANY_ELEMENT);
				continue;
			}
			if (!mkv_readchapteratom(fd, c))
				return 0;
			if (c->uid == id)
				return 1;
		}
	}
}

int
mkv_findtrack(int fd, const uint64_t uids[static TRACKS_MAX], struct mkv_track *t)
{
	bool any = true;
	for (int i = 0; i < TRACKS_MAX; i += 1)
		if (uids[i]) { any = false; break; }

	if (ebml_descend(fd, MKV_TRACKS) == -1)
		return 0;
	for (;;) {
		if (!mkv_readtrackentry(fd, t))
			return 0;
		if (!t->enabled || t->type != AUDIO)
			continue;
		if (any)
			return 1;
		for (int i = 0; i < TRACKS_MAX; i += 1)
			if (t->uid == uids[i])
				return 1;
	}
}

int
mkv_findcue(int fd, uint64_t start, uint64_t ts_scale, uint32_t track, off_t *out)
{
	struct mkv_cue cue = {0}, next;
	int found = 0;

	if (ebml_descend(fd, MKV_CUES) == -1)
		return 0;

	while (mkv_readcuepoint(fd, &next)) {
		if (next.time * ts_scale > start)
			break;
		cue = next;
		found = 1;
	}

	if (!found)
		return 0;

	for (int i = 0; i < TRACKS_MAX; i += 1) {
		if (cue.tracks[i].num == track) {
			*out = cue.tracks[i].pos;
			return 1;
		}
	}
	return 0;
}

ssize_t
mkv_nextframe(int fd, struct mkv_cursor *cursor, const struct mkv_range *range)
{
	const off_t begin = pos(fd);

	for (;;) switch (ebml_peek(fd)) {
	case MKV_CLUSTER:
		if (ebml_descend(fd, MKV_CLUSTER) == -1)
			goto err;
		if (!ebml_readuint(fd, MKV_CLUSTERTIMESTAMP, &cursor->cluster.ts))
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

		if (vint_value(b.track) != range->track) {
			seek(fd, pos(fd) + b.frames_sz);
			break;
		}

		const uint64_t ts = range->ts_scale
		                  * (cursor->cluster.ts + (uint64_t) b.timecode);
		if (ts < range->start) {
			seek(fd, pos(fd) + b.frames_sz);
			break;
		}
		if (range->end && ts >= range->end) {
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

/* ── Tag parsing helpers ─────────────────────────────────── */

static char *
dup_str(const char *s, size_t len)
{
	char *r = malloc(len + 1);
	if (!r)
		return NULL;
	memcpy(r, s, len);
	r[len] = '\0';
	return r;
}

static int
append_tag_value(struct tag_values *tv, const char *s, size_t len)
{
	size_t n = tv->count + 1;
	char   **nv = realloc(tv->vals, n * sizeof *nv);
	size_t  *nl = realloc(tv->lens, n * sizeof *nl);
	char    *copy;

	if (!nv || !nl) {
		if (nv) tv->vals = nv;
		if (nl) tv->lens = nl;
		return 0;
	}
	tv->vals = nv;
	tv->lens = nl;

	if (!(copy = dup_str(s, len)))
		return 0;
	tv->vals[tv->count] = copy;
	tv->lens[tv->count] = len;
	tv->count++;
	return 1;
}

#define SCOPE_CHAPTER 0
#define SCOPE_TRACK   1
#define SCOPE_ALBUM   2

/* Add a (name, value) pair to the correct bucket and field. */
static void
add_tag_value(const char *name, const char *value, size_t value_len,
              int scope, const char *parent_name,
              struct song_tags *ct, struct song_tags *tt, struct song_tags *at)
{
	struct tag_values *tv = NULL;
	enum tag_field field;

	if (parent_name && strcasecmp(parent_name, "ORIGINAL") == 0) {
		if (strcasecmp(name, "DATE_RELEASED") == 0)
			field = TAG_ORIG_DATE;
		else
			return;
	} else if (strcasecmp(name, "DATE_RELEASED") == 0) {
		field = TAG_DATE;
	} else if (strcasecmp(name, "ARTIST") == 0) {
		field = TAG_ARTIST;
	} else if (strcasecmp(name, "TITLE") == 0) {
		if (scope == SCOPE_ALBUM) {
			/* Album title lives in the album bucket. */
			append_tag_value(&at->fields[TAG_ALBUM], value, value_len);
			return;
		}
		field = TAG_TITLE;
	} else if (strcasecmp(name, "GENRE") == 0) {
		field = TAG_GENRE;
	} else if (strcasecmp(name, "PART_NUMBER") == 0) {
		field = TAG_TRACK;
	} else {
		return;
	}

	if (scope == SCOPE_CHAPTER)     tv = &ct->fields[field];
	else if (scope == SCOPE_TRACK)  tv = &tt->fields[field];
	else                            tv = &at->fields[field];

	append_tag_value(tv, value, value_len);
}

/* Read the body of one SimpleTag (fd past the header, end = element end). */
static void
read_simpletag_body(int fd, off_t end, int scope, const char *parent_name,
                    struct song_tags *ct, struct song_tags *tt,
                    struct song_tags *at);

static void
read_one_simpletag(int fd, int scope, const char *parent_name,
                   struct song_tags *ct, struct song_tags *tt,
                   struct song_tags *at)
{
	off_t st_end;

	if ((st_end = ebml_descend(fd, MKV_SIMPLETAG)) == -1)
		return;
	read_simpletag_body(fd, st_end, scope, parent_name, ct, tt, at);
	seek(fd, st_end);
}

static void
read_simpletag_body(int fd, off_t end, int scope, const char *parent_name,
                    struct song_tags *ct, struct song_tags *tt,
                    struct song_tags *at)
{
	char  *name = NULL, *value = NULL;
	size_t name_sz = 0, value_sz = 0;
	char   name_buf[64];
	off_t  body_start = pos(fd);

	/* Pass 1: collect TagName. */
	while (pos(fd) < end) {
		if (ebml_peek(fd) == MKV_TAGNAME) {
			ebml_readstring(fd, MKV_TAGNAME, &name, &name_sz);
			break;
		}
		ebml_skip(fd, EBML_ANY_ELEMENT);
	}

	if (!name || name_sz == 0) {
		free(name);
		return;
	}

	/* Null-terminate into stack buffer. */
	size_t nlen = name_sz < sizeof name_buf - 1 ? name_sz : sizeof name_buf - 1;
	memcpy(name_buf, name, nlen);
	name_buf[nlen] = '\0';
	free(name);
	name = NULL;
	name_sz = 0;

	/* Pass 2: collect TagString and recurse into nested SimpleTags. */
	seek(fd, body_start);
	while (pos(fd) < end) {
		switch (ebml_peek(fd)) {
		case MKV_TAGNAME:
			ebml_skip(fd, MKV_TAGNAME);
			break;
		case MKV_TAGSTRING:
			ebml_readstring(fd, MKV_TAGSTRING, &value, &value_sz);
			break;
		case MKV_SIMPLETAG:
			read_one_simpletag(fd, scope, name_buf, ct, tt, at);
			break;
		default:
			ebml_skip(fd, EBML_ANY_ELEMENT);
			break;
		}
	}

	if (value && value_sz > 0)
		add_tag_value(name_buf, value, value_sz, scope, parent_name,
		              ct, tt, at);
	free(value);
}

void
song_tags_free(struct song_tags *t)
{
	for (int f = 0; f < TAG_FIELD_COUNT; f++) {
		struct tag_values *tv = &t->fields[f];
		for (size_t i = 0; i < tv->count; i++)
			free(tv->vals[i]);
		free(tv->vals);
		free(tv->lens);
		*tv = (struct tag_values){0};
	}
}

int
mkv_visitchapters(int fd,
    int (*cb)(const struct mkv_chapter *, void *), void *userdata)
{
	off_t chapters_end, edition_end;
	int visited = 0;

	if ((chapters_end = ebml_descend(fd, MKV_CHAPTERS)) == -1)
		return 0;

	while (pos(fd) < chapters_end) {
		if (ebml_peek(fd) != MKV_EDITIONENTRY) {
			ebml_skip(fd, EBML_ANY_ELEMENT);
			continue;
		}
		if ((edition_end = ebml_descend(fd, MKV_EDITIONENTRY)) == -1)
			return visited;

		while (pos(fd) < edition_end) {
			if (ebml_peek(fd) != MKV_CHAPTERATOM) {
				ebml_skip(fd, EBML_ANY_ELEMENT);
				continue;
			}

			struct mkv_chapter c;
			if (!mkv_readchapteratom(fd, &c))
				return visited;

			off_t resume = pos(fd);
			visited = 1;
			if (!cb(&c, userdata))
				return 1;
			seek(fd, resume);
		}
	}

	return visited;
}

int
mkv_readsongtags(int fd, uint64_t chapter_uid, uint64_t track_uid,
                 struct song_tags *out)
{
	struct song_tags ct = {0}, tt = {0}, at = {0};
	off_t tags_end;

	if ((tags_end = ebml_descend(fd, MKV_TAGS)) != -1) {
		while (pos(fd) < tags_end) {
			off_t tag_end;
			if (ebml_peek(fd) != MKV_TAG) {
				ebml_skip(fd, EBML_ANY_ELEMENT);
				continue;
			}
			if ((tag_end = ebml_descend(fd, MKV_TAG)) == -1)
				break;

			/* Read Targets. */
			uint32_t target_type = 50;
			bool has_any_chapter = false, has_matching_chapter = false;
			bool has_any_track   = false, has_matching_track   = false;

			if (ebml_peek(fd) == MKV_TARGETS) {
				off_t targets_end;
				if ((targets_end = ebml_descend(fd, MKV_TARGETS)) == -1) {
					seek(fd, tag_end);
					continue;
				}
				while (pos(fd) < targets_end) {
					uint64_t tmp = 0;
					switch (ebml_peek(fd)) {
					case MKV_TARGETTYPEVALUE:
						ebml_readuint(fd, MKV_TARGETTYPEVALUE, &target_type);
						break;
					case MKV_TAGCHAPTERUID:
						if (ebml_readuint(fd, MKV_TAGCHAPTERUID, &tmp)) {
							has_any_chapter = true;
							if (tmp == chapter_uid)
								has_matching_chapter = true;
						}
						break;
					case MKV_TAGTRACKUID:
						if (ebml_readuint(fd, MKV_TAGTRACKUID, &tmp)) {
							has_any_track = true;
							if (tmp == track_uid)
								has_matching_track = true;
						}
						break;
					default:
						ebml_skip(fd, EBML_ANY_ELEMENT);
						break;
					}
				}
			}

			/* Determine scope. */
			int scope;
			if (has_matching_chapter) {
				scope = SCOPE_CHAPTER;
			} else if (has_any_chapter) {
				/* Has chapter UIDs but none match — skip. */
				seek(fd, tag_end);
				continue;
			} else if (has_matching_track) {
				scope = SCOPE_TRACK;
			} else if (has_any_track) {
				/* Has track UIDs but none match — skip. */
				seek(fd, tag_end);
				continue;
			} else if (target_type == 30) {
				scope = SCOPE_TRACK;
			} else if (target_type == 50) {
				scope = SCOPE_ALBUM;
			} else {
				seek(fd, tag_end);
				continue;
			}

			/* Read SimpleTags. */
			while (pos(fd) < tag_end) {
				if (ebml_peek(fd) != MKV_SIMPLETAG) {
					ebml_skip(fd, EBML_ANY_ELEMENT);
					continue;
				}
				read_one_simpletag(fd, scope, NULL, &ct, &tt, &at);
			}

			seek(fd, tag_end);
		}
	}

	/* Merge: chapter > track > album per field. */
	for (int f = 0; f < TAG_FIELD_COUNT; f++) {
		struct tag_values *src;
		if (ct.fields[f].count > 0)
			src = &ct.fields[f];
		else if (tt.fields[f].count > 0)
			src = &tt.fields[f];
		else if (at.fields[f].count > 0)
			src = &at.fields[f];
		else
			continue;
		out->fields[f] = *src;
		*src = (struct tag_values){0};
	}

	song_tags_free(&ct);
	song_tags_free(&tt);
	song_tags_free(&at);
	return 1;
}
