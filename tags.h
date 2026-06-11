/* Include: stddef.h, stdint.h. */
#pragma once

enum tag_field {
	TAG_DATE,
	TAG_ORIG_DATE,
	TAG_ARTIST,
	TAG_TITLE,
	TAG_GENRE,
	TAG_ALBUM,
	TAG_TRACK,
	TAG_DISC,  /* PART_NUMBER at TargetTypeValue 60 (EDITION/VOLUME level) */
	TAG_FIELD_COUNT,
};

struct tag_values {
	char   **vals; /* heap array of null-terminated strings */
	size_t  *lens;
	size_t   count;
};

struct song_tags {
	struct tag_values fields[TAG_FIELD_COUNT];
};

struct track_tags {
	int    has_replaygain_gain;
	double replaygain_gain_db;
	int    has_replaygain_peak;
	double replaygain_peak;
};

[[gnu::nonnull(1)]]
void song_tags_free(struct song_tags *);

/* fd positioned by caller at MKV_TAGS (seek to si.segment + si.tags first). */
[[gnu::fd_arg_read(1), gnu::nonnull(5)]]
int mkv_readsongtags(int fd, uint64_t chapter_uid, uint64_t track_uid,
                     uint64_t edition_uid, struct song_tags *);
/* fd positioned by caller at MKV_TAGS (seek to si.segment + si.tags first). */
[[gnu::fd_arg_read(1), gnu::nonnull(3)]]
int mkv_readtracktags(int fd, uint64_t track_uid, struct track_tags *);
