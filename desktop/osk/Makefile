# desktop/osk/Makefile - lp-osk, the on-screen keyboard. See lp-osk.c.
#
# Built inside the Debian base chroot, against the GTK 3, gtk-layer-shell
# and libxkbcommon the image ships; pkg-config finds everything.
#
# Not ../common/shell.mk: lp-osk uses none of liblpshell.a but the
# springs, and its single-instance socket has to answer (status, watch,
# the tests), which lp_single_instance does not. lp-motion.c is compiled
# straight in, as the GTK 4 applications do - the same file the rest of
# the shell links through liblpshell.a, so the keyboard moves on exactly
# the same springs.
#
# The two protocols are compiled from the XML in protocol/: wlroots
# implements input-method-unstable-v2 and virtual-keyboard-unstable-v1,
# and neither is in Debian's wayland-protocols package (which has only
# input-method v1, a different protocol). Generating from a copy held
# here means the build cannot drift with a newer package.
#
#   make            lp-osk
#   make check      the Hangul automaton against Unicode (no display needed)
#   make install    into $(DESTDIR)$(PREFIX)/bin

CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk+-3.0 gtk-layer-shell-0 gio-unix-2.0 wayland-client xkbcommon
SCANNER := $(shell $(PKG) --variable=wayland_scanner wayland-scanner)
COMMON  := ../common

CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror -I. -I$(COMMON) -DG_LOG_DOMAIN=\"lp-osk\"
CFLAGS  += $(shell $(PKG) --cflags $(PKGS))
LDLIBS  += $(shell $(PKG) --libs $(PKGS)) -lm

PREFIX  ?= /usr/local
BINDIR  ?= $(DESTDIR)$(PREFIX)/bin

PROTOS  := input-method-unstable-v2 virtual-keyboard-unstable-v1
GEN_H   := $(PROTOS:%=%-client-protocol.h)
GEN_O   := $(PROTOS:%=%-protocol.o)
SRC     := lp-osk.c ui.c type.c evdev.c hangul.c $(COMMON)/lp-motion.c

all: lp-osk

%-client-protocol.h: protocol/%.xml
	$(SCANNER) client-header $< $@

%-protocol.c: protocol/%.xml
	$(SCANNER) private-code $< $@

# Generated code is not ours to warn about.
%-protocol.o: %-protocol.c
	$(CC) $(filter-out -Werror -Wextra,$(CFLAGS)) -c -o $@ $<

lp-osk: $(SRC) $(GEN_H) $(GEN_O) osk.h hangul.h $(COMMON)/lp-motion.h $(COMMON)/lp-i18n.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(GEN_O) $(LDLIBS)

hangul-test: hangul-test.c hangul.c hangul.h
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror -o $@ hangul-test.c hangul.c

check: hangul-test
	./hangul-test

install: lp-osk
	install -D -m 755 lp-osk $(BINDIR)/lp-osk

clean:
	rm -f lp-osk hangul-test $(GEN_H) $(PROTOS:%=%-protocol.c) $(GEN_O)

.PHONY: all check install clean
.SECONDARY: $(PROTOS:%=%-protocol.c)
