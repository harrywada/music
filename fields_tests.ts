#include "fields.h"

#suite fields

#tcase sp_field_name

#test accepts_sp_fields
	const char *names[] = {
		"date", "orig-date", "artist", "title",
		"genre", "album", "#", "disc",
	};
	enum tag_field fields[] = {
		TAG_DATE, TAG_ORIG_DATE, TAG_ARTIST, TAG_TITLE,
		TAG_GENRE, TAG_ALBUM, TAG_TRACK, TAG_DISC,
	};

	for (int i = 0; i < 8; i++) {
		enum tag_field got;
		ck_assert(sp_field_name(names[i], &got));
		ck_assert_int_eq(got, fields[i]);
	}

#test rejects_unknown_fields
	enum tag_field got;
	ck_assert(!sp_field_name("track", &got));
	ck_assert(!sp_field_name("bogus", &got));
