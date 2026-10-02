# desktop/common/Makefile - liblpshell.a, the code every shell component
# shares (lp-shell.h says what and why).
#
# Built inside the Debian base chroot, like every desktop program here:
# the image's GTK 3 and gtk-layer-shell are what it has to link against,
# and the build host's are newer. pkg-config is the only way anything is
# found; there are no hard-coded paths.
#
# The wlroots protocols and wayfire's own shell protocol (from wayfire
# v0.7.4's proto/, the version the base carries) are compiled from the XML in protocols/ with
# wayland-scanner at build time. The XML is kept in the tree because the
# base has no wlr-protocols package, and generating from a copy we hold
# means the build cannot drift with whatever a newer package ships.

CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk+-3.0 gtk-layer-shell-0 gio-unix-2.0 wayland-client
SCANNER := $(shell $(PKG) --variable=wayland_scanner wayland-scanner)

CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror -I.
CFLAGS  += $(shell $(PKG) --cflags $(PKGS))

PROTOS  := wlr-foreign-toplevel-management-unstable-v1 \
           wlr-input-inhibitor-unstable-v1 \
           wayfire-shell-unstable-v2
GEN_H   := $(PROTOS:%=%-client-protocol.h)
GEN_C   := $(PROTOS:%=%-protocol.c)

# lp-motion.c is main's (the springs every desktop program shares, GTK 3
# and GTK 4 alike); it is compiled into this archive so that the shell
# links the same file the applications do.
OBJS    := lp-shell.o lp-json.o lp-toplevel.o lp-apps.o lp-motion.o \
           lp-sheet.o lp-wfshell.o $(PROTOS:%=%-protocol.o)

all: liblpshell.a

%-client-protocol.h: protocols/%.xml
	$(SCANNER) client-header $< $@

%-protocol.c: protocols/%.xml
	$(SCANNER) private-code $< $@

# Generated protocol code is not ours to warn about.
%-protocol.o: %-protocol.c
	$(CC) $(filter-out -Werror -Wextra,$(CFLAGS)) -c -o $@ $<

%.o: %.c $(GEN_H) $(wildcard *.h)
	$(CC) $(CFLAGS) -c -o $@ $<

liblpshell.a: $(OBJS)
	rm -f $@
	ar rcs $@ $^

clean:
	rm -f $(OBJS) $(GEN_H) $(GEN_C) liblpshell.a

.PHONY: all clean
.SECONDARY: $(GEN_C) $(GEN_H)
