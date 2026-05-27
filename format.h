/* Include: matroska.h. */
#pragma once

/* Opaque compiled format string. */
struct format;

/* Parse and validate a format string.
   Returns NULL and writes to stderr if any {field} name is unknown
   or a '{' is unterminated.  Caller must format_free(). */
struct format *format_compile(const char *fmt);

/* Write the formatted line for `tags` to `fd`, followed by '\n'.
   Builds the entire line into a heap buffer, then issues a single
   write(2).  Multi-valued fields are joined with ", ". */
void format_print(const struct format *, const struct song_tags *, int fd);

/* Safe on NULL. */
void format_free(struct format *);
