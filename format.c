#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "ebml.h"
#include "matroska.h"
#include "format.h"

/* ── Internal segment types ────────────────────────────── */

enum seg_type { SEG_LITERAL, SEG_FIELD };

struct seg {
	enum seg_type type;
	union {
		char          *literal; /* heap-allocated, null-terminated */
		enum tag_field field;
	};
};

struct format {
	struct seg *segs;
	size_t      count;
};

/* ── Growable byte buffer ───────────────────────────────── */

static int
buf_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
	if (*len + n > *cap) {
		size_t ncap = (*cap == 0) ? 64 : *cap * 2;
		while (ncap < *len + n) ncap *= 2;
		char *nb = realloc(*buf, ncap);
		if (!nb) return 0;
		*buf = nb;
		*cap = ncap;
	}
	memcpy(*buf + *len, s, n);
	*len += n;
	return 1;
}

/* ── Segment array helpers ─────────────────────────────── */

static int
push_literal(struct format *f, const char *s, size_t len)
{
	if (len == 0) return 1; /* nothing to push */

	struct seg *ns = realloc(f->segs, (f->count + 1) * sizeof *ns);
	if (!ns) return 0;
	f->segs = ns;

	char *copy = malloc(len + 1);
	if (!copy) return 0;
	memcpy(copy, s, len);
	copy[len] = '\0';

	f->segs[f->count].type    = SEG_LITERAL;
	f->segs[f->count].literal = copy;
	f->count++;
	return 1;
}

static int
push_field(struct format *f, enum tag_field field)
{
	struct seg *ns = realloc(f->segs, (f->count + 1) * sizeof *ns);
	if (!ns) return 0;
	f->segs = ns;
	f->segs[f->count].type  = SEG_FIELD;
	f->segs[f->count].field = field;
	f->count++;
	return 1;
}

/* ── format_compile ────────────────────────────────────── */

struct format *
format_compile(const char *fmt)
{
	struct format *f = calloc(1, sizeof *f);
	if (!f) return NULL;

	/* Accumulate literal characters between fields. */
	char  *litbuf = NULL;
	size_t litlen = 0, litcap = 0;

	for (const char *p = fmt; *p; ) {
		if (*p == '\\' && *(p + 1)) {
			/* Escaped character: next char is literal. */
			if (!buf_append(&litbuf, &litlen, &litcap, p + 1, 1))
				goto oom;
			p += 2;
		} else if (*p == '{') {
			/* Flush pending literal. */
			if (!push_literal(f, litbuf, litlen))
				goto oom;
			litlen = 0;

			/* Find closing brace. */
			const char *close = strchr(p + 1, '}');
			if (!close) {
				fprintf(stderr, "sp: unterminated '{' in format string\n");
				goto err;
			}

			/* Extract field name. */
			size_t namelen = (size_t)(close - (p + 1));
			char name[32];
			if (namelen >= sizeof name) {
				fprintf(stderr, "sp: field name too long\n");
				goto err;
			}
			memcpy(name, p + 1, namelen);
			name[namelen] = '\0';

			/* Map to enum tag_field. */
			enum tag_field field;
			if      (strcmp(name, "date")      == 0) field = TAG_DATE;
			else if (strcmp(name, "orig-date") == 0) field = TAG_ORIG_DATE;
			else if (strcmp(name, "artist")    == 0) field = TAG_ARTIST;
			else if (strcmp(name, "title")     == 0) field = TAG_TITLE;
			else if (strcmp(name, "genre")     == 0) field = TAG_GENRE;
			else if (strcmp(name, "album")     == 0) field = TAG_ALBUM;
			else if (strcmp(name, "track")     == 0) field = TAG_TRACK;
			else {
				fprintf(stderr, "sp: unknown field: %s\n", name);
				goto err;
			}

			if (!push_field(f, field))
				goto oom;

			p = close + 1;
		} else {
			if (!buf_append(&litbuf, &litlen, &litcap, p, 1))
				goto oom;
			p++;
		}
	}

	/* Flush any remaining literal. */
	if (!push_literal(f, litbuf, litlen))
		goto oom;

	free(litbuf);
	return f;

oom:
	fprintf(stderr, "sp: out of memory\n");
err:
	free(litbuf);
	format_free(f);
	return NULL;
}

/* ── format_print ──────────────────────────────────────── */

void
format_print(const struct format *f, const struct song_tags *tags, int fd)
{
	char  *buf = NULL;
	size_t len = 0, cap = 0;

	for (size_t i = 0; i < f->count; i++) {
		const struct seg *s = &f->segs[i];
		if (s->type == SEG_LITERAL) {
			buf_append(&buf, &len, &cap,
			           s->literal, strlen(s->literal));
		} else {
			const struct tag_values *tv = &tags->fields[s->field];
			for (size_t j = 0; j < tv->count; j++) {
				if (j > 0)
					buf_append(&buf, &len, &cap, ", ", 2);
				buf_append(&buf, &len, &cap,
				           tv->vals[j], tv->lens[j]);
			}
		}
	}
	buf_append(&buf, &len, &cap, "\n", 1);

	if (buf) {
		(void) write(fd, buf, len);
		free(buf);
	}
}

/* ── format_free ───────────────────────────────────────── */

void
format_free(struct format *f)
{
	if (!f) return;
	for (size_t i = 0; i < f->count; i++)
		if (f->segs[i].type == SEG_LITERAL)
			free(f->segs[i].literal);
	free(f->segs);
	free(f);
}
