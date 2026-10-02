# desktop/lockscreen/Makefile - lp-lockscreen, the lock and sign-in screen.
# See lockscreen.c.
#
# Built inside the Debian base chroot like the other shell components
# (../common/shell.mk: GTK 3, gtk-layer-shell, liblpshell.a, which also
# carries the wlr-input-inhibitor protocol). libpam is linked by its
# soname: the base has the library but not libpam0g-dev's symlink.
BIN := lp-lockscreen
SRC := lockscreen.c
EXTRA_LDLIBS := -l:libpam.so.0
include ../common/shell.mk

# /etc/pam.d/lp-lockscreen (tools/mkdesktop.sh installs it).
PAMDIR ?= $(DESTDIR)/etc/pam.d
install: install-pam
install-pam:
	install -D -m 644 pam.lp-lockscreen $(PAMDIR)/lp-lockscreen
.PHONY: install-pam
