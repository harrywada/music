#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp(3). */
#include <sys/types.h>
#include <unistd.h>

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
read_uint_body(int fd, off_t end, uint64_t *out)
{
	uint64_t val = 0;
	size_t len = (size_t)(end - pos(fd));
	uint8_t buf[sizeof val];

	if (len > sizeof buf || read(fd, buf, len) != (ssize_t)len)
		return 0;
	for (size_t i = 0; i < len; i++)
		val = (val << 8) | buf[i];
	*out = val;
	return 1;
}

static int
read_string_body(int fd, off_t end, char **dest, size_t *sz)
{
	size_t len = (size_t)(end - pos(fd));

	if (*sz < len) {
		*dest = realloc(*dest, len);
		if (*dest == NULL)
			return 0;
	}
	*sz = len;
	return read(fd, *dest, len) == (ssize_t)len;
}

static int
read_string_body_buf(int fd, off_t end, char *dest, size_t dest_sz)
{
	size_t len = (size_t)(end - pos(fd));
	size_t n = len < dest_sz - 1 ? len : dest_sz - 1;

	if (read(fd, dest, n) != (ssize_t)n)
		return 0;
	dest[n] = '\0';
	return 1;
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

enum mkv_tag_name {
	MKV_TAG_NAME_UNKNOWN,
	MKV_TAG_NAME_DATE_RELEASED,
	MKV_TAG_NAME_ARTIST,
	MKV_TAG_NAME_TITLE,
	MKV_TAG_NAME_GENRE,
	MKV_TAG_NAME_PART_NUMBER,
	MKV_TAG_NAME_ORIGINAL,
};

static enum mkv_tag_name
classify_tag_name(const char *name)
{
	if (strcasecmp(name, "DATE_RELEASED") == 0)
		return MKV_TAG_NAME_DATE_RELEASED;
	if (strcasecmp(name, "ARTIST") == 0)
		return MKV_TAG_NAME_ARTIST;
	if (strcasecmp(name, "TITLE") == 0)
		return MKV_TAG_NAME_TITLE;
	if (strcasecmp(name, "GENRE") == 0)
		return MKV_TAG_NAME_GENRE;
	if (strcasecmp(name, "PART_NUMBER") == 0)
		return MKV_TAG_NAME_PART_NUMBER;
	if (strcasecmp(name, "ORIGINAL") == 0)
		return MKV_TAG_NAME_ORIGINAL;
	return MKV_TAG_NAME_UNKNOWN;
}

/* Add a (name, value) pair to the correct bucket and field. */
static void
add_tag_value(enum mkv_tag_name name, const char *value, size_t value_len,
              int scope, enum mkv_tag_name parent_name,
              struct song_tags *ct, struct song_tags *tt, struct song_tags *at)
{
	struct tag_values *tv = NULL;
	enum tag_field field;

	if (parent_name == MKV_TAG_NAME_ORIGINAL) {
		if (name == MKV_TAG_NAME_DATE_RELEASED)
			field = TAG_ORIG_DATE;
		else
			return;
	} else if (name == MKV_TAG_NAME_DATE_RELEASED) {
		field = TAG_DATE;
	} else if (name == MKV_TAG_NAME_ARTIST) {
		field = TAG_ARTIST;
	} else if (name == MKV_TAG_NAME_TITLE) {
		if (scope == SCOPE_ALBUM) {
			/* Album title lives in the album bucket. */
			append_tag_value(&at->fields[TAG_ALBUM], value, value_len);
			return;
		}
		field = TAG_TITLE;
	} else if (name == MKV_TAG_NAME_GENRE) {
		field = TAG_GENRE;
	} else if (name == MKV_TAG_NAME_PART_NUMBER) {
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
read_simpletag_body(int fd, off_t end, int scope, enum mkv_tag_name parent_name,
                    struct song_tags *ct, struct song_tags *tt,
                    struct song_tags *at);

static void
read_simpletag_body(int fd, off_t end, int scope, enum mkv_tag_name parent_name,
                    struct song_tags *ct, struct song_tags *tt,
                    struct song_tags *at)
{
	char  *value = NULL;
	size_t value_sz = 0;
	char   name_buf[64] = {0};
	enum mkv_tag_name name;

	/* TagName MUST be first per the Matroska spec; skip the backward
	   seek for well-formed files.  Fall back to a two-pass approach
	   only when a non-conformant file places TagName after other
	   elements. */
	if (pos(fd) < end) {
		off_t child_start = pos(fd), child_end;
		uint32_t id;

		if (ebml_element(fd, &id, &child_end) && id == MKV_TAGNAME) {
			read_string_body_buf(fd, child_end, name_buf,
			                     sizeof name_buf);
			seek(fd, child_end);
		} else {
			seek(fd, child_start);
		}
	}
	if (name_buf[0] == '\0') {
		off_t body_start = pos(fd);
		while (pos(fd) < end) {
			off_t child_end;
			uint32_t id;

			if (!ebml_element(fd, &id, &child_end))
				break;
			if (id == MKV_TAGNAME) {
				read_string_body_buf(fd, child_end, name_buf,
				                     sizeof name_buf);
				break;
			}
			seek(fd, child_end);
		}
		seek(fd, body_start);
	}

	if (name_buf[0] == '\0')
		return;
	name = classify_tag_name(name_buf);

	/* Forward pass: fd is already past TagName in the fast path, or
	   reset to body_start in the fallback (TagName skipped in switch). */
	while (pos(fd) < end) {
		off_t child_end;
		uint32_t id;

		if (!ebml_element(fd, &id, &child_end))
			break;
		switch (id) {
		case MKV_TAGNAME:
			break;
		case MKV_TAGSTRING:
			read_string_body(fd, child_end, &value, &value_sz);
			break;
		case MKV_SIMPLETAG:
			read_simpletag_body(fd, child_end, scope, name, ct, tt, at);
			break;
		default:
			break;
		}
		seek(fd, child_end);
	}

	if (value && value_sz > 0)
		add_tag_value(name, value, value_sz, scope, parent_name,
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

	off_t begin = pos(fd), targets_end;
	uint32_t id;

	if (ebml_element(fd, &id, &targets_end)) {
		if (id != MKV_TARGETS) {
			seek(fd, begin);
		} else {
			while (pos(fd) < targets_end) {
				uint64_t tmp = 0;
				off_t child_end;

				if (!ebml_element(fd, &id, &child_end))
					return -1;
				switch (id) {
				case MKV_TARGETTYPEVALUE:
					if (read_uint_body(fd, child_end, &tmp))
						target_type = (uint32_t)tmp;
					break;
				case MKV_TAGCHAPTERUID:
					if (read_uint_body(fd, child_end, &tmp)) {
						has_any_chapter = true;
						if (tmp == chapter_uid)
							has_matching_chapter = true;
					}
					break;
				case MKV_TAGTRACKUID:
					if (read_uint_body(fd, child_end, &tmp)) {
						has_any_track = true;
						if (tmp == track_uid)
							has_matching_track = true;
					}
					break;
				default:
					break;
				}
				seek(fd, child_end);
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
			uint32_t id;

			if (!ebml_element(fd, &id, &tag_end))
				break;
			if (id != MKV_TAG)
				goto next_tag;

			int scope = tag_scope(fd, chapter_uid, track_uid);
			if (scope < 0)
				goto next_tag;

			/* Read SimpleTags. */
			while (pos(fd) < tag_end) {
				off_t child_end;

				if (!ebml_element(fd, &id, &child_end))
					break;
				if (id == MKV_SIMPLETAG)
					read_simpletag_body(fd, child_end, scope,
					                    MKV_TAG_NAME_UNKNOWN, &ct,
					                    &tt, &at);
				seek(fd, child_end);
			}

next_tag:
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
