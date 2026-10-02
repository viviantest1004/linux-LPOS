# desktop/settings/Makefile - the Settings app (lp-settings).
#
# Not clang and not our own libc, for the same reason as desktop/files:
# this program links against the Debian base's glibc and GTK 4, and the
# only way to find those is pkg-config inside the chroot that is the
# target. Built on a host with GTK 4.14 against an image with 4.8, the
# result does not start there.
#
# -Werror is left off for the same reason: several APIs that are current
# in 4.8 are deprecated from 4.10, so a newer GTK warns about them. Against
# the base's 4.8.3 this builds with no warnings at all, and that is checked.
#
# One compiler invocation for all the files rather than an object per
# file. The whole program compiles in a few seconds, and no .o files are
# left in the source tree - including ../common/lp-motion.c, the shared
# springs, which is compiled in here rather than linked from
# liblpshell.a because that archive is built against GTK 3 and this
# program is GTK 4 (lp-motion.c is written for both). core.c also
# includes ../theme/motion.css with .incbin, so it is a dependency.

CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk4 gio-unix-2.0

BIN     := lp-settings
CORE    := core.c ui.c sys.c driver.c panels.c ../common/lp-motion.c
PANELS  := $(wildcard panels/*.c)
SRC     := $(CORE) $(PANELS)
HDRS    := core.h ../common/lp-i18n.h ../common/lp-motion.h ../theme/motion.css
DESKTOP := lp-settings.desktop

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -I. -I../common
CFLAGS  += $(shell $(PKG) --cflags $(PKGS))
LDLIBS  += $(shell $(PKG) --libs $(PKGS)) -lm

PREFIX  ?= /usr/local
BINDIR  := $(DESTDIR)$(PREFIX)/bin
APPDIR  := $(DESTDIR)$(PREFIX)/share/applications

all: $(BIN)

$(BIN): $(SRC) $(HDRS)
	@$(PKG) --exists $(PKGS) || \
	    { echo "gtk4 development files are missing: apt install libgtk-4-dev"; exit 1; }
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

install: $(BIN)
	install -D -m 755 $(BIN) $(BINDIR)/$(BIN)
	install -D -m 644 $(DESKTOP) $(APPDIR)/$(DESKTOP)

clean:
	rm -f $(BIN)

.PHONY: all install clean
