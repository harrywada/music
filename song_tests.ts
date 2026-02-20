#include "song.h"

#suite struct song

struct song song;

#tcase parse_song

const struct {
	char line[16];
	char path[16];
	long uid;
} parse_song_works_cases[] = {
	{ "/a/b/c.mka#123", "/a/b/c.mka", 123 },
	{ "/a/b/c.mka#000", "/a/b/c.mka", 0 },
};

#test-loop(0, 2) parse_song_works
	const char *line = parse_song_works_cases[_i].line;
	const char *path = parse_song_works_cases[_i].path;
	const long uid = parse_song_works_cases[_i].uid;

	ck_assert(parse_song(line, &song));
	ck_assert_str_eq(song.path, path);
	ck_assert_int_eq(song.uid, uid);

#test parse_song_rejects_negative_uids
	const char line[] = "/a/b/c.mka#-456";
	ck_assert(!parse_song(line, &song));

#test parse_song_rejects_alpha_uids
	const char line[] = "/a/b/c.mka#abc";
	ck_assert(!parse_song(line, &song));

#test parse_song_rejects_empty_strings
	const char line[] = "\0";
	ck_assert(!parse_song(line, &song));

#test parse_song_rejects_empty_uids
	const char line[] = "/a/b/c.mka#";
	ck_assert(!parse_song(line, &song));

#test parse_song_rejects_missing_uids
	const char line[] = "/a/b/c.mka";
	ck_assert(!parse_song(line, &song));
