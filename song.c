#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "song.h"
#include "utils.h"

bool parse_song(const char *line, struct song *dest)
{
	char *end = strchr(line, '#');
	if (!end)
		return false;

	dest->path = malloc((end - line + 1) * sizeof(char));
	if (!dest->path)
		return false;
	strncpy(dest->path, line, end - line);
	
	if (!isdigit(*++end))
		goto err;

	dest->uid = strtoul(end, &end, 10);
	if (dest->uid == ULONG_MAX) {
		debug(errno, "strtoul");
		goto err;
	} else if (*end != '\0') {
		goto err;
	}

	return true;

err:
	free(dest->path);
	return false;
}
