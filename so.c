#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#include "ebml.h"
#include "matroska.h"
#include "matroska_utils.h"
#include "tags.h"
#include "fields.h"
#include "song.h"
#include "utils.h"

struct order {
	enum tag_field *fields;
	size_t          count;
};

struct item {
	char            *path;
	unsigned long    uid;
	struct song_tags tags;
	size_t           pos;
};

struct list {
	struct item *items;
	size_t       count;
	size_t       cap;
};

static const struct order *sort_order;

static char *
xstrdup(const char *s)
{
	char *dup = strdup(s);
	if (!dup)
		die(errno, "strdup");
	return dup;
}

static void *
xreallocarray(void *ptr, size_t n, size_t size)
{
	if (size && n > (size_t)-1 / size)
		die(0, "reallocarray overflow");
	void *p = realloc(ptr, n * size);
	if (!p)
		die(errno, "realloc");
	return p;
}

/* Open a Matroska file, skip EBML header, descend into Segment,
   read seekinfo.  Returns fd on success, -1 on failure (already warned). */
static int
open_mkv(const char *path, struct mkv_seekinfo *si)
{
	int fd = open(path, O_RDONLY);
	if (fd == -1) { warn(errno, "%s", path); return -1; }

	*si = (struct mkv_seekinfo){0};
	if (ebml_skip(fd, EBML_HEADER) == -1
	||  ebml_descend(fd, MKV_SEGMENT) == -1
	||  !mkv_readseekinfo(fd, si)) {
		warn(0, "so: can't read Matroska headers in %s", path);
		close(fd);
		return -1;
	}
	return fd;
}

static void
push_item(struct list *list, const char *path, unsigned long uid,
          struct song_tags *tags)
{
	if (list->count == list->cap) {
		list->cap = list->cap ? list->cap * 2 : 64;
		list->items = xreallocarray(list->items, list->cap,
		                            sizeof *list->items);
	}
	list->items[list->count++] = (struct item){
		.path = xstrdup(path),
		.uid  = uid,
		.tags = *tags,
		.pos  = list->count,
	};
	*tags = (struct song_tags){0};
}

static int
read_song(const char *path, unsigned long uid, void *ud)
{
	struct list *list = ud;
	int fd;
	struct mkv_seekinfo si;
	struct mkv_chapter  chapter;
	struct mkv_track    track;
	struct song_tags    tags = {0};
	bool keep = false;

	if ((fd = open_mkv(path, &si)) == -1) return 1;

	if (!si.chapters) {
		warn(0, "so: no chapters in %s", path);
		goto done;
	}

	seek(fd, si.segment + si.chapters);
	if (!mkv_findchapter(fd, uid, &chapter)) {
		warn(0, "so: chapter %lu not found in %s", uid, path);
		goto done;
	}

	if (!si.tracks) {
		warn(0, "so: no tracks in %s", path);
		goto done;
	}

	seek(fd, si.segment + si.tracks);
	if (!mkv_findtrack(fd, chapter.track_uids, &track)) {
		warn(0, "so: no audio track in %s", path);
		goto done;
	}

	if (si.tags) {
		seek(fd, si.segment + si.tags);
		mkv_readsongtags(fd, uid, track.uid, chapter.edition_uid,
		                 &tags);
	}

	keep = true;

done:
	close(fd);
	if (keep)
		push_item(list, path, uid, &tags);
	song_tags_free(&tags);
	return 1;
}

static long
field_number(const struct song_tags *tags, enum tag_field field)
{
	const struct tag_values *tv = &tags->fields[field];
	if (tv->count == 0 && field == TAG_DISC)
		return 1;
	if (tv->count == 0)
		return 0;
	return strtol(tv->vals[0], NULL, 10);
}

static char *
join_values(const struct tag_values *tv)
{
	size_t len = 0;
	for (size_t i = 0; i < tv->count; i++)
		len += tv->lens[i] + (i ? 2 : 0);

	char *s = malloc(len + 1);
	if (!s)
		die(errno, "malloc");

	size_t off = 0;
	for (size_t i = 0; i < tv->count; i++) {
		if (i) {
			memcpy(s + off, ", ", 2);
			off += 2;
		}
		memcpy(s + off, tv->vals[i], tv->lens[i]);
		off += tv->lens[i];
	}
	s[off] = '\0';
	return s;
}

static int
cmp_joined(const struct tag_values *a, const struct tag_values *b)
{
	char *as = join_values(a);
	char *bs = join_values(b);
	int c = strcasecmp(as, bs);
	free(as);
	free(bs);
	return c;
}

static int
cmp_field(const struct item *a, const struct item *b, enum tag_field field)
{
	const struct tag_values *av = &a->tags.fields[field];
	const struct tag_values *bv = &b->tags.fields[field];

	if (field == TAG_TRACK || field == TAG_DISC) {
		long an = field_number(&a->tags, field);
		long bn = field_number(&b->tags, field);
		return an < bn ? -1 : an > bn ? 1 : 0;
	}

	if (av->count == 0 || bv->count == 0)
		return av->count ? 1 : bv->count ? -1 : 0;

	return cmp_joined(av, bv);
}

static int
cmp_items(const void *ap, const void *bp)
{
	const struct item *a = ap;
	const struct item *b = bp;

	for (size_t i = 0; i < sort_order->count; i++) {
		int c = cmp_field(a, b, sort_order->fields[i]);
		if (c)
			return c;
	}
	return a->pos < b->pos ? -1 : a->pos > b->pos ? 1 : 0;
}

static int
parse_order(int argc, char *argv[], struct order *order)
{
	static enum tag_field default_fields[] = { TAG_ALBUM, TAG_TRACK };

	if (argc == 1) {
		order->fields = default_fields;
		order->count = sizeof default_fields / sizeof default_fields[0];
		return 1;
	}

	order->fields = xreallocarray(NULL, (size_t)(argc - 1),
	                             sizeof *order->fields);
	order->count = (size_t)(argc - 1);

	for (int i = 1; i < argc; i++) {
		if (!sp_field_name(argv[i], &order->fields[i - 1])) {
			fprintf(stderr, "so: unknown field: %s\n", argv[i]);
			free(order->fields);
			return 0;
		}
	}
	return 1;
}

int
main(int argc, char *argv[])
{
	struct order order = {0};
	if (!parse_order(argc, argv, &order))
		return 1;

	struct list list = {0};
	char   *line    = NULL;
	size_t  linecap = 0;
	ssize_t len;

	while ((len = getline(&line, &linecap, stdin)) > 0) {
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (!len) continue;
		expand_song(line, read_song, &list);
	}

	sort_order = &order;
	qsort(list.items, list.count, sizeof *list.items, cmp_items);

	for (size_t i = 0; i < list.count; i++) {
		printf("%s#%lu\n", list.items[i].path, list.items[i].uid);
		free(list.items[i].path);
		song_tags_free(&list.items[i].tags);
	}

	free(list.items);
	free(line);
	if (argc != 1)
		free(order.fields);
	return 0;
}
