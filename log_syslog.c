#include <stdarg.h>  /* va_list(3type). */
#include <stdio.h>   /* vsnprintf(3). */
#include <string.h>  /* strerror(3). */
#include <syslog.h>  /* syslog(3). */

void
log_msg(int priority, int err, char *msg, va_list args)
{
	char buf[512];
	vsnprintf(buf, sizeof buf, msg, args);
	if (err)
		syslog(priority, "%s: %s", buf, strerror(err));
	else
		syslog(priority, "%s", buf);
}
