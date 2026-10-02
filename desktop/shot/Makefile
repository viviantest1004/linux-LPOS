# desktop/settings/Makefile - 스크린샷 앱.
#
# desktop/files/Makefile 과 같은 이유로 clang 도 자체 libc 도 쓰지
# 않는다. 이 프로그램은 데비안 베이스의 glibc 와 GTK 4 에 링크되고,
# 그것들을 찾는 방법은 pkg-config 하나뿐이다. 그래서 반드시 대상이
# 될 chroot 안에서 빌드한다 - 빌드 호스트의 GTK 가 4.14 이고 이미지의
# GTK 가 4.8 이면, 여기서 링크된 것이 저기서 뜨지 않는다.
#
# -Werror 를 켜지 않는 이유도 같다. 4.8 에서 현역인 API 중 4.10 부터
# deprecated 인 것이 여럿이라, 더 새 GTK 에서 빌드하면 경고가 난다.

CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk4

BIN     := lp-shot
SRC     := shot.c
DESKTOP := lp-shot.desktop

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -I../common
CFLAGS  += $(shell $(PKG) --cflags $(PKGS))
LDLIBS  += $(shell $(PKG) --libs $(PKGS))

PREFIX  ?= /usr/local
BINDIR  := $(DESTDIR)$(PREFIX)/bin
APPDIR  := $(DESTDIR)$(PREFIX)/share/applications

all: $(BIN)

$(BIN): $(SRC)
	@$(PKG) --exists $(PKGS) || \
	    { echo "gtk4 개발 파일이 없습니다: apt install libgtk-4-dev"; exit 1; }
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

install: $(BIN)
	install -D -m 755 $(BIN) $(BINDIR)/$(BIN)
	install -D -m 644 $(DESKTOP) $(APPDIR)/$(DESKTOP)

clean:
	rm -f $(BIN)

.PHONY: all install clean
