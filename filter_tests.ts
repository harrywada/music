#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "ebml.h"
#include "matroska.h"
#include "filter.h"

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

#suite filter

/* ── filter_parse ──────────────────────────────────────── */

#tcase parse

#test parse_empty_returns_null_no_error
	int err = 0;
	const char *dummy[] = { NULL };
	ck_assert_ptr_null(filter_parse(0, dummy, &err));
	ck_assert_int_eq(err, 0);

#test parse_single_eq
	int err = 0;
	const char *argv[] = { "artist", "=", "Bach" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert_int_eq(err, 0);
	ck_assert_ptr_nonnull(n);
	ck_assert_int_eq(n->type, FN_PRED);
	ck_assert_int_eq(n->pred.field, TAG_ARTIST);
	ck_assert_int_eq(n->pred.op, FOP_EQ);
	ck_assert_str_eq(n->pred.value, "Bach");
	filter_free(n);

#test parse_all_fields
	const char *fields[] = {
		"date", "orig-date", "artist", "title", "genre", "album", "track",
	};
	enum tag_field expected[] = {
		TAG_DATE, TAG_ORIG_DATE, TAG_ARTIST, TAG_TITLE,
		TAG_GENRE, TAG_ALBUM, TAG_TRACK,
	};
	for (int i = 0; i < 7; i++) {
		int err = 0;
		const char *argv[] = { fields[i], "=",
		    /* valid value for each type */
		    (i == 0 || i == 1) ? "2012" : (i == 6) ? "1" : "x" };
		struct filter_node *n = filter_parse(3, argv, &err);
		ck_assert_int_eq(err, 0);
		ck_assert_ptr_nonnull(n);
		ck_assert_int_eq(n->pred.field, expected[i]);
		filter_free(n);
	}

#test parse_and
	int err = 0;
	const char *argv[] = { "artist", "=", "A", "and", "genre", "=", "B" };
	struct filter_node *n = filter_parse(7, argv, &err);
	ck_assert_int_eq(err, 0);
	ck_assert_ptr_nonnull(n);
	ck_assert_int_eq(n->type, FN_AND);
	filter_free(n);

#test parse_or
	int err = 0;
	const char *argv[] = { "artist", "=", "A", "or", "genre", "=", "B" };
	struct filter_node *n = filter_parse(7, argv, &err);
	ck_assert_int_eq(err, 0);
	ck_assert_int_eq(n->type, FN_OR);
	filter_free(n);

#test parse_not
	int err = 0;
	const char *argv[] = { "not", "artist", "=", "A" };
	struct filter_node *n = filter_parse(4, argv, &err);
	ck_assert_int_eq(err, 0);
	ck_assert_int_eq(n->type, FN_NOT);
	filter_free(n);

#test parse_parens
	int err = 0;
	const char *argv[] = { "(", "artist", "=", "A", ")" };
	struct filter_node *n = filter_parse(5, argv, &err);
	ck_assert_int_eq(err, 0);
	ck_assert_ptr_nonnull(n);
	ck_assert_int_eq(n->type, FN_PRED);
	filter_free(n);

#test parse_error_unknown_field
	int err = 0;
	const char *argv[] = { "bogus", "=", "x" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert_ptr_null(n);
	ck_assert_int_eq(err, 1);

#test parse_error_bad_date
	int err = 0;
	const char *argv[] = { "date", "=", "notadate" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert_ptr_null(n);
	ck_assert_int_eq(err, 1);

#test parse_error_date_regex
	int err = 0;
	const char *argv[] = { "date", "~", "2012" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert_ptr_null(n);
	ck_assert_int_eq(err, 1);

#test parse_error_bad_track_int
	int err = 0;
	const char *argv[] = { "track", "=", "foo" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert_ptr_null(n);
	ck_assert_int_eq(err, 1);

#test parse_error_track_regex
	int err = 0;
	const char *argv[] = { "track", "~", "5" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert_ptr_null(n);
	ck_assert_int_eq(err, 1);

#test parse_error_incomplete
	int err = 0;
	const char *argv[] = { "artist", "=" };
	struct filter_node *n = filter_parse(2, argv, &err);
	ck_assert_ptr_null(n);
	ck_assert_int_eq(err, 1);

/* ── filter_eval ───────────────────────────────────────── */

#tcase eval

#test eval_null_filter_always_true
	struct song_tags t = {0};
	ck_assert(filter_eval(NULL, &t));

#test eval_eq_match
	struct song_tags t = {0};
	tags_set(&t, TAG_ARTIST, "Van Morrison");
	int err = 0;
	const char *argv[] = { "artist", "=", "Van Morrison" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_eq_case_insensitive
	struct song_tags t = {0};
	tags_set(&t, TAG_ARTIST, "Van Morrison");
	int err = 0;
	const char *argv[] = { "artist", "=", "van morrison" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_eq_no_match
	struct song_tags t = {0};
	tags_set(&t, TAG_ARTIST, "Van Morrison");
	int err = 0;
	const char *argv[] = { "artist", "=", "Bob Dylan" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(!filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_missing_field_fails
	struct song_tags t = {0};
	int err = 0;
	const char *argv[] = { "artist", "=", "Bach" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(!filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_neq_universal_negative
	struct song_tags t = {0};
	tags_set(&t, TAG_ARTIST, "Bach");
	int err = 0;
	const char *argv[] = { "artist", "!=", "Mozart" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_neq_match_fails
	struct song_tags t = {0};
	tags_set(&t, TAG_ARTIST, "Bach");
	int err = 0;
	const char *argv[] = { "artist", "!=", "Bach" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(!filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_multi_value_existential
	struct song_tags t = {0};
	tags_set(&t, TAG_ARTIST, "Mozart");
	tags_add(&t, TAG_ARTIST, "Bach");
	int err = 0;
	const char *argv[] = { "artist", "=", "Bach" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_multi_value_neq_fails_on_any_match
	struct song_tags t = {0};
	tags_set(&t, TAG_ARTIST, "Mozart");
	tags_add(&t, TAG_ARTIST, "Bach");
	int err = 0;
	const char *argv[] = { "artist", "!=", "Bach" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(!filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_regex_match
	struct song_tags t = {0};
	tags_set(&t, TAG_TITLE, "Symphony No. 5");
	int err = 0;
	const char *argv[] = { "title", "~", "symphony" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_and_both_match
	struct song_tags t = {0};
	tags_set(&t, TAG_ARTIST, "Bach");
	tags_set(&t, TAG_GENRE,  "Baroque");
	int err = 0;
	const char *argv[] = { "artist", "=", "Bach", "and", "genre", "=", "Baroque" };
	struct filter_node *n = filter_parse(7, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_and_one_fail
	struct song_tags t = {0};
	tags_set(&t, TAG_ARTIST, "Bach");
	tags_set(&t, TAG_GENRE,  "Jazz");
	int err = 0;
	const char *argv[] = { "artist", "=", "Bach", "and", "genre", "=", "Baroque" };
	struct filter_node *n = filter_parse(7, argv, &err);
	ck_assert(!filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_or_one_match
	struct song_tags t = {0};
	tags_set(&t, TAG_GENRE, "Jazz");
	int err = 0;
	const char *argv[] = { "genre", "=", "Rock", "or", "genre", "=", "Jazz" };
	struct filter_node *n = filter_parse(7, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_not_inverts
	struct song_tags t = {0};
	tags_set(&t, TAG_GENRE, "Jazz");
	int err = 0;
	const char *argv[] = { "not", "genre", "=", "Rock" };
	struct filter_node *n = filter_parse(4, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_track_numeric_gt
	struct song_tags t = {0};
	tags_set(&t, TAG_TRACK, "5");
	int err = 0;
	const char *argv[] = { "track", ">", "3" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_track_numeric_lt_fails
	struct song_tags t = {0};
	tags_set(&t, TAG_TRACK, "2");
	int err = 0;
	const char *argv[] = { "track", ">", "3" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(!filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_date_exact
	struct song_tags t = {0};
	tags_set(&t, TAG_DATE, "2012");
	int err = 0;
	const char *argv[] = { "date", "=", "2012" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_date_prefix_match
	/* Filter "2012" is a prefix of song date "2012-05-03". */
	struct song_tags t = {0};
	tags_set(&t, TAG_DATE, "2012-05-03");
	int err = 0;
	const char *argv[] = { "date", "=", "2012" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_date_more_specific_filter_no_match
	/* Filter "2012-05" is more specific than song date "2012". */
	struct song_tags t = {0};
	tags_set(&t, TAG_DATE, "2012");
	int err = 0;
	const char *argv[] = { "date", "=", "2012-05" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(!filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_date_le_ordering
	struct song_tags t = {0};
	tags_set(&t, TAG_DATE, "1993-05-12");
	int err = 0;
	const char *argv[] = { "date", "<=", "1993-09" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_date_le_excludes_later
	struct song_tags t = {0};
	tags_set(&t, TAG_DATE, "1993-10-01");
	int err = 0;
	const char *argv[] = { "date", "<=", "1993-09" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(!filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);

#test eval_date_ge_year
	struct song_tags t = {0};
	tags_set(&t, TAG_DATE, "1990-06-01");
	int err = 0;
	const char *argv[] = { "date", ">=", "1990" };
	struct filter_node *n = filter_parse(3, argv, &err);
	ck_assert(filter_eval(n, &t));
	filter_free(n); song_tags_free(&t);
