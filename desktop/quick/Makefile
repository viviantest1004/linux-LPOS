# desktop/quick/Makefile - lp-quick, quick settings. See quick.c.
#
# Built inside the Debian base chroot, against the GTK 3 and
# gtk-layer-shell the image ships; ../common/shell.mk has the rest
# (liblpshell.a, -Werror, install). It was a GTK 4 program once; quick.c
# says why it is not any more.
BIN := lp-quick
SRC := quick.c
include ../common/shell.mk

# The .desktop file keeps "lp-quick" findable by name in the app grid's
# search without putting a tile for it there (NoDisplay).
APPDIR ?= $(DESTDIR)$(PREFIX)/share/applications
install: install-desktop
install-desktop:
	install -D -m 644 lp-quick.desktop $(APPDIR)/lp-quick.desktop
.PHONY: install-desktop
