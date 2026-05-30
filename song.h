#pragma once

struct song {
	char *path;
	unsigned long uid;
};

[[gnu::nonnull(1, 2)]]
bool parse_song(const char *, struct song *);
[[gnu::nonnull(1)]]
void cleanup_song(struct song *);

/* Expand `line` to individual (path, uid) pairs and call cb for each.
   If line contains '#', it is parsed as "path#uid" and cb is called
   once.  If line is a bare path, the Matroska file is opened, all
   chapters are enumerated, and cb is called for each.  Returns the
   number of calls made (≥1), or 0 on any error. */
[[gnu::nonnull(1, 2)]]
int expand_song(const char *line,
                int (*cb)(const char *, unsigned long, void *),
                void *ud);
