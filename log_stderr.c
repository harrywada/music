#include <stdarg.h>  /* va_list(3type). */
#include <stdio.h>   /* vsnprintf(3), fprintf(3), stderr(3). */
#include <string.h>  /* strerror(3). */

void
log_msg(int priority, int err, char *msg, va_list args)
{
	char buf[512];
	(void)priority;
	vsnprintf(buf, sizeof buf, msg, args);
	if (err)
		fprintf(stderr, "%s: %s\n", buf, strerror(err));
	else
		fprintf(stderr, "%s\n", buf);
}
