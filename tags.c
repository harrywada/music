#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp(3). */
#include <sys/types.h>

#include <stdbool.h>
#include "ebml.h"
#include "matroska.h"
#include "tags.h"
#include "utils.h"

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
#define SCOPE_EDITION 3  /* TargetTypeValue 60 — disc/volume level */

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
		field = (scope == SCOPE_EDITION) ? TAG_DISC : TAG_TRACK;
	} else {
		return;
	}

	/* Edition-level tags only contribute the disc number. */
	if (scope == SCOPE_EDITION && field != TAG_DISC)
		return;

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

	/* TagName MUST be first per the Matroska spec; skip the backward
	   seek for well-formed files.  Fall back to a two-pass approach
	   only when a non-conformant file places TagName after other
	   elements. */
	if (pos(fd) < end && ebml_peek(fd) == MKV_TAGNAME) {
		ebml_readstring(fd, MKV_TAGNAME, &name, &name_sz);
	} else {
		off_t body_start = pos(fd);
		while (pos(fd) < end) {
			if (ebml_peek(fd) == MKV_TAGNAME) {
				ebml_readstring(fd, MKV_TAGNAME, &name, &name_sz);
				break;
			}
			ebml_skip(fd, EBML_ANY_ELEMENT);
		}
		seek(fd, body_start);
	}

	if (!name || name_sz == 0) {
		free(name);
		return;
	}

	size_t nlen = name_sz < sizeof name_buf - 1 ? name_sz : sizeof name_buf - 1;
	memcpy(name_buf, name, nlen);
	name_buf[nlen] = '\0';
	free(name);
	name = NULL;
	name_sz = 0;

	/* Forward pass: fd is already past TagName in the fast path, or
	   reset to body_start in the fallback (TagName skipped in switch). */
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

/* Determine the scope for a Tag element.  Called with fd positioned at
 * the first child of Tag (before or at Targets).
 *
 * Chapter UIDs are specific (they identify an individual song): skip any
 * Tag whose chapter UIDs are all for other chapters.  Track UIDs identify
 * the audio track that carries all songs in the container; files that have
 * been remuxed often end up with a TagTrackUID that no longer matches the
 * TrackUID in the Tracks element, so fall back to TargetTypeValue rather
 * than silently discarding the tag.
 *
 * Returns a SCOPE_* constant, or -1 if this Tag should be skipped. */
static int
tag_scope(int fd, uint64_t chapter_uid, uint64_t track_uid)
{
	uint32_t target_type      = 50;
	bool has_any_chapter      = false;
	bool has_matching_chapter = false;
	bool has_any_track        = false;
	bool has_matching_track   = false;

	if (ebml_peek(fd) == MKV_TARGETS) {
		off_t targets_end;
		if ((targets_end = ebml_descend(fd, MKV_TARGETS)) == -1)
			return -1;
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

	/* An explicit TagTrackUID that doesn't match means this tag targets a
	 * different track; skip it.  When no TagTrackUID is present, fall back
	 * to TargetTypeValue so that track-level tags in files that have been
	 * remuxed (and thus have stale UIDs) are not silently discarded. */
	if (has_matching_chapter) return SCOPE_CHAPTER;
	if (has_any_chapter)      return -1;
	if (has_matching_track)   return SCOPE_TRACK;
	if (has_any_track)        return -1;
	if (target_type == 30)    return SCOPE_TRACK;
	if (target_type == 50)    return SCOPE_ALBUM;
	if (target_type == 60)    return SCOPE_EDITION;
	return -1;
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

			int scope = tag_scope(fd, chapter_uid, track_uid);
			if (scope < 0) {
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
