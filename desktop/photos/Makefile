# desktop/photos/Makefile - LP Photos, the picture viewer and editor.
#
# Same rules as desktop/shot/Makefile: no clang, no own libc. It links
# the Debian base's glibc and GTK 4 and finds them through pkg-config,
# so it is built inside the target chroot - GTK 4.14 on the build host
# would link things GTK 4.8 on the image does not have.
#
# No -Werror, for the same reason as there: a few 4.8 APIs used here
# (GtkColorButton, gtk_style_context_get_color) are deprecated from 4.10
# on, and a newer GTK warns about them.

CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk4

BIN     := lp-photos
SRC     := photos.c edit.c
HDR     := photos.h ../common/lp-i18n.h ../common/lp-fit.h
DESKTOP := lp-photos.desktop

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -I../common
CFLAGS  += $(shell $(PKG) --cflags $(PKGS))
LDLIBS  += $(shell $(PKG) --libs $(PKGS)) -lm

PREFIX  ?= /usr/local
BINDIR  := $(DESTDIR)$(PREFIX)/bin
APPDIR  := $(DESTDIR)$(PREFIX)/share/applications

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	@$(PKG) --exists $(PKGS) || \
	    { echo "gtk4 개발 파일이 없습니다: apt install libgtk-4-dev"; exit 1; }
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

install: $(BIN)
	install -D -m 755 $(BIN) $(BINDIR)/$(BIN)
	install -D -m 644 $(DESKTOP) $(APPDIR)/$(DESKTOP)

clean:
	rm -f $(BIN)

.PHONY: all install clean
