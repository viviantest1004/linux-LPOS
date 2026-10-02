# desktop/firstboot/Makefile - the first-boot setup window (lp-firstboot).
#
# Built inside the Debian base against its GTK 4.8, like the installer
# window it shares its look and its account and region pages with
# (../installer/setup-ui.c, setup-pages.c). Its backend is the
# installer's lp-install (`configure`), reached through the setup gate;
# there is nothing else to install from here.
#
#   make                        (inside the base chroot)
#   make install DESTDIR=/root  (what tools/mkdesktop.sh does)

CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk4

BIN     := lp-firstboot
INST    := ../installer
SRC     := lp-firstboot.c $(INST)/setup-ui.c $(INST)/setup-pages.c \
           ../common/lp-motion.c ../common/lp-json.c
HDRS    := $(INST)/setup-ui.h $(INST)/setup-pages.h ../common/lp-motion.h \
           ../common/lp-json.h ../theme/motion.css

CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror -I$(INST) -I../common
CFLAGS  += $(shell $(PKG) --cflags $(PKGS))
LDLIBS  += $(shell $(PKG) --libs $(PKGS)) -lm

PREFIX  ?= /usr/local
BINDIR  := $(DESTDIR)$(PREFIX)/bin

all: $(BIN)

$(BIN): $(SRC) $(HDRS)
	@$(PKG) --exists $(PKGS) || \
	    { echo "gtk4 development files are missing: apt install libgtk-4-dev"; exit 1; }
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

install: $(BIN)
	install -D -m 755 $(BIN) $(BINDIR)/$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all install clean
