/* Include: ebml.h, matroska.h, regex.h, stdbool.h, stdint.h, sys/types.h. */
#pragma once

#include <regex.h>
#include <stdbool.h>

enum filter_op {
	FOP_EQ,
	FOP_NEQ,
	FOP_LT,
	FOP_LE,
	FOP_GT,
	FOP_GE,
	FOP_RE,
};

enum filter_ntype { FN_AND, FN_OR, FN_NOT, FN_PRED };

struct filter_node {
	enum filter_ntype type;
	union {
		struct {
			struct filter_node *left, *right; /* FN_AND, FN_OR */
		};
		struct filter_node *operand;          /* FN_NOT */
		struct {
			enum tag_field field;
			enum filter_op op;
			char          *value;         /* heap-allocated */
			regex_t        re;
			bool           re_compiled;
		} pred;                               /* FN_PRED */
	};
};

/* Parse argv tokens [0..argc) into a filter AST.
   Returns NULL and leaves *err unchanged when argc == 0 (accept-all).
   Returns NULL and sets *err = 1 on parse error (message to stderr).
   Caller must call filter_free() on the returned pointer. */
[[gnu::nonnull(2, 3)]]
struct filter_node *filter_parse(int argc, const char *argv[], int *err);

/* Evaluate the filter against song tags.
   A NULL node is accept-all and returns true. */
bool filter_eval(const struct filter_node *, const struct song_tags *);

/* Free the AST. Safe to call with NULL. */
void filter_free(struct filter_node *);
