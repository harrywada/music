#include <ctype.h>
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp(3). */
#include <sys/types.h>
#include "tags.h"
#include "filter.h"

/* ── Token cursor ──────────────────────────────────────── */

struct ctx {
	int          argc;
	const char **argv;
	int          pos;
	int         *err;
};

/* ── Type validation ───────────────────────────────────── */

static bool
is_date_value(const char *s)
{
	/* Accepts YYYY, YYYY-MM, or YYYY-MM-DD. */
	size_t len = strlen(s);

	if (len == 4) {
		for (int i = 0; i < 4; i++)
			if (!isdigit((unsigned char) s[i])) return false;
		return true;
	}
	if (len == 7) {
		for (int i = 0; i < 4; i++)
			if (!isdigit((unsigned char) s[i])) return false;
		if (s[4] != '-') return false;
		for (int i = 5; i < 7; i++)
			if (!isdigit((unsigned char) s[i])) return false;
		return true;
	}
	if (len == 10) {
		for (int i = 0; i < 4; i++)
			if (!isdigit((unsigned char) s[i])) return false;
		if (s[4] != '-') return false;
		for (int i = 5; i < 7; i++)
			if (!isdigit((unsigned char) s[i])) return false;
		if (s[7] != '-') return false;
		for (int i = 8; i < 10; i++)
			if (!isdigit((unsigned char) s[i])) return false;
		return true;
	}
	return false;
}

static bool
is_int_value(const char *s)
{
	if (!*s) return false;
	if (*s == '-' || *s == '+') s++;
	if (!*s) return false;
	while (*s)
		if (!isdigit((unsigned char) *s++)) return false;
	return true;
}

/* ── AST helpers ───────────────────────────────────────── */

static struct filter_node *
make_binary(enum filter_ntype type, struct filter_node *left,
            struct filter_node *right)
{
	struct filter_node *n = malloc(sizeof *n);
	if (!n) { filter_free(left); filter_free(right); return NULL; }
	n->type  = type;
	n->left  = left;
	n->right = right;
	return n;
}

/* ── Parser ────────────────────────────────────────────── */

static struct filter_node *parse_or(struct ctx *);
static struct filter_node *parse_and(struct ctx *);
static struct filter_node *parse_not(struct ctx *);
static struct filter_node *parse_primary(struct ctx *);
static struct filter_node *parse_pred(struct ctx *);

static struct filter_node *
parse_or(struct ctx *ctx)
{
	struct filter_node *left = parse_and(ctx);
	if (!left) return NULL;

	while (ctx->pos < ctx->argc
	       && strcmp(ctx->argv[ctx->pos], "or") == 0) {
		ctx->pos++;
		struct filter_node *right = parse_and(ctx);
		if (!right) { filter_free(left); return NULL; }
		left = make_binary(FN_OR, left, right);
		if (!left) return NULL;
	}
	return left;
}

static struct filter_node *
parse_and(struct ctx *ctx)
{
	struct filter_node *left = parse_not(ctx);
	if (!left) return NULL;

	while (ctx->pos < ctx->argc
	       && strcmp(ctx->argv[ctx->pos], "and") == 0) {
		ctx->pos++;
		struct filter_node *right = parse_not(ctx);
		if (!right) { filter_free(left); return NULL; }
		left = make_binary(FN_AND, left, right);
		if (!left) return NULL;
	}
	return left;
}

static struct filter_node *
parse_not(struct ctx *ctx)
{
	if (ctx->pos < ctx->argc
	    && strcmp(ctx->argv[ctx->pos], "not") == 0) {
		ctx->pos++;
		struct filter_node *operand = parse_not(ctx);
		if (!operand) return NULL;
		struct filter_node *n = malloc(sizeof *n);
		if (!n) { filter_free(operand); *ctx->err = 1; return NULL; }
		n->type    = FN_NOT;
		n->operand = operand;
		return n;
	}
	return parse_primary(ctx);
}

static struct filter_node *
parse_primary(struct ctx *ctx)
{
	if (ctx->pos < ctx->argc
	    && strcmp(ctx->argv[ctx->pos], "(") == 0) {
		ctx->pos++;
		struct filter_node *inner = parse_or(ctx);
		if (!inner) return NULL;
		if (ctx->pos >= ctx->argc
		    || strcmp(ctx->argv[ctx->pos], ")") != 0) {
			fprintf(stderr, "sf: expected ')'\n");
			filter_free(inner);
			*ctx->err = 1;
			return NULL;
		}
		ctx->pos++;
		return inner;
	}
	return parse_pred(ctx);
}

static struct filter_node *
parse_pred(struct ctx *ctx)
{
	if (ctx->pos + 2 >= ctx->argc) {
		fprintf(stderr, "sf: incomplete predicate\n");
		*ctx->err = 1;
		return NULL;
	}

	const char *field_str = ctx->argv[ctx->pos++];
	const char *op_str    = ctx->argv[ctx->pos++];
	const char *val_str   = ctx->argv[ctx->pos++];

	/* Parse field name. */
	enum tag_field field;
	if      (strcmp(field_str, "date")      == 0) field = TAG_DATE;
	else if (strcmp(field_str, "orig-date") == 0) field = TAG_ORIG_DATE;
	else if (strcmp(field_str, "artist")    == 0) field = TAG_ARTIST;
	else if (strcmp(field_str, "title")     == 0) field = TAG_TITLE;
	else if (strcmp(field_str, "genre")     == 0) field = TAG_GENRE;
	else if (strcmp(field_str, "album")     == 0) field = TAG_ALBUM;
	else if (strcmp(field_str, "track")     == 0) field = TAG_TRACK;
	else if (strcmp(field_str, "disc")      == 0) field = TAG_DISC;
	else {
		fprintf(stderr, "sf: unknown field: %s\n", field_str);
		*ctx->err = 1;
		return NULL;
	}

	/* Parse operator. */
	enum filter_op op;
	if      (strcmp(op_str, "=")  == 0) op = FOP_EQ;
	else if (strcmp(op_str, "!=") == 0) op = FOP_NEQ;
	else if (strcmp(op_str, "<")  == 0) op = FOP_LT;
	else if (strcmp(op_str, "<=") == 0) op = FOP_LE;
	else if (strcmp(op_str, ">")  == 0) op = FOP_GT;
	else if (strcmp(op_str, ">=") == 0) op = FOP_GE;
	else if (strcmp(op_str, "~")  == 0) op = FOP_RE;
	else {
		fprintf(stderr, "sf: unknown operator: %s\n", op_str);
		*ctx->err = 1;
		return NULL;
	}

	/* Type-aware validation. */
	bool is_date  = (field == TAG_DATE || field == TAG_ORIG_DATE);
	bool is_numeric = (field == TAG_TRACK || field == TAG_DISC);

	if (is_date) {
		if (op == FOP_RE) {
			fprintf(stderr, "sf: `~` not valid for date field\n");
			*ctx->err = 1;
			return NULL;
		}
		if (!is_date_value(val_str)) {
			fprintf(stderr,
			        "sf: invalid date value: %s"
			        " (expected YYYY, YYYY-MM, or YYYY-MM-DD)\n",
			        val_str);
			*ctx->err = 1;
			return NULL;
		}
	} else if (is_numeric) {
		if (op == FOP_RE) {
			fprintf(stderr, "sf: `~` not valid for numeric field\n");
			*ctx->err = 1;
			return NULL;
		}
		if (!is_int_value(val_str)) {
			fprintf(stderr, "sf: invalid integer value for %s: %s\n",
			        field_str, val_str);
			*ctx->err = 1;
			return NULL;
		}
	}

	/* Build predicate node. */
	struct filter_node *n = malloc(sizeof *n);
	if (!n) { *ctx->err = 1; return NULL; }
	n->type            = FN_PRED;
	n->pred.field      = field;
	n->pred.op         = op;
	n->pred.re_compiled = false;
	n->pred.value      = strdup(val_str);
	if (!n->pred.value) { free(n); *ctx->err = 1; return NULL; }

	if (op == FOP_RE) {
		int r = regcomp(&n->pred.re, val_str,
		                REG_EXTENDED | REG_ICASE | REG_NOSUB);
		if (r != 0) {
			char errbuf[128];
			regerror(r, &n->pred.re, errbuf, sizeof errbuf);
			fprintf(stderr, "sf: invalid regex '%s': %s\n",
			        val_str, errbuf);
			free(n->pred.value);
			free(n);
			*ctx->err = 1;
			return NULL;
		}
		n->pred.re_compiled = true;
	}

	return n;
}

struct filter_node *
filter_parse(int argc, const char *argv[], int *err)
{
	if (argc == 0) return NULL;

	struct ctx ctx = { .argc = argc, .argv = argv, .pos = 0, .err = err };
	struct filter_node *n = parse_or(&ctx);
	if (!n) return NULL;

	if (ctx.pos != ctx.argc) {
		fprintf(stderr, "sf: unexpected token: %s\n", argv[ctx.pos]);
		filter_free(n);
		*err = 1;
		return NULL;
	}
	return n;
}

/* ── Evaluator ─────────────────────────────────────────── */

/* True if song value satisfies equality against the filter value. */
static bool
eq_match(const struct filter_node *n, const char *val)
{
	const char *fv = n->pred.value;

	switch (n->pred.field) {
	case TAG_DATE:
	case TAG_ORIG_DATE:
		/* Prefix match: filter value must be a prefix of song date. */
		return strncmp(val, fv, strlen(fv)) == 0;
	case TAG_TRACK:
	case TAG_DISC:
		return strtol(val, NULL, 10) == strtol(fv, NULL, 10);
	default:
		return strcasecmp(val, fv) == 0;
	}
}

static bool
cmp_single(const struct filter_node *n, const char *val)
{
	const char *fv = n->pred.value;
	int c;

	switch (n->pred.op) {
	case FOP_EQ:
		return eq_match(n, val);
	case FOP_NEQ:
		/* Universal negative — handled in eval_pred, not here. */
		return !eq_match(n, val);
	case FOP_RE:
		return regexec(&n->pred.re, val, 0, NULL, 0) == 0;
	default:
		break;
	}

	/* Ordering operators. */
	switch (n->pred.field) {
	case TAG_DATE:
	case TAG_ORIG_DATE:
		c = strncmp(val, fv, strlen(fv));
		break;
	case TAG_TRACK:
	case TAG_DISC: {
		long sv  = strtol(val, NULL, 10);
		long fvl = strtol(fv,  NULL, 10);
		c = (sv < fvl) ? -1 : (sv > fvl) ? 1 : 0;
		break;
	}
	default:
		c = strcasecmp(val, fv);
		break;
	}

	switch (n->pred.op) {
	case FOP_LT: return c < 0;
	case FOP_LE: return c <= 0;
	case FOP_GT: return c > 0;
	case FOP_GE: return c >= 0;
	default:     return false;
	}
}

static bool
eval_pred(const struct filter_node *n, const struct song_tags *tags)
{
	const struct tag_values *tv = &tags->fields[n->pred.field];

	if (tv->count == 0) return false;

	if (n->pred.op == FOP_NEQ) {
		/* Universal negative: true if no value equals the filter value. */
		for (size_t i = 0; i < tv->count; i++)
			if (eq_match(n, tv->vals[i])) return false;
		return true;
	}

	/* Existential: true if any value satisfies. */
	for (size_t i = 0; i < tv->count; i++)
		if (cmp_single(n, tv->vals[i])) return true;
	return false;
}

bool
filter_eval(const struct filter_node *n, const struct song_tags *tags)
{
	if (!n) return true;

	switch (n->type) {
	case FN_AND:  return filter_eval(n->left, tags) && filter_eval(n->right, tags);
	case FN_OR:   return filter_eval(n->left, tags) || filter_eval(n->right, tags);
	case FN_NOT:  return !filter_eval(n->operand, tags);
	case FN_PRED: return eval_pred(n, tags);
	}
	return false;
}

void
filter_free(struct filter_node *n)
{
	if (!n) return;

	switch (n->type) {
	case FN_AND: case FN_OR:
		filter_free(n->left);
		filter_free(n->right);
		break;
	case FN_NOT:
		filter_free(n->operand);
		break;
	case FN_PRED:
		free(n->pred.value);
		if (n->pred.re_compiled)
			regfree(&n->pred.re);
		break;
	}
	free(n);
}
