#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <stddef.h>
#include "ebml.h"
#include "matroska.h"
#include "tags.h"

#suite tags

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

/* Write a UTF-8 string element. */
static void
wstring(int fd, uint32_t id, const char *s)
{
	size_t len = strlen(s);
	wid(fd, id);
	wlen(fd, len);
	write(fd, s, len);
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
	fd = shm_open("tags_tests", O_RDWR|O_CREAT, 0600);
	ck_assert_int_ge(fd, 0);
}

void
fd_teardown(void)
{
	close(fd);
	shm_unlink("tags_tests");
}

/* ================================================================== */
/* mkv_readsongtags                                                    */
/* ================================================================== */

#tcase mkv_readsongtags

#test readsongtags_album_tags_apply_to_all_songs
	struct song_tags tags = {0};

	struct mstart ts = begin_master(fd, MKV_TAGS);
	  struct mstart t1 = begin_master(fd, MKV_TAG);
	    struct mstart tgt = begin_master(fd, MKV_TARGETS);
	      wuint(fd, MKV_TARGETTYPEVALUE, 50, 1);
	    end_master(fd, tgt);
	    struct mstart st = begin_master(fd, MKV_SIMPLETAG);
	      wstring(fd, MKV_TAGNAME,   "ARTIST");
	      wstring(fd, MKV_TAGSTRING, "The Band");
	    end_master(fd, st);
	  end_master(fd, t1);
	end_master(fd, ts);
	lseek(fd, 0, SEEK_SET);

	mkv_readsongtags(fd, 0xAA, 0x01, 0, &tags);
	ck_assert_uint_eq(tags.fields[TAG_ARTIST].count, 1);
	ck_assert_str_eq(tags.fields[TAG_ARTIST].vals[0], "The Band");
	song_tags_free(&tags);

#test readsongtags_track_uid_scopes_tags_to_matching_track
	/* Two tracks with distinct artists; verify only the matching one appears. */
	struct song_tags tags = {0};

	struct mstart ts = begin_master(fd, MKV_TAGS);
	  /* Tag targeting track 0xAA (disc 1). */
	  struct mstart t1 = begin_master(fd, MKV_TAG);
	    struct mstart tgt1 = begin_master(fd, MKV_TARGETS);
	      wuint(fd, MKV_TARGETTYPEVALUE, 30,   1);
	      wuint(fd, MKV_TAGTRACKUID,    0xAA,  4);
	    end_master(fd, tgt1);
	    struct mstart st1 = begin_master(fd, MKV_SIMPLETAG);
	      wstring(fd, MKV_TAGNAME,   "ARTIST");
	      wstring(fd, MKV_TAGSTRING, "Artist A");
	    end_master(fd, st1);
	  end_master(fd, t1);
	  /* Tag targeting track 0xBB (disc 2). */
	  struct mstart t2 = begin_master(fd, MKV_TAG);
	    struct mstart tgt2 = begin_master(fd, MKV_TARGETS);
	      wuint(fd, MKV_TARGETTYPEVALUE, 30,   1);
	      wuint(fd, MKV_TAGTRACKUID,    0xBB,  4);
	    end_master(fd, tgt2);
	    struct mstart st2 = begin_master(fd, MKV_SIMPLETAG);
	      wstring(fd, MKV_TAGNAME,   "ARTIST");
	      wstring(fd, MKV_TAGSTRING, "Artist B");
	    end_master(fd, st2);
	  end_master(fd, t2);
	end_master(fd, ts);
	lseek(fd, 0, SEEK_SET);

	/* Reading for track 0xBB: must see only "Artist B". */
	mkv_readsongtags(fd, 0, 0xBB, 0, &tags);
	ck_assert_uint_eq(tags.fields[TAG_ARTIST].count, 1);
	ck_assert_str_eq(tags.fields[TAG_ARTIST].vals[0], "Artist B");
	song_tags_free(&tags);

#test readsongtags_chapter_tag_takes_priority_over_track_tag
	/* Chapter-level title overrides a track-level title. */
	struct song_tags tags = {0};

	struct mstart ts = begin_master(fd, MKV_TAGS);
	  /* Track-level tag. */
	  struct mstart t1 = begin_master(fd, MKV_TAG);
	    struct mstart tgt1 = begin_master(fd, MKV_TARGETS);
	      wuint(fd, MKV_TARGETTYPEVALUE, 30,   1);
	      wuint(fd, MKV_TAGTRACKUID,    0xAA,  4);
	    end_master(fd, tgt1);
	    struct mstart st1 = begin_master(fd, MKV_SIMPLETAG);
	      wstring(fd, MKV_TAGNAME,   "TITLE");
	      wstring(fd, MKV_TAGSTRING, "Track Title");
	    end_master(fd, st1);
	  end_master(fd, t1);
	  /* Chapter-level tag — takes priority. */
	  struct mstart t2 = begin_master(fd, MKV_TAG);
	    struct mstart tgt2 = begin_master(fd, MKV_TARGETS);
	      wuint(fd, MKV_TAGCHAPTERUID, 0xCC, 4);
	    end_master(fd, tgt2);
	    struct mstart st2 = begin_master(fd, MKV_SIMPLETAG);
	      wstring(fd, MKV_TAGNAME,   "TITLE");
	      wstring(fd, MKV_TAGSTRING, "Chapter Title");
	    end_master(fd, st2);
	  end_master(fd, t2);
	end_master(fd, ts);
	lseek(fd, 0, SEEK_SET);

	mkv_readsongtags(fd, 0xCC, 0xAA, 0, &tags);
	ck_assert_uint_eq(tags.fields[TAG_TITLE].count, 1);
	ck_assert_str_eq(tags.fields[TAG_TITLE].vals[0], "Chapter Title");
	song_tags_free(&tags);

#test readsongtags_disc_uses_matching_edition_uid
	struct song_tags tags = {0};

	struct mstart ts = begin_master(fd, MKV_TAGS);
	  struct mstart t1 = begin_master(fd, MKV_TAG);
	    struct mstart tgt1 = begin_master(fd, MKV_TARGETS);
	      wuint(fd, MKV_TAGEDITIONUID, 0x11, 1);
	    end_master(fd, tgt1);
	    struct mstart st1 = begin_master(fd, MKV_SIMPLETAG);
	      wstring(fd, MKV_TAGNAME,   "PART_NUMBER");
	      wstring(fd, MKV_TAGSTRING, "1");
	    end_master(fd, st1);
	  end_master(fd, t1);
	  struct mstart t2 = begin_master(fd, MKV_TAG);
	    struct mstart tgt2 = begin_master(fd, MKV_TARGETS);
	      wuint(fd, MKV_TAGEDITIONUID, 0x22, 1);
	    end_master(fd, tgt2);
	    struct mstart st2 = begin_master(fd, MKV_SIMPLETAG);
	      wstring(fd, MKV_TAGNAME,   "PART_NUMBER");
	      wstring(fd, MKV_TAGSTRING, "2");
	    end_master(fd, st2);
	  end_master(fd, t2);
	end_master(fd, ts);
	lseek(fd, 0, SEEK_SET);

	mkv_readsongtags(fd, 0, 0, 0x22, &tags);
	ck_assert_uint_eq(tags.fields[TAG_DISC].count, 1);
	ck_assert_str_eq(tags.fields[TAG_DISC].vals[0], "2");
	song_tags_free(&tags);

#main-pre
	tcase_add_checked_fixture(tc1_1, fd_setup, fd_teardown);
