#pragma once

struct song {
	char *path;
	unsigned long uid;
};

[[gnu::nonnull(1, 2)]]
bool parse_song(const char *, struct song *);
[[gnu::nonnull(1)]]
void cleanup_song(struct song *);
