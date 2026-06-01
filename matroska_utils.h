/* Include: matroska.h, stddef.h. */
#pragma once

struct mkv_cluster {
	uint64_t ts;
};

/* Immutable playback configuration passed to mkv_nextframe. */
struct mkv_range {
	uint32_t track;
	uint64_t ts_scale;
	uint64_t start, end;        /* nanoseconds; end == 0 means play to EOF */
	uint64_t sample_rate;       /* Hz; 0 disables sample-precise truncation */
	size_t   frame_sz;          /* bytes per PCM frame = channels × (bps / 8) */
};

/* Mutable stream cursor updated by mkv_nextframe across calls. */
struct mkv_cursor {
	struct mkv_cluster cluster;
	uint64_t block_ts;          /* nanoseconds; effective start of last returned block */
	size_t   leading_skip;      /* bytes to seek past at fd before reading (start truncation) */
	size_t   pending_skip;      /* bytes to seek past after reading the block (end truncation) */
	off_t    blockgroup_end;    /* nonzero while caller is reading a BlockGroup Block */
};

/* Result of mkv_findcoverart(). Strings are NUL-terminated and truncated
   silently to fit. data_off is the absolute file offset of the FileData
   body; data_sz is its byte count. */
struct mkv_attachment {
	char   mime[64];
	char   filename[256];
	off_t  data_off;
	size_t data_sz;
};

[[gnu::fd_arg_read(1)]]
int mkv_findchapter(int, uint64_t, struct mkv_chapter *);
[[gnu::fd_arg_read(1)]]
int mkv_findtrack(int, const uint64_t [static TRACKS_MAX], struct mkv_track *);
[[gnu::fd_arg_read(1)]]
int mkv_findcue(int, uint64_t, uint64_t, uint32_t, off_t *);
/* fd positioned by caller at MKV_ATTACHMENTS. Finds the first AttachedFile
   whose FileMediaType begins with "image/". Returns 1 and fills *out on
   success, 0 if none found or on error. */
[[gnu::fd_arg_read(1), gnu::nonnull(2)]]
int mkv_findcoverart(int, struct mkv_attachment *);
/* fd positioned by caller at MKV_CHAPTERS element. Saves and restores fd
   position around each callback so the callback may freely seek. */
[[gnu::fd_arg_read(1), gnu::nonnull(2)]]
int mkv_visitchapters(int fd,
    int (*cb)(const struct mkv_chapter *, void *), void *);
[[nodiscard, gnu::fd_arg_read(1)]]
ssize_t mkv_nextframe(int, struct mkv_cursor *, const struct mkv_range *);
