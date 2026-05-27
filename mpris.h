#pragma once
#include "state.h"

struct mpris;

/* Open a session-bus connection and register MPRIS 2.2 objects.
   D-Bus well-known name: org.mpris.MediaPlayer2.musicd
   Returns NULL (with syslog warning) on failure. */
struct mpris *mpris_open(void);

/* File descriptor to register with poll(2). */
int mpris_fd(const struct mpris *);

/* Events sd-bus currently needs (POLLIN / POLLOUT).
   Must be refreshed before each poll(2) call. */
short mpris_events(const struct mpris *);

/* Dispatch one pending D-Bus message.  Method handlers may modify *st
   via cmd_* calls.  Returns > 0 if a message was dispatched, 0 if none
   pending, < 0 on error.  Call in a loop until <= 0. */
int mpris_process(struct mpris *, struct state *st);

/* Compare old and new state; emit PropertiesChanged for what changed.
   Also refreshes cached Matroska metadata when the current song changes.
   Call once per poll cycle after each state commit. */
void mpris_notify(struct mpris *, struct state old, struct state new);

/* Flush, unregister, and free.  Safe on NULL. */
void mpris_close(struct mpris *);
