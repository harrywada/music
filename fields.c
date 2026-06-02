#include <string.h>
#include "fields.h"

bool
sp_field_name(const char *name, enum tag_field *field)
{
	if      (strcmp(name, "date")      == 0) *field = TAG_DATE;
	else if (strcmp(name, "orig-date") == 0) *field = TAG_ORIG_DATE;
	else if (strcmp(name, "artist")    == 0) *field = TAG_ARTIST;
	else if (strcmp(name, "title")     == 0) *field = TAG_TITLE;
	else if (strcmp(name, "genre")     == 0) *field = TAG_GENRE;
	else if (strcmp(name, "album")     == 0) *field = TAG_ALBUM;
	else if (strcmp(name, "#")         == 0) *field = TAG_TRACK;
	else if (strcmp(name, "disc")      == 0) *field = TAG_DISC;
	else return false;
	return true;
}
