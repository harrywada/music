# User-customizable values
TRACKS_MAX    = 4
VINT_OCTET_MAX = 8
MPRIS         = 1
SDBUS         = elogind
PLAY_PATH     = /usr/libexec/music/play

PKG_CONFIG   ?= pkg-config

CFLAGS += -std=c23 -D_POSIX_SOURCE -D_POSIX_C_SOURCE=200809L -DPLAY_PATH=\"$(PLAY_PATH)\" \
          -Wall -Wextra -Werror -Wno-empty-body \
          -isystem /usr/include/alsa/ \
          -DTRACKS_MAX="$(TRACKS_MAX)" \
          -DVINT_OCTET_MAX="$(VINT_OCTET_MAX)"

ifeq ($(MPRIS),1)
MPRIS_OBJ    = mpris.o tags.o
MPRIS_CFLAGS = -DMPRIS $(shell $(PKG_CONFIG) --cflags lib$(SDBUS))
MPRIS_LFLAGS = $(shell $(PKG_CONFIG) --libs lib$(SDBUS))
endif

.PHONY: all
all: play musicd sq sf sc sp so

play: LDLIBS += -lasound -lm
play: play.c ebml.o matroska.o matroska_utils.o tags.o replaygain.o utils.o log_syslog.o
musicd: musicd.c cmds.o state.o queue.o song.o utils.o log_syslog.o $(MPRIS_OBJ) matroska.o matroska_utils.o ebml.o
musicd: CFLAGS  += $(MPRIS_CFLAGS)
musicd: LDLIBS += $(MPRIS_LFLAGS)
sq: sq.c ebml.o matroska.o matroska_utils.o song.o utils.o log_stderr.o
sf: sf.c filter.o tags.o ebml.o matroska.o matroska_utils.o song.o utils.o log_stderr.o
sc: sc.c utils.o log_stderr.o
sp: sp.c format.o fields.o tags.o ebml.o matroska.o matroska_utils.o song.o utils.o log_stderr.o
so: so.c fields.o tags.o ebml.o matroska.o matroska_utils.o song.o utils.o log_stderr.o

.SUFFIXES: _tests.ts _tests.c
_tests.ts_tests.c:
	checkmk $< >$@

LDLIBS += -lcheck
cmds_tests: cmds_tests.c cmds.o state.o queue.o song.o utils.o log_stderr.o ebml.o matroska.o matroska_utils.o
filter_tests: filter_tests.c filter.o tags.o ebml.o utils.o log_stderr.o
format_tests: format_tests.c format.o fields.o tags.o ebml.o utils.o log_stderr.o
fields_tests: fields_tests.c fields.o
so_tests.c: so.c
so_tests: so_tests.c fields.o tags.o ebml.o matroska.o matroska_utils.o song.o utils.o log_stderr.o
ebml_tests: ebml_tests.c ebml.o utils.o log_stderr.o
matroska_tests: matroska_tests.c matroska.o ebml.o utils.o log_stderr.o
matroska_utils_tests: matroska_utils_tests.c matroska_utils.o matroska.o ebml.o utils.o log_stderr.o
tags_tests: tags_tests.c tags.o ebml.o matroska.o utils.o log_stderr.o
replaygain_tests: LDLIBS += -lm
replaygain_tests: replaygain_tests.c replaygain.o
queue_tests: queue_tests.c queue.o song.o utils.o log_stderr.o ebml.o matroska.o matroska_utils.o
song_tests: song_tests.c song.o utils.o log_stderr.o ebml.o matroska.o matroska_utils.o
utils_tests: utils_tests.c utils.o log_stderr.o

DESTDIR   =
BINDIR    = /usr/bin
LIBEXECDIR = /usr/libexec/music
MANDIR    = /usr/share/man

.PHONY: install
install: all
	install -d $(DESTDIR)$(LIBEXECDIR) $(DESTDIR)$(BINDIR) \
		$(DESTDIR)$(MANDIR)/man1
	install -m755 play $(DESTDIR)$(LIBEXECDIR)/play
	install -m755 musicd sq sf sc sp so $(DESTDIR)$(BINDIR)/
	install -m644 man/*.1 $(DESTDIR)$(MANDIR)/man1/

.PHONY: clean
clean:
	rm -rf musicd play sq sf sc sp so *_tests *_tests.c *.o log_syslog.o log_stderr.o
