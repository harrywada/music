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
/* mkv_readseekinfo                                                    */
/* ================================================================== */

#tcase mkv_readseekinfo

#test readseekinfo_reads_all_known_ids
	struct mkv_seekinfo si = {0};

	struct mstart sh = begin_master(fd, MKV_SEEKHEAD);
	  struct mstart sk1 = begin_master(fd, MKV_SEEK);
	    wuint(fd, MKV_SEEKID, MKV_INFO,        4);
	    wuint(fd, MKV_SEEKPOSITION, 10,         4);
	  end_master(fd, sk1);
	  struct mstart sk2 = begin_master(fd, MKV_SEEK);
	    wuint(fd, MKV_SEEKID, MKV_CLUSTER,     4);
	    wuint(fd, MKV_SEEKPOSITION, 20,         4);
	  end_master(fd, sk2);
	  struct mstart sk3 = begin_master(fd, MKV_SEEK);
	    wuint(fd, MKV_SEEKID, MKV_TRACKS,      4);
	    wuint(fd, MKV_SEEKPOSITION, 30,         4);
	  end_master(fd, sk3);
	  struct mstart sk4 = begin_master(fd, MKV_SEEK);
	    wuint(fd, MKV_SEEKID, MKV_CUES,        4);
	    wuint(fd, MKV_SEEKPOSITION, 40,         4);
	  end_master(fd, sk4);
	  struct mstart sk5 = begin_master(fd, MKV_SEEK);
	    wuint(fd, MKV_SEEKID, MKV_ATTACHMENTS, 4);
	    wuint(fd, MKV_SEEKPOSITION, 50,         4);
	  end_master(fd, sk5);
	  struct mstart sk6 = begin_master(fd, MKV_SEEK);
	    wuint(fd, MKV_SEEKID, MKV_CHAPTERS,    4);
	    wuint(fd, MKV_SEEKPOSITION, 60,         4);
	  end_master(fd, sk6);
	  struct mstart sk7 = begin_master(fd, MKV_SEEK);
	    wuint(fd, MKV_SEEKID, MKV_TAGS,        4);
	    wuint(fd, MKV_SEEKPOSITION, 70,         4);
	  end_master(fd, sk7);
	end_master(fd, sh);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readseekinfo(fd, &si));
	ck_assert_int_eq(si.info,        10);
	ck_assert_int_eq(si.clusters,    20);
	ck_assert_int_eq(si.tracks,      30);
	ck_assert_int_eq(si.cues,        40);
	ck_assert_int_eq(si.attachments, 50);
	ck_assert_int_eq(si.chapters,    60);
	ck_assert_int_eq(si.tags,        70);

#test readseekinfo_segment_is_initial_position
	struct mkv_seekinfo si = {0};

	/* Offset the SeekHead from 0 so the segment field is non-trivial. */
	wbe(fd, 0, 8);
	struct mstart sh = begin_master(fd, MKV_SEEKHEAD);
	  struct mstart sk = begin_master(fd, MKV_SEEK);
	    wuint(fd, MKV_SEEKID, MKV_INFO, 4);
	    wuint(fd, MKV_SEEKPOSITION, 100, 4);
	  end_master(fd, sk);
	end_master(fd, sh);
	lseek(fd, 8, SEEK_SET);

	ck_assert(mkv_readseekinfo(fd, &si));
	ck_assert_int_eq(si.segment, 8);

#test readseekinfo_returns_false_on_unknown_seekid
	struct mkv_seekinfo si = {0};

	struct mstart sh = begin_master(fd, MKV_SEEKHEAD);
	  struct mstart sk = begin_master(fd, MKV_SEEK);
	    wuint(fd, MKV_SEEKID, 0x12345678, 4); /* not a known Matroska element ID */
	    wuint(fd, MKV_SEEKPOSITION, 99, 4);
	  end_master(fd, sk);
	end_master(fd, sh);
	lseek(fd, 0, SEEK_SET);

	ck_assert(!mkv_readseekinfo(fd, &si));

/* ================================================================== */
/* mkv_readinfo                                                        */
/* ================================================================== */

#tcase mkv_readinfo

#test readinfo_reads_timestamp_scale
	struct mkv_info info = {0};

	struct mstart m = begin_master(fd, MKV_INFO);
	  wuint(fd, MKV_TIMESTAMPSCALE, 500000, 4);
	end_master(fd, m);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readinfo(fd, &info));
	ck_assert_uint_eq(info.ts_scale, 500000);

#test readinfo_default_timestamp_scale
	struct mkv_info info = {0};

	struct mstart m = begin_master(fd, MKV_INFO);
	end_master(fd, m);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readinfo(fd, &info));
	ck_assert_uint_eq(info.ts_scale, 1000000);

#test readinfo_skips_unknown_elements
	struct mkv_info info = {0};

	struct mstart m = begin_master(fd, MKV_INFO);
	  wuint(fd, 0xEC, 0, 1); /* EBML Void — unknown in Info context */
	  wuint(fd, MKV_TIMESTAMPSCALE, 250000, 4);
	end_master(fd, m);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readinfo(fd, &info));
	ck_assert_uint_eq(info.ts_scale, 250000);

/* ================================================================== */
/* mkv_readtrackentry                                                  */
/* ================================================================== */

#tcase mkv_readtrackentry

#test readtrackentry_reads_full_audio_track
	struct mkv_track t = {0};

	struct mstart te = begin_master(fd, MKV_TRACKENTRY);
	  wuint(fd, MKV_TRACKNUMBER, 2,      1);
	  wuint(fd, MKV_TRACKUID,    0xDEAD, 4);
	  wuint(fd, MKV_TRACKTYPE,   AUDIO,  1);
	  wuint(fd, MKV_FLAGENABLED, 1,      1);
	  struct mstart au = begin_master(fd, MKV_AUDIO);
	    wfloat64(fd, MKV_SAMPLINGFREQUENCY, 44100.0);
	    wuint(fd, MKV_CHANNELS, 2,  1);
	    wuint(fd, MKV_BITDEPTH, 16, 1);
	  end_master(fd, au);
	end_master(fd, te);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readtrackentry(fd, &t));
	ck_assert_uint_eq(t.num,      2);
	ck_assert_uint_eq(t.uid,      0xDEAD);
	ck_assert_uint_eq(t.channels, 2);
	ck_assert_uint_eq(t.bps,      16);
	ck_assert(t.rate == 44100.0);
	ck_assert(t.enabled);
	ck_assert(t.type == AUDIO);

#test readtrackentry_defaults_for_missing_fields
	struct mkv_track t = {0};

	struct mstart te = begin_master(fd, MKV_TRACKENTRY);
	  wuint(fd, MKV_TRACKNUMBER, 1,     1);
	  wuint(fd, MKV_TRACKTYPE,   AUDIO, 1);
	end_master(fd, te);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readtrackentry(fd, &t));
	ck_assert_uint_eq(t.channels, 1);
	ck_assert(t.rate == 8000.0);
	ck_assert(t.enabled == 1);

#test readtrackentry_reads_track_type
	struct mkv_track t = {0};

	struct mstart te = begin_master(fd, MKV_TRACKENTRY);
	  wuint(fd, MKV_TRACKNUMBER, 1,     1);
	  wuint(fd, MKV_TRACKUID,    0x111, 2);
	  wuint(fd, MKV_TRACKTYPE,   VIDEO, 1);
	  wuint(fd, MKV_FLAGENABLED, 1,     1);
	end_master(fd, te);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readtrackentry(fd, &t));
	ck_assert(t.type == VIDEO);

#test readtrackentry_disabled_flag
	struct mkv_track t = {0};

	struct mstart te = begin_master(fd, MKV_TRACKENTRY);
	  wuint(fd, MKV_TRACKNUMBER, 1,     1);
	  wuint(fd, MKV_TRACKTYPE,   AUDIO, 1);
	  wuint(fd, MKV_FLAGENABLED, 0,     1);
	end_master(fd, te);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readtrackentry(fd, &t));
	ck_assert(!t.enabled);

/* ================================================================== */
/* mkv_readcuepoint                                                    */
/* ================================================================== */

#tcase mkv_readcuepoint

#test readcuepoint_reads_time_and_track_position
	struct mkv_cue cue = {0};

	struct mstart cp = begin_master(fd, MKV_CUEPOINT);
	  wuint(fd, MKV_CUETIME, 500, 4);
	  struct mstart ctp = begin_master(fd, MKV_CUETRACKPOSITIONS);
	    wuint(fd, MKV_CUETRACK,           1,    1);
	    wuint(fd, MKV_CUECLUSTERPOSITION, 1000, 4);
	  end_master(fd, ctp);
	end_master(fd, cp);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readcuepoint(fd, &cue));
	ck_assert_uint_eq(cue.time,          500);
	ck_assert_uint_eq(cue.tracks[0].num, 1);
	ck_assert_int_eq(cue.tracks[0].pos,  1000);

#test readcuepoint_relpos_defaults_to_minus_one
	struct mkv_cue cue = {0};

	struct mstart cp = begin_master(fd, MKV_CUEPOINT);
	  wuint(fd, MKV_CUETIME, 100, 4);
	  struct mstart ctp = begin_master(fd, MKV_CUETRACKPOSITIONS);
	    wuint(fd, MKV_CUETRACK,           1,   1);
	    wuint(fd, MKV_CUECLUSTERPOSITION, 200, 4);
	  end_master(fd, ctp);
	end_master(fd, cp);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readcuepoint(fd, &cue));
	ck_assert_int_eq(cue.tracks[0].relpos, -1);

#test readcuepoint_reads_relpos
	struct mkv_cue cue = {0};

	struct mstart cp = begin_master(fd, MKV_CUEPOINT);
	  wuint(fd, MKV_CUETIME, 200, 4);
	  struct mstart ctp = begin_master(fd, MKV_CUETRACKPOSITIONS);
	    wuint(fd, MKV_CUETRACK,            2,   1);
	    wuint(fd, MKV_CUECLUSTERPOSITION,  500, 4);
	    wuint(fd, MKV_CUERELATIVEPOSITION, 42,  2);
	  end_master(fd, ctp);
	end_master(fd, cp);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readcuepoint(fd, &cue));
	ck_assert_int_eq(cue.tracks[0].relpos, 42);

#test readcuepoint_reads_multiple_track_positions
	struct mkv_cue cue = {0};

	struct mstart cp = begin_master(fd, MKV_CUEPOINT);
	  wuint(fd, MKV_CUETIME, 300, 4);
	  struct mstart ctp1 = begin_master(fd, MKV_CUETRACKPOSITIONS);
	    wuint(fd, MKV_CUETRACK,           1,   1);
	    wuint(fd, MKV_CUECLUSTERPOSITION, 100, 4);
	  end_master(fd, ctp1);
	  struct mstart ctp2 = begin_master(fd, MKV_CUETRACKPOSITIONS);
	    wuint(fd, MKV_CUETRACK,           2,   1);
	    wuint(fd, MKV_CUECLUSTERPOSITION, 200, 4);
	  end_master(fd, ctp2);
	end_master(fd, cp);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readcuepoint(fd, &cue));
	ck_assert_uint_eq(cue.tracks[0].num, 1);
	ck_assert_int_eq(cue.tracks[0].pos,  100);
	ck_assert_uint_eq(cue.tracks[1].num, 2);
	ck_assert_int_eq(cue.tracks[1].pos,  200);

#test readcuepoint_returns_false_on_wrong_element
	struct mkv_cue cue = {0};

	struct mstart cl = begin_master(fd, MKV_CLUSTER);
	  wuint(fd, MKV_CLUSTERTIMESTAMP, 0, 4);
	end_master(fd, cl);
	lseek(fd, 0, SEEK_SET);

	ck_assert(!mkv_readcuepoint(fd, &cue));

/* ================================================================== */
/* mkv_readblock                                                       */
/* ================================================================== */

#tcase mkv_readblock

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

#test readblock_no_lacing_simpleblock
	uint8_t payload[] = { 0x01, 0x02, 0x03, 0x04 };
	struct mkv_block b;

	write_simpleblock(fd, 1, 0, payload, sizeof payload);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readblock(fd, &b));
	ck_assert_int_eq(b.frames_n,           1);
	ck_assert_int_eq((ssize_t)b.frames_sz, (ssize_t)sizeof payload);

#test readblock_no_lacing_block
	uint8_t payload[] = { 0xAA, 0xBB, 0xCC };
	struct mkv_block b;

	wid(fd, MKV_BLOCK);
	wlen(fd, 1 + 2 + 1 + sizeof payload);
	wbe(fd, 0x81, 1); /* track 1 as 1-byte VINT */
	wbe(fd, 0,    2); /* timecode */
	wbe(fd, 0,    1); /* flags: no lacing */
	write(fd, payload, sizeof payload);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readblock(fd, &b));
	ck_assert_int_eq(b.frames_n,           1);
	ck_assert_int_eq((ssize_t)b.frames_sz, (ssize_t)sizeof payload);

#test readblock_reads_track_number
	uint8_t payload[] = { 0x00 };
	struct mkv_block b;

	wid(fd, MKV_SIMPLEBLOCK);
	wlen(fd, 1 + 2 + 1 + sizeof payload);
	wbe(fd, 0x85, 1); /* track 5 as 1-byte VINT: 0x80|5 */
	wbe(fd, 0,    2);
	wbe(fd, 0,    1);
	write(fd, payload, sizeof payload);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readblock(fd, &b));
	ck_assert_uint_eq(vint_value(b.track), 5);

#test readblock_xiph_lacing
	/* 2 frames: sizes 5 and 3.  Total frame data = 8. */
	uint8_t data[8] = {0};
	struct mkv_block b;

	wid(fd, MKV_SIMPLEBLOCK);
	wlen(fd, 1 + 2 + 1 + 1 + 1 + sizeof data); /* 14 */
	wbe(fd, 0x81, 1); /* track 1 */
	wbe(fd, 0,    2); /* timecode */
	wbe(fd, 0x02, 1); /* flags: Xiph lacing */
	wbe(fd, 0x01, 1); /* frames_n-1 in file: 1 (2 frames) */
	wbe(fd, 0x05, 1); /* first frame size: 5 */
	write(fd, data, sizeof data);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readblock(fd, &b));
	ck_assert_int_eq(b.frames_n,           2);
	ck_assert_int_eq((ssize_t)b.frames_sz, 8);

#test readblock_fixed_size_lacing
	/* 3 frames of 4 bytes each.  Total = 12. */
	uint8_t data[12] = {0};
	struct mkv_block b;

	wid(fd, MKV_SIMPLEBLOCK);
	wlen(fd, 1 + 2 + 1 + 1 + sizeof data); /* 17 */
	wbe(fd, 0x81, 1); /* track 1 */
	wbe(fd, 0,    2);
	wbe(fd, 0x04, 1); /* flags: fixed-size lacing */
	wbe(fd, 0x02, 1); /* frames_n-1 in file: 2 (3 frames) */
	write(fd, data, sizeof data);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readblock(fd, &b));
	ck_assert_int_eq(b.frames_n,           3);
	ck_assert_int_eq((ssize_t)b.frames_sz, 12);

#test readblock_ebml_lacing
	/* 3 frames: sizes 4, 6, 8.  Total = 18.
	   Diff VINT for frame 2: stored = (6-4) + bias(63) = 65 → 0xC1. */
	uint8_t data[18] = {0};
	struct mkv_block b;

	wid(fd, MKV_SIMPLEBLOCK);
	wlen(fd, 1 + 2 + 1 + 1 + 1 + 1 + sizeof data); /* 25 */
	wbe(fd, 0x81, 1); /* track 1 */
	wbe(fd, 0,    2);
	wbe(fd, 0x06, 1); /* flags: EBML lacing */
	wbe(fd, 0x02, 1); /* frames_n-1 in file: 2 (3 frames) */
	wbe(fd, 0x84, 1); /* first frame size: 4 as 1-byte VINT */
	wbe(fd, 0xC1, 1); /* diff VINT: value=65, decoded size = 65-63+4 = 6 */
	write(fd, data, sizeof data);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readblock(fd, &b));
	ck_assert_int_eq(b.frames_n,           3);
	ck_assert_int_eq((ssize_t)b.frames_sz, 18);

#test readblock_returns_false_on_wrong_element
	struct mkv_block b;

	struct mstart cl = begin_master(fd, MKV_CLUSTER);
	  wuint(fd, MKV_CLUSTERTIMESTAMP, 0, 4);
	end_master(fd, cl);
	lseek(fd, 0, SEEK_SET);

	ck_assert(!mkv_readblock(fd, &b));

/* ================================================================== */
/* mkv_readchapteratom                                                 */
/* ================================================================== */

#tcase mkv_readchapteratom

#test readchapteratom_reads_all_fields
	struct mkv_chapter c;

	struct mstart ca = begin_master(fd, MKV_CHAPTERATOM);
	  wuint(fd, MKV_CHAPTERUID,       0x1234, 4);
	  wuint(fd, MKV_CHAPTERTIMESTART,  1000,  4);
	  wuint(fd, MKV_CHAPTERTIMEEND,    2000,  4);
	  struct mstart ct = begin_master(fd, MKV_CHAPTERTRACK);
	    wuint(fd, MKV_CHAPTERTRACKUID, 0xABCD, 4);
	  end_master(fd, ct);
	end_master(fd, ca);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readchapteratom(fd, &c));
	ck_assert_uint_eq(c.uid,           0x1234);
	ck_assert_uint_eq(c.start,         1000);
	ck_assert_uint_eq(c.end,           2000);
	ck_assert_uint_eq(c.track_uids[0], 0xABCD);

#test readchapteratom_end_defaults_to_zero
	struct mkv_chapter c;

	struct mstart ca = begin_master(fd, MKV_CHAPTERATOM);
	  wuint(fd, MKV_CHAPTERUID,      0x5678, 4);
	  wuint(fd, MKV_CHAPTERTIMESTART, 3000,  4);
	end_master(fd, ca);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readchapteratom(fd, &c));
	ck_assert_uint_eq(c.end, 0);

#test readchapteratom_reads_multiple_track_uids
	struct mkv_chapter c;

	struct mstart ca = begin_master(fd, MKV_CHAPTERATOM);
	  wuint(fd, MKV_CHAPTERUID,      0x9999, 4);
	  wuint(fd, MKV_CHAPTERTIMESTART, 0,     4);
	  struct mstart ct = begin_master(fd, MKV_CHAPTERTRACK);
	    wuint(fd, MKV_CHAPTERTRACKUID, 0x0001, 4);
	    wuint(fd, MKV_CHAPTERTRACKUID, 0x0002, 4);
	  end_master(fd, ct);
	end_master(fd, ca);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readchapteratom(fd, &c));
	ck_assert_uint_eq(c.track_uids[0], 0x0001);
	ck_assert_uint_eq(c.track_uids[1], 0x0002);

#test readchapteratom_all_track_uids_zero_when_absent
	struct mkv_chapter c;

	struct mstart ca = begin_master(fd, MKV_CHAPTERATOM);
	  wuint(fd, MKV_CHAPTERUID,      0xAAAA, 4);
	  wuint(fd, MKV_CHAPTERTIMESTART, 0,     4);
	end_master(fd, ca);
	lseek(fd, 0, SEEK_SET);

	ck_assert(mkv_readchapteratom(fd, &c));
	for (int i = 0; i < TRACKS_MAX; i++)
	    ck_assert_uint_eq(c.track_uids[i], 0);

#test readchapteratom_returns_false_on_wrong_element
	struct mkv_chapter c;

	struct mstart cl = begin_master(fd, MKV_CLUSTER);
	  wuint(fd, MKV_CLUSTERTIMESTAMP, 0, 4);
	end_master(fd, cl);
	lseek(fd, 0, SEEK_SET);

	ck_assert(!mkv_readchapteratom(fd, &c));

#main-pre
	tcase_add_checked_fixture(tc1_1, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_2, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_3, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_4, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_5, fd_setup, fd_teardown);
	tcase_add_checked_fixture(tc1_6, fd_setup, fd_teardown);
