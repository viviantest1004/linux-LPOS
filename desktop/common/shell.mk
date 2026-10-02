# desktop/common/shell.mk - included by every shell component's Makefile.
#
# A component's Makefile sets BIN, SRC and optionally EXTRA_PKGS and
# EXTRA_LDLIBS, then includes this. It gets liblpshell.a, the GTK 3 and
# gtk-layer-shell flags, -Werror, and install/clean targets that all
# look the same.

COMMON  := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk+-3.0 gtk-layer-shell-0 gio-unix-2.0 wayland-client $(EXTRA_PKGS)

CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror -I$(COMMON)
CFLAGS  += $(shell $(PKG) --cflags $(PKGS))
# -lm for lp-motion's closed-form spring (exp, sin, cos, sqrt).
LDLIBS  += $(COMMON)/liblpshell.a $(shell $(PKG) --libs $(PKGS)) -lm $(EXTRA_LDLIBS)

PREFIX  ?= /usr/local
BINDIR  ?= $(DESTDIR)$(PREFIX)/bin

all: $(BIN)

$(COMMON)/liblpshell.a: FORCE
	$(MAKE) -C $(COMMON)

$(BIN): $(SRC) $(COMMON)/liblpshell.a $(wildcard $(COMMON)/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

install: $(BIN)
	install -D -m 755 $(BIN) $(BINDIR)/$(BIN)

clean:
	rm -f $(BIN)

FORCE:

.PHONY: all install clean FORCE
