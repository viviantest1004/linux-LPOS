# desktop/software/Makefile - lp-software, the software centre.
#
# Built inside the Debian base chroot like every desktop program here:
# the image's GTK 4.8 and GLib 2.74 are what it links against, and
# pkg-config is the only way they are found. lp-kit.c (the pieces this
# program shares with lp-files and lp-tasks) and desktop/common's
# lp-motion.c are compiled straight in rather than made into a library:
# three programs, two small files, one link line each.
#
# lp-kit.c includes desktop/theme/motion.css with .incbin, by a path
# relative to the directory the compiler runs in - this one - so build
# with `make -C desktop/software`, not from elsewhere with a -f.
#
# -Werror is on: GTK 4.8 is both the build and the target, so a warning
# here is a warning on the machine.

CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk4 gio-unix-2.0

BIN     ?= lp-software
SRC     := software.c lp-kit.c ../common/lp-motion.c
HDRS    := lp-kit.h ../common/lp-motion.h ../common/lp-i18n.h ../common/lp-fit.h ../theme/motion.css
DESKTOP := lp-software.desktop

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Werror -I. -I../common
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
	install -D -m 755 $(BIN) $(BINDIR)/lp-software
	install -D -m 644 $(DESKTOP) $(APPDIR)/$(DESKTOP)

clean:
	rm -f lp-software

.PHONY: all install clean
