# desktop/video/Makefile - 동영상 (lp-video).
#
# desktop/shot/Makefile 과 같은 이유로 대상 chroot 안에서 빌드한다:
# 이미지의 GTK 는 4.8 이고, 빌드 호스트의 더 새 GTK 에 링크된 것은
# 이미지에서 뜨지 않는다.
#
# libmpv 는 링크하지 않는다. 헤더가 이미지에 없고(mpv-min.h 가 대신
# 한다), 없는 기계에서도 앱은 떠서 편집 도구를 써야 하므로 실행 중에
# dlopen 으로 연다. 그래서 -ldl 만 붙는다.

CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk4

BIN     := lp-video
SRC     := video.c edit.c
HDR     := edit.h mpv-min.h ../common/lp-fit.h
DESKTOP := lp-video.desktop

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -I../common
CFLAGS  += $(shell $(PKG) --cflags $(PKGS))
LDLIBS  += $(shell $(PKG) --libs $(PKGS)) -ldl -lm

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
