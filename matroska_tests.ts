#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdbool.h>
#include <stddef.h>
#include "ebml.h"
#include "matroska.h"

#suite matroska

/* ------------------------------------------------------------------ */
/* EBML write helpers                                                  */
/* ------------------------------------------------------------------ */

/* Write an N-byte big-endian integer. */
static void
wbe(int fd, uint64_t v, int n)
{
	uint8_t buf[8] = {0};
	for (int i = n - 1; i >= 0; i--) { buf[i] = v & 0xFF; v >>= 8; }
	write(fd, buf, n);
}

/* Write an EBML element ID (already big-endian-encoded, no re-encoding). */
static void
wid(int fd, uint32_t id)
{
	if      (id > 0xFFFFFF) wbe(fd, id, 4);
	else if (id > 0xFFFF)   wbe(fd, id, 3);
	else if (id > 0xFF)     wbe(fd, id, 2);
	else                     wbe(fd, id, 1);
}

/* Write a VINT-encoded element length. */
static void
wlen(int fd, uint64_t len)
{
	if      (len < 0x7F)   wbe(fd, 0x80   | len, 1);
	else if (len < 0x3FFF) wbe(fd, 0x4000 | len, 2);
	else                    wbe(fd, 0x200000 | len, 3);
}

/* Write a complete EBML unsigned-int element of vsz value bytes. */
static void
wuint(int fd, uint32_t id, uint64_t val, int vsz)
{
	wid(fd, id); wlen(fd, vsz); wbe(fd, val, vsz);
}

/* Write a float64 element (big-endian IEEE 754). */
static void
wfloat64(int fd, uint32_t id, double val)
{
	uint64_t bits;
	wid(fd, id); wlen(fd, 8);
	__builtin_memcpy(&bits, &val, 8);
	wbe(fd, bits, 8);
}

/* Begin a master element; returns a cookie for end_master. */
struct mstart { off_t len_pos, data_start; };

static struct mstart
begin_master(int fd, uint32_t id)
{
	struct mstart m;
	wid(fd, id);
	m.len_pos = lseek(fd, 0, SEEK_CUR);
	wbe(fd, 0, 2); /* 2-byte placeholder — patched by end_master */
	m.data_start = lseek(fd, 0, SEEK_CUR);
	return m;
}

/* Patch the placeholder length and leave fd at the end of the element. */
static void
end_master(int fd, struct mstart m)
{
	off_t cur = lseek(fd, 0, SEEK_CUR);
	lseek(fd, m.len_pos, SEEK_SET);
	wbe(fd, 0x4000 | (cur - m.data_start), 2);
	lseek(fd, cur, SEEK_SET);
}

/* ------------------------------------------------------------------ */
/* Shared fd fixture                                                    */
/* ------------------------------------------------------------------ */

int fd;

void
fd_setup(void)
{
	fd = shm_open("matroska_tests", O_RDWR|O_CREAT, 0600);
	ck_assert_int_ge(fd, 0);
}

void
fd_teardown(void)
{
	close(fd);
	shm_unlink("matroska_tests");
}

/* ================================================================== */
/* mkv_findchapter                                                     */
/* ================================================================== */

#tcase mkv_findchapter

#test findchapter_finds_chapter_by_uid
	struct mkv_chapter c;

	struct mstart ch = begin_master(fd, MKV_CHAPTERS);
	  struct mstart ee = begin_master(fd, MKV_EDITIONENTRY);
	    struct mstart ca1 = begin_master(fd, MKV_CHAPTERATOM);
	      wuint(fd, MKV_CHAPTERUID,      0xAAAA, 8);
	      wuint(fd, MKV_CHAPTERTIMESTART, 0,      8);
	      wuint(fd, MKV_CHAPTERTIMEEND,   1000,   8);
	    end_master(fd, ca1);
	    struct mstart ca2 = begin_master(fd, MKV_CHAPTERATOM);
	      wuint(fd, MKV_CHAPTERUID,      0xBBBB, 8);
	      wuint(fd, MKV_CHAPTERTIMESTART, 1000,   8);
	      wuint(fd, MKV_CHAPTERTIMEEND,   2000,   8);
	    end_master(fd, ca2);
	  end_master(fd, ee);
	end_master(fd, ch);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_findchapter(fd, 0xBBBB, &c));
	ck_assert_uint_eq(c.uid,   0xBBBB);
	ck_assert_uint_eq(c.start, 1000);
	ck_assert_uint_eq(c.end,   2000);

#test findchapter_returns_false_when_uid_not_found
	struct mkv_chapter c;

	struct mstart ch = begin_master(fd, MKV_CHAPTERS);
	  struct mstart ee = begin_master(fd, MKV_EDITIONENTRY);
	    struct mstart ca = begin_master(fd, MKV_CHAPTERATOM);
	      wuint(fd, MKV_CHAPTERUID,      0xAAAA, 8);
	      wuint(fd, MKV_CHAPTERTIMESTART, 0,      8);
	    end_master(fd, ca);
	  end_master(fd, ee);
	end_master(fd, ch);
	lseek(fd, 0, SEEK_SET);

	ck_assert(!mkv_findchapter(fd, 0xDEAD, &c));

#test findchapter_end_is_zero_when_chaptertimeend_absent
	struct mkv_chapter c;

	struct mstart ch = begin_master(fd, MKV_CHAPTERS);
	  struct mstart ee = begin_master(fd, MKV_EDITIONENTRY);
	    struct mstart ca = begin_master(fd, MKV_CHAPTERATOM);
	      wuint(fd, MKV_CHAPTERUID,      0xCCCC, 8);
	      wuint(fd, MKV_CHAPTERTIMESTART, 5000,   8);
	      /* no ChapterTimeEnd */
	    end_master(fd, ca);
	  end_master(fd, ee);
	end_master(fd, ch);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_findchapter(fd, 0xCCCC, &c));
	ck_assert_uint_eq(c.end, 0);

#test findchapter_searches_across_edition_entries
	struct mkv_chapter c;

	struct mstart ch = begin_master(fd, MKV_CHAPTERS);
	  struct mstart ee1 = begin_master(fd, MKV_EDITIONENTRY);
	    struct mstart ca1 = begin_master(fd, MKV_CHAPTERATOM);
	      wuint(fd, MKV_CHAPTERUID,      0x1111, 8);
	      wuint(fd, MKV_CHAPTERTIMESTART, 0,      8);
	    end_master(fd, ca1);
	  end_master(fd, ee1);
	  struct mstart ee2 = begin_master(fd, MKV_EDITIONENTRY);
	    struct mstart ca2 = begin_master(fd, MKV_CHAPTERATOM);
	      wuint(fd, MKV_CHAPTERUID,      0x2222, 8);
	      wuint(fd, MKV_CHAPTERTIMESTART, 9000,   8);
	    end_master(fd, ca2);
	  end_master(fd, ee2);
	end_master(fd, ch);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_findchapter(fd, 0x2222, &c));
	ck_assert_uint_eq(c.uid, 0x2222);

/* ================================================================== */
/* mkv_findtrack                                                       */
/* ================================================================== */

#tcase mkv_findtrack

#test findtrack_finds_audio_track_by_uid
	struct mkv_track t;
	uint64_t uids[TRACKS_MAX] = { 0xF001, 0, 0, 0 };

	struct mstart tr = begin_master(fd, MKV_TRACKS);
	  struct mstart te = begin_master(fd, MKV_TRACKENTRY);
	    wuint(fd, MKV_TRACKNUMBER,  1,      1);
	    wuint(fd, MKV_TRACKUID,     0xF001, 8);
	    wuint(fd, MKV_TRACKTYPE,    AUDIO,  1);
	    wuint(fd, MKV_FLAGENABLED,  1,      1);
	    struct mstart au = begin_master(fd, MKV_AUDIO);
	      wfloat64(fd, MKV_SAMPLINGFREQUENCY, 44100.0);
	      wuint(fd, MKV_CHANNELS,  2,  1);
	      wuint(fd, MKV_BITDEPTH,  16, 1);
	    end_master(fd, au);
	  end_master(fd, te);
	end_master(fd, tr);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_findtrack(fd, uids, &t));
	ck_assert_uint_eq(t.uid,      0xF001);
	ck_assert_uint_eq(t.num,      1);
	ck_assert_uint_eq(t.channels, 2);
	ck_assert_uint_eq(t.bps,      16);
	ck_assert(t.rate == 44100.0);

#test findtrack_skips_non_audio_tracks
	struct mkv_track t;
	uint64_t uids[TRACKS_MAX] = { 0xF001, 0, 0, 0 };

	struct mstart tr = begin_master(fd, MKV_TRACKS);
	  /* Video track with matching UID — must be skipped. */
	  struct mstart te1 = begin_master(fd, MKV_TRACKENTRY);
	    wuint(fd, MKV_TRACKNUMBER, 1,      1);
	    wuint(fd, MKV_TRACKUID,    0xF001, 8);
	    wuint(fd, MKV_TRACKTYPE,   1,      1); /* VIDEO */
	    wuint(fd, MKV_FLAGENABLED, 1,      1);
	  end_master(fd, te1);
	  /* Audio track without matching UID — no match. */
	  struct mstart te2 = begin_master(fd, MKV_TRACKENTRY);
	    wuint(fd, MKV_TRACKNUMBER, 2,      1);
	    wuint(fd, MKV_TRACKUID,    0xF002, 8);
	    wuint(fd, MKV_TRACKTYPE,   AUDIO,  1);
	    wuint(fd, MKV_FLAGENABLED, 1,      1);
	  end_master(fd, te2);
	end_master(fd, tr);
	lseek(fd, 0, SEEK_SET);

	ck_assert(!mkv_findtrack(fd, uids, &t));

#test findtrack_skips_disabled_track
	struct mkv_track t;
	uint64_t uids[TRACKS_MAX] = { 0xF001, 0, 0, 0 };

	struct mstart tr = begin_master(fd, MKV_TRACKS);
	  struct mstart te = begin_master(fd, MKV_TRACKENTRY);
	    wuint(fd, MKV_TRACKNUMBER, 1,      1);
	    wuint(fd, MKV_TRACKUID,    0xF001, 8);
	    wuint(fd, MKV_TRACKTYPE,   AUDIO,  1);
	    wuint(fd, MKV_FLAGENABLED, 0,      1); /* disabled */
	  end_master(fd, te);
	end_master(fd, tr);
	lseek(fd, 0, SEEK_SET);

	ck_assert(!mkv_findtrack(fd, uids, &t));

#test findtrack_all_uids_zero_returns_first_audio_track
	/* All ChapterTrackUIDs = 0 means any track applies (Matroska spec). */
	struct mkv_track t;
	uint64_t uids[TRACKS_MAX] = { 0, 0, 0, 0 };

	struct mstart tr = begin_master(fd, MKV_TRACKS);
	  struct mstart te = begin_master(fd, MKV_TRACKENTRY);
	    wuint(fd, MKV_TRACKNUMBER, 3,      1);
	    wuint(fd, MKV_TRACKUID,    0xABCD, 8);
	    wuint(fd, MKV_TRACKTYPE,   AUDIO,  1);
	    wuint(fd, MKV_FLAGENABLED, 1,      1);
	    struct mstart au = begin_master(fd, MKV_AUDIO);
	      wfloat64(fd, MKV_SAMPLINGFREQUENCY, 48000.0);
	      wuint(fd, MKV_CHANNELS, 1, 1);
	    end_master(fd, au);
	  end_master(fd, te);
	end_master(fd, tr);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_findtrack(fd, uids, &t));
	ck_assert_uint_eq(t.uid, 0xABCD);
	ck_assert_uint_eq(t.num, 3);

/* ================================================================== */
/* mkv_findcue                                                         */
/* ================================================================== */

#tcase mkv_findcue

/* Write a CuePoint with one CueTrackPositions entry. */
static void
write_cuepoint(int fd, uint64_t time, uint32_t track, off_t cluster_pos)
{
	struct mstart cp = begin_master(fd, MKV_CUEPOINT);
	  wuint(fd, MKV_CUETIME, time, 8);
	  struct mstart ctp = begin_master(fd, MKV_CUETRACKPOSITIONS);
	    wuint(fd, MKV_CUETRACK,           track,       4);
	    wuint(fd, MKV_CUECLUSTERPOSITION, cluster_pos, 8);
	  end_master(fd, ctp);
	end_master(fd, cp);
}

#test findcue_returns_cluster_pos_for_cue_before_start
	/* ts_scale=1 so cue.time * 1 = cue.time ns; start = 500 ns. */
	off_t out = -1;

	struct mstart cues = begin_master(fd, MKV_CUES);
	  write_cuepoint(fd, 0,    1, 100);
	  write_cuepoint(fd, 1000, 1, 200);
	end_master(fd, cues);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_findcue(fd, 500, 1, 1, &out));
	ck_assert_int_eq(out, 100); /* cue at t=0, before start=500 */

#test findcue_picks_last_cue_before_start
	off_t out = -1;

	struct mstart cues = begin_master(fd, MKV_CUES);
	  write_cuepoint(fd, 100,  1, 10);
	  write_cuepoint(fd, 200,  1, 20);
	  write_cuepoint(fd, 5000, 1, 30);
	end_master(fd, cues);
	lseek(fd, 0, SEEK_SET);

	/* start=1000: cues at t=100 and t=200 qualify; pick the later one. */
	ck_assert(mkv_findcue(fd, 1000, 1, 1, &out));
	ck_assert_int_eq(out, 20);

#test findcue_returns_false_when_first_cue_exceeds_start
	off_t out = -1;

	struct mstart cues = begin_master(fd, MKV_CUES);
	  write_cuepoint(fd, 1000, 1, 50);
	end_master(fd, cues);
	lseek(fd, 0, SEEK_SET);

	/* start=500: only cue is at t=1000 > 500, so no valid cue. */
	ck_assert(!mkv_findcue(fd, 500, 1, 1, &out));

#test findcue_returns_false_when_track_not_in_cue
	off_t out = -1;

	struct mstart cues = begin_master(fd, MKV_CUES);
	  write_cuepoint(fd, 0, 2, 99); /* track 2, not track 1 */
	end_master(fd, cues);
	lseek(fd, 0, SEEK_SET);

	ck_assert(!mkv_findcue(fd, 1000, 1, 1, &out));

#test findcue_respects_ts_scale
	/* cue.time=10, ts_scale=100 → 10*100=1000 ns; start=999 → no match. */
	off_t out = -1;

	struct mstart cues = begin_master(fd, MKV_CUES);
	  write_cuepoint(fd, 10, 1, 77);
	end_master(fd, cues);
	lseek(fd, 0, SEEK_SET);

	ck_assert(!mkv_findcue(fd, 999, 100, 1, &out));

/* ================================================================== */
/* mkv_nextframe                                                       */
/* ================================================================== */

#tcase mkv_nextframe

/* Write a SimpleBlock: track VINT (1 byte for track<128), timecode, flags, data. */
static void
write_simpleblock(int fd, uint32_t track, int16_t timecode, const uint8_t *data, size_t dsz)
{
	uint8_t track_vint = 0x80 | (uint8_t) track; /* 1-byte vint */
	size_t total = 1 + 2 + 1 + dsz;              /* track + tc + flags + data */
	wid(fd, MKV_SIMPLEBLOCK);
	wlen(fd, total);
	write(fd, &track_vint, 1);
	wbe(fd, (uint16_t) timecode, 2);
	wbe(fd, 0, 1); /* flags */
	write(fd, data, dsz);
}

/* Write a minimal cluster: ClusterTimestamp followed by one SimpleBlock. */
static void
write_cluster(int fd, uint64_t ts, uint32_t track, const uint8_t *data, size_t dsz)
{
	struct mstart cl = begin_master(fd, MKV_CLUSTER);
	  wuint(fd, MKV_CLUSTERTIMESTAMP, ts, 8);
	  write_simpleblock(fd, track, 0, data, dsz);
	end_master(fd, cl);
}

#test nextframe_returns_frame_size_and_positions_fd_at_data
	uint8_t payload[] = { 0xAA, 0xBB, 0xCC, 0xDD };
	struct mkv_range r = { .track=1, .ts_scale=1, .start=0, .end=0 };
	struct mkv_cursor cur = {0};

	write_cluster(fd, 0, 1, payload, sizeof payload);
	lseek(fd, 0, SEEK_SET);

	ssize_t sz = mkv_nextframe(fd, &cur, &r);
	ck_assert_int_eq(sz, (ssize_t) sizeof payload);

	uint8_t got[sizeof payload];
	ck_assert_int_eq(read(fd, got, sizeof got), (ssize_t) sizeof got);
	ck_assert_mem_eq(got, payload, sizeof payload);

#test nextframe_skips_wrong_track
	uint8_t payload[] = { 0x01, 0x02 };
	/* Only blocks for track 2 — range wants track 1 → all skipped → EOF → -1. */
	struct mkv_range r = { .track=1, .ts_scale=1, .start=0, .end=0 };
	struct mkv_cursor cur = {0};

	write_cluster(fd, 0, 2, payload, sizeof payload);
	lseek(fd, 0, SEEK_SET);

	ck_assert_int_eq(mkv_nextframe(fd, &cur, &r), -1);

#test nextframe_skips_blocks_before_start
	uint8_t early[]  = { 0x01 };
	uint8_t target[] = { 0x02, 0x03 };
	struct mkv_range r = { .track=1, .ts_scale=1, .start=100, .end=0 };
	struct mkv_cursor cur = {0};

	/* Cluster ts=0, block timecode=0 → ts=0 < start=100, skip.
	   Then cluster ts=100, block timecode=0 → ts=100 = start, return. */
	struct mstart cl1 = begin_master(fd, MKV_CLUSTER);
	  wuint(fd, MKV_CLUSTERTIMESTAMP, 0, 8);
	  write_simpleblock(fd, 1, 0, early, sizeof early);
	end_master(fd, cl1);
	struct mstart cl2 = begin_master(fd, MKV_CLUSTER);
	  wuint(fd, MKV_CLUSTERTIMESTAMP, 100, 8);
	  write_simpleblock(fd, 1, 0, target, sizeof target);
	end_master(fd, cl2);
	lseek(fd, 0, SEEK_SET);

	ck_assert_int_eq(mkv_nextframe(fd, &cur, &r), (ssize_t) sizeof target);

#test nextframe_returns_zero_at_chapter_end
	uint8_t payload[] = { 0x01 };
	/* Block ts = ts_scale * (cluster_ts + timecode) = 1 * (200 + 0) = 200.
	   range.end = 200 → ts >= end → return 0. */
	struct mkv_range r = { .track=1, .ts_scale=1, .start=0, .end=200 };
	struct mkv_cursor cur = {0};

	write_cluster(fd, 200, 1, payload, sizeof payload);
	lseek(fd, 0, SEEK_SET);

	ck_assert_int_eq(mkv_nextframe(fd, &cur, &r), 0);

#test nextframe_end_zero_does_not_trigger_chapter_end
	/* end=0 means "play to EOF" — the range-end check must be skipped. */
	uint8_t payload[] = { 0xFF };
	struct mkv_range r = { .track=1, .ts_scale=1, .start=0, .end=0 };
	struct mkv_cursor cur = {0};

	write_cluster(fd, 0, 1, payload, sizeof payload);
	lseek(fd, 0, SEEK_SET);

	/* Must return the frame size, not 0. */
	ck_assert_int_eq(mkv_nextframe(fd, &cur, &r), (ssize_t) sizeof payload);

#test nextframe_updates_cursor_on_cluster_transition
	uint8_t p1[] = { 0xAA };
	uint8_t p2[] = { 0xBB };
	struct mkv_range r = { .track=1, .ts_scale=1, .start=0, .end=0 };
	struct mkv_cursor cur = {0};

	struct mstart cl1 = begin_master(fd, MKV_CLUSTER);
	  wuint(fd, MKV_CLUSTERTIMESTAMP, 0, 8);
	  write_simpleblock(fd, 1, 0, p1, sizeof p1);
	end_master(fd, cl1);
	struct mstart cl2 = begin_master(fd, MKV_CLUSTER);
	  wuint(fd, MKV_CLUSTERTIMESTAMP, 500, 8);
	  write_simpleblock(fd, 1, 0, p2, sizeof p2);
	end_master(fd, cl2);
	lseek(fd, 0, SEEK_SET);

	ck_assert_int_gt(mkv_nextframe(fd, &cur, &r), 0);
	ck_assert_uint_eq(cur.cluster.ts, 0);
	read(fd, p1, sizeof p1); /* consume first frame */

	ck_assert_int_gt(mkv_nextframe(fd, &cur, &r), 0);
	ck_assert_uint_eq(cur.cluster.ts, 500);

#test nextframe_returns_minus_one_on_eof
	/* Empty fd — first ebml_peek returns 0 → -1. */
	struct mkv_range r = { .track=1, .ts_scale=1, .start=0, .end=0 };
	struct mkv_cursor cur = {0};

	ck_assert_int_eq(mkv_nextframe(fd, &cur, &r), -1);

#main-pre
	tcase_add_checked_fixture(tc1_1, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_2, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_3, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_4, fd_setup, fd_teardown);
