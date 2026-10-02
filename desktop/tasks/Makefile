# desktop/tasks/Makefile - lp-tasks, the system monitor.
#
# Built inside the Debian base chroot like every desktop program here:
# the image's GTK 4.8 and GLib 2.74 are what it links against, and
# pkg-config is the only way they are found. lp-kit.c (the pieces this
# program shares with lp-files and lp-software) and desktop/common's
# lp-motion.c are compiled straight in, as desktop/software does.
#
# So are lp-toplevel.c and lp-apps.c, the shell's window list: the Apps
# page lists what has a window, and only the compositor knows that
# (wlr-foreign-toplevel-management - wayfire 0.7 has it in its core, sway
# too). The shell's liblpshell.a cannot be linked instead, because it is
# built against GTK 3; the two files are plain GDK and GIO and build
# against either. The protocol's code is generated here from the XML
# desktop/common keeps, with the scanner the base ships.
#
# This Makefile once named tasks.c alone and could not build it, so the
# lp-tasks the image carried stayed the first version, which asked
# swaymsg for the windows and under wayfire found none. Everything the
# program is made of is listed below; a missing piece fails the build.
#
# lp-kit.c includes desktop/theme/motion.css with .incbin, by a path
# relative to the directory the compiler runs in - this one - so build
# with `make -C desktop/tasks`, not from elsewhere with a -f.
#
# -Werror is on: GTK 4.8 is both the build and the target, so a warning
# here is a warning on the machine.

CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk4 gio-unix-2.0 wayland-client
SCANNER := $(shell $(PKG) --variable=wayland_scanner wayland-scanner)

BIN     ?= lp-tasks
PROTO   := wlr-foreign-toplevel-management-unstable-v1
SRC     := tasks.c ../software/lp-kit.c ../common/lp-motion.c \
           ../common/lp-toplevel.c ../common/lp-apps.c
HDRS    := ../software/lp-kit.h ../common/lp-motion.h ../common/lp-i18n.h ../common/lp-fit.h \
           ../common/lp-toplevel.h ../common/lp-apps.h ../theme/motion.css \
           $(PROTO)-client-protocol.h
DESKTOP := lp-tasks.desktop

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Werror -I. -I../software -I../common
CFLAGS  += $(shell $(PKG) --cflags $(PKGS))
LDLIBS  += $(shell $(PKG) --libs $(PKGS)) -lm

PREFIX  ?= /usr/local
BINDIR  := $(DESTDIR)$(PREFIX)/bin
APPDIR  := $(DESTDIR)$(PREFIX)/share/applications

all: $(BIN)

$(PROTO)-client-protocol.h: ../common/protocols/$(PROTO).xml
	$(SCANNER) client-header $< $@

$(PROTO)-protocol.c: ../common/protocols/$(PROTO).xml
	$(SCANNER) private-code $< $@

# Generated code is not ours to warn about.
$(PROTO)-protocol.o: $(PROTO)-protocol.c
	$(CC) $(filter-out -Werror -Wextra,$(CFLAGS)) -c -o $@ $<

$(BIN): $(SRC) $(HDRS) $(PROTO)-protocol.o
	@$(PKG) --exists $(PKGS) || \
	    { echo "gtk4 development files are missing: apt install libgtk-4-dev"; exit 1; }
	$(CC) $(CFLAGS) -o $@ $(SRC) $(PROTO)-protocol.o $(LDLIBS)

install: $(BIN)
	install -D -m 755 $(BIN) $(BINDIR)/lp-tasks
	install -D -m 644 $(DESKTOP) $(APPDIR)/$(DESKTOP)

clean:
	rm -f lp-tasks $(PROTO)-client-protocol.h $(PROTO)-protocol.c $(PROTO)-protocol.o

.PHONY: all install clean
