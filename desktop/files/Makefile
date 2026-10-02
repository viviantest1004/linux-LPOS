# desktop/files/Makefile - 파일 관리자.
#
# userland/Makefile 과 달리 여기서는 clang 도, 자체 libc 도 쓰지 않는다.
# 이 프로그램은 데비안 베이스 위에서 glibc 와 GTK 4 에 링크되고,
# 그것들을 가져오는 방법은 pkg-config 하나뿐이다. 컴파일러도 베이스가
# 가진 cc 를 그대로 쓴다 - 크로스 컴파일할 대상이 없기 때문이다.
#
# 낱말 하나: 다운로드 폴더는 '다운로드'다. '내려받기'가 아니다.
# 명세서 §1-6 의 예문에는 내려받기가 있지만 저장소 주인이 뒤집었고,
# 뒤집힌 쪽이 사람들이 실제로 부르는 이름이다. 소스와 .desktop 양쪽에
# 같은 낱말이 들어 있으므로 고칠 때 한쪽만 고치지 않도록 한다.
#
# -Werror 를 켜지 않는다. 이 앱이 맞추는 것은 베이스에 들어 있는
# GTK 4.8 이고, 거기서 현역인 gtk_show_uri_full() 과 GtkMessageDialog 이
# 4.10 부터 deprecated 다. 더 새로운 GTK 가 깔린 기계에서 빌드하면
# 경고가 나오는데, 그 경고를 없애자고 4.8 에 없는 API 로 바꾸면 정작
# 대상 기계에서 컴파일되지 않는다.

CC      ?= cc
PKG     ?= pkg-config
PKGS    := gtk4

BIN     := lp-files
SRC     := files.c
DESKTOP := lp-files.desktop

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -I../common
CFLAGS  += $(shell $(PKG) --cflags $(PKGS))
LDLIBS  += $(shell $(PKG) --libs $(PKGS))

PREFIX  ?= /usr
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

uninstall:
	rm -f $(BINDIR)/$(BIN) $(APPDIR)/$(DESKTOP)

clean:
	rm -f $(BIN)

.PHONY: all install uninstall clean
