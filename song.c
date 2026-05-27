#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "song.h"
#include "utils.h"

bool parse_song(const char *line, struct song *s)
{
	char *end = strchr(line, '#');
	if (!end)
		return false;

	s->path = malloc((end - line + 1) * sizeof(char));
	if (!s->path)
		return false;
	strncpy(s->path, line, end - line);
	
	if (!isdigit(*++end))
		goto err;

	s->uid = strtoul(end, &end, 10);
	if (s->uid == ULONG_MAX) {
		debug(errno, "strtoul");
		goto err;
	} else if (*end != '\0') {
		goto err;
	}

	return true;

err:
	free(s->path);
	return false;
}

void
cleanup_song(struct song *s)
{
	free(s->path);
}
