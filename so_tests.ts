#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include "tags.h"

#define main so_main
#include "so.c"
#undef main

static void
tags_set(struct song_tags *t, enum tag_field f, const char *val)
{
	t->fields[f].vals    = malloc(sizeof(char *));
	t->fields[f].vals[0] = strdup(val);
	t->fields[f].lens    = malloc(sizeof(size_t));
	t->fields[f].lens[0] = strlen(val);
	t->fields[f].count   = 1;
}

#suite so

#tcase parse_order

#test parse_order_defaults_to_album_then_track
	char *argv[] = { "so" };
	struct order order = {0};
	ck_assert(parse_order(1, argv, &order));
	ck_assert_int_eq(order.count, 2);
	ck_assert_int_eq(order.fields[0], TAG_ALBUM);
	ck_assert_int_eq(order.fields[1], TAG_TRACK);

#test parse_order_accepts_sp_field_names
	char *argv[] = { "so", "artist", "title", "#" };
	struct order order = {0};
	ck_assert(parse_order(4, argv, &order));
	ck_assert_int_eq(order.count, 3);
	ck_assert_int_eq(order.fields[0], TAG_ARTIST);
	ck_assert_int_eq(order.fields[1], TAG_TITLE);
	ck_assert_int_eq(order.fields[2], TAG_TRACK);
	free(order.fields);

#test parse_order_rejects_track_alias
	char *argv[] = { "so", "track" };
	struct order order = {0};
	ck_assert(!parse_order(2, argv, &order));

#tcase compare

#test cmp_items_uses_default_album_then_numeric_track
	struct order order = {
		.fields = (enum tag_field[]){ TAG_ALBUM, TAG_TRACK },
		.count = 2,
	};
	struct item items[] = {
		{ .pos = 0 }, { .pos = 1 }, { .pos = 2 },
	};
	tags_set(&items[0].tags, TAG_ALBUM, "B");
	tags_set(&items[0].tags, TAG_TRACK, "1");
	tags_set(&items[1].tags, TAG_ALBUM, "A");
	tags_set(&items[1].tags, TAG_TRACK, "10");
	tags_set(&items[2].tags, TAG_ALBUM, "A");
	tags_set(&items[2].tags, TAG_TRACK, "2");
	sort_order = &order;

	qsort(items, 3, sizeof items[0], cmp_items);

	ck_assert_str_eq(items[0].tags.fields[TAG_TRACK].vals[0], "2");
	ck_assert_str_eq(items[1].tags.fields[TAG_TRACK].vals[0], "10");
	ck_assert_str_eq(items[2].tags.fields[TAG_ALBUM].vals[0], "B");
	for (int i = 0; i < 3; i++)
		song_tags_free(&items[i].tags);

#test cmp_items_uses_input_order_for_ties
	struct order order = {
		.fields = (enum tag_field[]){ TAG_ARTIST },
		.count = 1,
	};
	struct item items[] = {
		{ .pos = 1 }, { .pos = 0 },
	};
	tags_set(&items[0].tags, TAG_ARTIST, "Bach");
	tags_set(&items[1].tags, TAG_ARTIST, "Bach");
	sort_order = &order;

	qsort(items, 2, sizeof items[0], cmp_items);

	ck_assert_int_eq(items[0].pos, 0);
	ck_assert_int_eq(items[1].pos, 1);
	for (int i = 0; i < 2; i++)
		song_tags_free(&items[i].tags);
