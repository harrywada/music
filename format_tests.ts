#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "tags.h"
#include "format.h"

/* ── Test helpers ──────────────────────────────────────── */

static void
tags_set(struct song_tags *t, enum tag_field f, const char *val)
{
	t->fields[f].vals    = malloc(sizeof(char *));
	t->fields[f].vals[0] = strdup(val);
	t->fields[f].lens    = malloc(sizeof(size_t));
	t->fields[f].lens[0] = strlen(val);
	t->fields[f].count   = 1;
}

static void
tags_add(struct song_tags *t, enum tag_field f, const char *val)
{
	size_t n = t->fields[f].count + 1;
	t->fields[f].vals = realloc(t->fields[f].vals, n * sizeof(char *));
	t->fields[f].lens = realloc(t->fields[f].lens, n * sizeof(size_t));
	t->fields[f].vals[n - 1] = strdup(val);
	t->fields[f].lens[n - 1] = strlen(val);
	t->fields[f].count = n;
}

/* Read format_print output via a pipe into a null-terminated buffer.
   Returns heap-allocated string; caller must free(). */
static char *
capture_print(const struct format *fmt, const struct song_tags *tags)
{
	int pipefd[2];
	if (pipe(pipefd) == -1) return NULL;
	format_print(fmt, tags, pipefd[1]);
	close(pipefd[1]);

	char  *buf = NULL;
	size_t len = 0;
	char   tmp[256];
	ssize_t n;
	while ((n = read(pipefd[0], tmp, sizeof tmp)) > 0) {
		buf = realloc(buf, len + (size_t)n + 1);
		memcpy(buf + len, tmp, (size_t)n);
		len += (size_t)n;
	}
	close(pipefd[0]);
	if (!buf) buf = strdup("");
	buf[len] = '\0';
	return buf;
}

#suite format

/* ── format_compile ────────────────────────────────────── */

#tcase compile

#test compile_literal_only
	struct format *f = format_compile("hello world");
	ck_assert_ptr_nonnull(f);
	format_free(f);

#test compile_single_field
	struct format *f = format_compile("{title}");
	ck_assert_ptr_nonnull(f);
	format_free(f);

#test compile_all_fields
	const char *fmts[] = {
		"{date}", "{orig-date}", "{artist}", "{title}",
		"{genre}", "{album}", "{#}",
	};
	for (int i = 0; i < 7; i++) {
		struct format *f = format_compile(fmts[i]);
		ck_assert_ptr_nonnull(f);
		format_free(f);
	}

#test compile_escaped_open_brace
	/* \{title} → literal "{title}", no substitution. */
	struct format *f = format_compile("\\{title}");
	ck_assert_ptr_nonnull(f);
	struct song_tags t = {0};
	tags_set(&t, TAG_TITLE, "Ignored");
	char *out = capture_print(f, &t);
	ck_assert_str_eq(out, "{title}\n");
	free(out); format_free(f); song_tags_free(&t);

#test compile_escaped_backslash
	/* \\{title} → literal "\" followed by field substitution. */
	struct format *f = format_compile("\\\\{title}");
	ck_assert_ptr_nonnull(f);
	struct song_tags t = {0};
	tags_set(&t, TAG_TITLE, "Shine On");
	char *out = capture_print(f, &t);
	ck_assert_str_eq(out, "\\Shine On\n");
	free(out); format_free(f); song_tags_free(&t);

#test compile_unknown_field_error
	struct format *f = format_compile("{bogus}");
	ck_assert_ptr_null(f);

#test compile_old_track_field_error
	struct format *f = format_compile("{track}");
	ck_assert_ptr_null(f);

#test compile_unterminated_brace
	struct format *f = format_compile("{title");
	ck_assert_ptr_null(f);

#test compile_empty_string
	struct format *f = format_compile("");
	ck_assert_ptr_nonnull(f);
	format_free(f);

/* ── format_print ──────────────────────────────────────── */

#tcase print

#test print_literal_only
	struct format *f = format_compile("no substitution");
	ck_assert_ptr_nonnull(f);
	struct song_tags t = {0};
	char *out = capture_print(f, &t);
	ck_assert_str_eq(out, "no substitution\n");
	free(out); format_free(f);

#test print_field_present
	struct format *f = format_compile("{artist}");
	struct song_tags t = {0};
	tags_set(&t, TAG_ARTIST, "Bach");
	char *out = capture_print(f, &t);
	ck_assert_str_eq(out, "Bach\n");
	free(out); format_free(f); song_tags_free(&t);

#test print_field_missing
	struct format *f = format_compile("{artist}");
	struct song_tags t = {0};
	char *out = capture_print(f, &t);
	ck_assert_str_eq(out, "\n");
	free(out); format_free(f);

#test print_multi_value_joined
	struct format *f = format_compile("{artist}");
	struct song_tags t = {0};
	tags_set(&t, TAG_ARTIST, "Mozart");
	tags_add(&t, TAG_ARTIST, "Bach");
	char *out = capture_print(f, &t);
	ck_assert_str_eq(out, "Mozart, Bach\n");
	free(out); format_free(f); song_tags_free(&t);

#test print_escaped_brace
	struct format *f = format_compile("\\{title}");
	struct song_tags t = {0};
	tags_set(&t, TAG_TITLE, "Ignored");
	char *out = capture_print(f, &t);
	ck_assert_str_eq(out, "{title}\n");
	free(out); format_free(f); song_tags_free(&t);

#test print_mixed_literal_and_field
	struct format *f = format_compile("{album} - {title}");
	struct song_tags t = {0};
	tags_set(&t, TAG_ALBUM, "Wish You Were Here");
	tags_set(&t, TAG_TITLE, "Shine On");
	char *out = capture_print(f, &t);
	ck_assert_str_eq(out, "Wish You Were Here - Shine On\n");
	free(out); format_free(f); song_tags_free(&t);

#test print_all_fields
	struct format *f = format_compile(
	    "{date}|{orig-date}|{artist}|{title}|{genre}|{album}|{#}");
	struct song_tags t = {0};
	tags_set(&t, TAG_DATE,      "1965");
	tags_set(&t, TAG_ORIG_DATE, "1960");
	tags_set(&t, TAG_ARTIST,    "Guaraldi");
	tags_set(&t, TAG_TITLE,     "Linus and Lucy");
	tags_set(&t, TAG_GENRE,     "Jazz");
	tags_set(&t, TAG_ALBUM,     "A Charlie Brown Christmas");
	tags_set(&t, TAG_TRACK,     "4");
	char *out = capture_print(f, &t);
	ck_assert_str_eq(out,
	    "1965|1960|Guaraldi|Linus and Lucy|Jazz"
	    "|A Charlie Brown Christmas|4\n");
	free(out); format_free(f); song_tags_free(&t);
