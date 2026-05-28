#include <stdlib.h>
#include "queue.h"

#suite struct queue

#tcase pop

const struct queue pop_works_cases[] = {
	{ .head = 0, .tail = 2, .size = 3,
	  .data = (struct song [])
	          { { .path = "/a/b.mka", .uid = 123 },
	            { .path = "/c/d.mka", .uid = 456 }, } },
	{ .head = 1, .tail = 3, .size = 8,
	  .data = (struct song [])
	          { { .path = "/a/b.mka", .uid = 123 },
	            { .path = "/c/d.mka", .uid = 456 },
	            { .path = "/e/f.mka", .uid = 789 }, } },
	{ .head = 4, .tail = 5, .size = 3,
	  .data = (struct song [])
	          { { .path = "/a/b.mka", .uid = 123 },
	            { .path = "/c/d.mka", .uid = 456 },
	            { .path = "/e/f.mka", .uid = 789 }, } },
};

const struct queue pop_doesnt_change_if_empty_cases[] = {
	{ .head = 0, .tail = 0, .size = 2,
	  .data = (struct song [])
	          { { .path = "/a/b.mka", .uid = 123 },
	            { .path = "/c/d.mka", .uid = 456 }, } },
	{ .head = 8, .tail = 8, .size = 6,
	  .data = (struct song [])
	          { { .path = "/a/b.mka", .uid = 123 },
	            { .path = "/c/d.mka", .uid = 456 },
	            { .path = "/e/f.mka", .uid = 789 }, } },
};

#test-loop(0, 3) pop_works
	struct queue q = pop_works_cases[_i];
	while (q.head < q.tail) {
		const struct song s1 = q.data[q.head % q.size];
		struct song s2;
		q = pop(q, &s2);
		ck_assert_str_eq(s2.path, s1.path);
		ck_assert_int_eq(s2.uid, s1.uid);
	}

#test-loop(0, 2) pop_doesnt_change_if_empty
	struct queue q1 = pop_doesnt_change_if_empty_cases[_i];
	struct queue q2 = pop(q1, nullptr);
	ck_assert_int_eq(q2.head, q1.head);
	ck_assert_int_eq(q2.tail, q1.tail);
	ck_assert_int_eq(q2.size, q1.size);

#test pop_handles_nullptr
	struct queue q1 = { .head = 0, .tail = 1, .size = 2,
	                    .data = (struct song [])
	                            { { .path = "/a/b.mka", .uid = 123 }, } };
	struct queue q2 = pop(q1, nullptr);
	ck_assert_int_eq(q2.head, 1);
	ck_assert_int_eq(q2.tail, 1);
	ck_assert_int_eq(q2.size, 2);

#tcase push

#test push_appends
	struct queue q;
	ck_assert(mkqueue(&q, 4));
	/* Paths must be heap-allocated because cleanup_queue calls cleanup_song. */
	struct song songs[] = {
		{ .path = strdup("/a.mka"), .uid = 1 },
		{ .path = strdup("/b.mka"), .uid = 2 },
	};
	q = push(q, songs[0]);
	ck_assert_uint_eq(qsize(q), 1);
	q = push(q, songs[1]);
	ck_assert_uint_eq(qsize(q), 2);
	struct song got;
	q = pop(q, &got);
	ck_assert_str_eq(got.path, songs[0].path);
	ck_assert_uint_eq(got.uid, songs[0].uid);
	free(got.path);
	q = pop(q, &got);
	ck_assert_str_eq(got.path, songs[1].path);
	ck_assert_uint_eq(got.uid, songs[1].uid);
	free(got.path);
	cleanup_queue(&q);

#test push_reallocates_when_full
	struct queue q;
	ck_assert(mkqueue(&q, 2));
	unsigned int old_size = q.size;
	/* Paths must be heap-allocated because cleanup_queue calls cleanup_song. */
	struct song songs[] = {
		{ .path = strdup("/a.mka"), .uid = 1 },
		{ .path = strdup("/b.mka"), .uid = 2 },
		{ .path = strdup("/c.mka"), .uid = 3 },
	};
	q = push(q, songs[0]);
	q = push(q, songs[1]);
	q = push(q, songs[2]); /* Queue was full; must reallocate. */
	ck_assert_uint_eq(qsize(q), 3);
	ck_assert(q.size > old_size);
	struct song got;
	for (int i = 0; i < 3; i++) {
		q = pop(q, &got);
		ck_assert_str_eq(got.path, songs[i].path);
		ck_assert_uint_eq(got.uid, songs[i].uid);
		free(got.path);
	}
	cleanup_queue(&q);

#tcase insert_at

const struct {
	struct song song;
	int         idx;
	unsigned int expected_pos;
} insert_at_cases[] = {
	/* Index 0: song appears at the head. */
	{ { .path = "/c.mka", .uid = 3 }, 0, 0 },
	/* Index -1: song appears at the tail (equivalent to push). */
	{ { .path = "/c.mka", .uid = 3 }, -1, 2 },
	/* Index 1: song appears at position 1. */
	{ { .path = "/c.mka", .uid = 3 }, 1, 1 },
	/* Index -3 with 2 existing songs: out of range, clamps to position 1
	 * (just after head) rather than displacing the active song. */
	{ { .path = "/c.mka", .uid = 3 }, -3, 1 },
	/* Large negative: also clamps to position 1. */
	{ { .path = "/c.mka", .uid = 3 }, -100, 1 },
};

#test-loop(0, 5) insert_at_works
	struct queue q;
	ck_assert(mkqueue(&q, 4));
	/* Paths must be heap-allocated because cleanup_queue calls cleanup_song. */
	struct song existing[] = {
		{ .path = strdup("/a.mka"), .uid = 1 },
		{ .path = strdup("/b.mka"), .uid = 2 },
	};
	q = push(q, existing[0]);
	q = push(q, existing[1]);

	struct song s = { .path = strdup(insert_at_cases[_i].song.path),
	                  .uid  = insert_at_cases[_i].song.uid };
	int idx           = insert_at_cases[_i].idx;
	unsigned int pos  = insert_at_cases[_i].expected_pos;
	unsigned int old_n = qsize(q);

	q = insert_at(q, s, idx);
	ck_assert_uint_eq(qsize(q), old_n + 1);

	struct song at_pos = q.data[(q.head + pos) % q.size];
	ck_assert_str_eq(at_pos.path, s.path);
	ck_assert_uint_eq(at_pos.uid, s.uid);
	cleanup_queue(&q);

#test insert_at_on_empty_queue
	struct queue q;
	ck_assert(mkqueue(&q, 4));
	struct song s = { .path = strdup("/a.mka"), .uid = 1 };
	q = insert_at(q, s, 0);
	ck_assert_uint_eq(qsize(q), 1);
	struct song got;
	q = pop(q, &got);
	ck_assert_str_eq(got.path, s.path);
	ck_assert_uint_eq(got.uid, s.uid);
	free(got.path);
	cleanup_queue(&q);

/* Out-of-range negative on a 1-element queue: must clamp to position 1,
 * not 0, to avoid displacing the existing element to the back. */
#test insert_at_negative_oob_preserves_order
	struct queue q;
	ck_assert(mkqueue(&q, 4));
	struct song songs[] = {
		{ .path = strdup("/a.mka"), .uid = 1 },
		{ .path = strdup("/b.mka"), .uid = 2 },
	};
	q = push(q, songs[0]);
	q = insert_at(q, songs[1], -2);
	ck_assert_uint_eq(qsize(q), 2);
	struct song got;
	q = pop(q, &got);
	ck_assert_str_eq(got.path, songs[0].path);
	ck_assert_uint_eq(got.uid, songs[0].uid);
	free(got.path);
	q = pop(q, &got);
	ck_assert_str_eq(got.path, songs[1].path);
	ck_assert_uint_eq(got.uid, songs[1].uid);
	free(got.path);
	cleanup_queue(&q);

#tcase qsize

const struct {
	struct queue q;
	unsigned int size;
} qsize_works_cases[] = {
	{ { .head = 2, .tail = 7, .size = 8 }, 5 },
	{ { .head = 0, .tail = 0, .size = 3 }, 0 },
	{ { .head = 9, .tail = 9, .size = 12 }, 0 },
	{ { .head = 8, .tail = 9, .size = 5 }, 1 },
	{ { .head = 2, .tail = 9, .size = 7 }, 7 },
};

#test-loop(0, 4) qsize_works
	const struct queue q = qsize_works_cases[_i].q;
	const unsigned int size = qsize_works_cases[_i].size;
	ck_assert_int_eq(qsize(q), size);
