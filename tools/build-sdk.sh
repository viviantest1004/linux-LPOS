#!/usr/bin/env bash
#
# build-sdk.sh - LP-zero OS 용 C 프로그램을 만들 수 있는 최소 SDK 를 뽑는다.
#
# 이 시스템에는 동적 링커도 공유 라이브러리도 없고, libc 는 표준 것이
# 아니라 userland/libc 다. 그래서 보통의 aarch64 크로스 컴파일러로
# 그냥 짓는 프로그램은 여기서 돌지 않는다 - 헤더도 다르고, 시작 코드도
# 다르고, 링크 방식도 다르다.
#
# 여기서 만드는 것:
#   sdk/include/    libc 헤더
#   sdk/lib/crt0.o  시작 코드 (_start)
#   sdk/lib/liblp.a 나머지 libc 전부
#   sdk/bin/lp-gcc  올바른 플래그로 clang 을 부르는 래퍼
#   sdk/example/    빌드해서 넣어볼 수 있는 예제
#
# 쓰는 쪽:
#   sdk/bin/lp-gcc -o myprog myprog.c
#   tools/mkpkg.sh myprog 1.0 stage     # stage/bin/myprog 를 넣어두고
#
# 파이썬 확장이나 남이 만든 리눅스 바이너리는 이 SDK 와 상관이 없다.
# 그쪽은 /data 의 glibc 를 쓴다 (tools/build-python.sh 참고).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USERLAND="${REPO_ROOT}/userland"
SDK="${1:-${REPO_ROOT}/sdk}"

CC="${CC:-clang}"
AR="${AR:-llvm-ar}"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
log() { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }

command -v "$CC" >/dev/null || die "$CC 가 없습니다"
command -v "$AR" >/dev/null || die "$AR 가 없습니다"

step "libc 빌드"
make -C "$USERLAND" bin/cat >/dev/null   # libc 오브젝트가 생기게 한다
OBJDIR="${USERLAND}/build/libc"
[[ -d "$OBJDIR" ]] || die "libc 오브젝트가 없습니다: $OBJDIR"
[[ -f "${OBJDIR}/crt0.o" ]] || die "crt0.o 가 없습니다"

step "SDK 조립: ${SDK}"
rm -rf "$SDK"
mkdir -p "${SDK}/include" "${SDK}/lib" "${SDK}/bin" "${SDK}/example"

cp "${USERLAND}/libc/include/"*.h "${SDK}/include/"
log "헤더 $(ls -1 "${SDK}/include" | wc -l) 개"

# crt0 는 아카이브에 넣지 않고 따로 둔다. 아카이브 멤버는 참조된 심볼이
# 있을 때만 끌려 나오는데, 시작 코드는 아무도 부르지 않기 때문이다.
cp "${OBJDIR}/crt0.o" "${SDK}/lib/crt0.o"
OBJS=$(find "$OBJDIR" -name '*.o' ! -name 'crt0.o' | sort)
[[ -n "$OBJS" ]] || die "libc 오브젝트를 찾지 못했습니다"
# shellcheck disable=SC2086
"$AR" rcs "${SDK}/lib/liblp.a" $OBJS
log "liblp.a $(stat -c%s "${SDK}/lib/liblp.a") bytes, $(echo "$OBJS" | wc -l) 개 오브젝트"

cat > "${SDK}/bin/lp-gcc" <<'WRAPPER'
#!/bin/sh
#
# lp-gcc - LP-zero OS 용으로 C 를 컴파일한다.
#
#   lp-gcc -o myprog myprog.c
#
# 보통의 cc 처럼 쓰면 되지만, 붙는 것이 다르다:
#
#   --target/-mcpu   Zero 2 W 의 Cortex-A53
#   -ffreestanding   표준 라이브러리가 있다고 가정하지 않는다
#   -nostdlibinc     시스템 헤더를 배제한다. stdio.h 는 이 SDK 의 것이다
#   -fno-builtin     우리 memset 이 memset 호출로 바뀌어 무한재귀가 되는
#                    것을 막는다
#   -static          동적 링커가 없다. 이미지 어디에도 없다
#   -e _start        main 을 부르기 전에 도는 것은 crt0.o 하나뿐이다
#
# 환경변수로 바꿀 수 있는 것: CC (기본 clang)
set -e
SDK="$(cd "$(dirname "$0")/.." && pwd)"
CC="${CC:-clang}"
exec "$CC" \
    --target=aarch64-linux-gnu -mcpu=cortex-a53 \
    -ffreestanding -nostdlibinc -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie \
    -ffunction-sections -fdata-sections \
    -std=c11 -Os -Wall -Wextra \
    -I"${SDK}/include" \
    -nostdlib -static -fuse-ld=lld \
    -Wl,--gc-sections -Wl,-e,_start \
    "${SDK}/lib/crt0.o" \
    "$@" \
    "${SDK}/lib/liblp.a"
WRAPPER
chmod +x "${SDK}/bin/lp-gcc"

cat > "${SDK}/example/hello.c" <<'EXAMPLE'
/* hello.c - LP-zero OS 용 프로그램의 최소 형태.
 *
 *   ../bin/lp-gcc -o hello hello.c
 *
 * 만들어진 바이너리를 /data/bin 에 두면 바로 PATH 에 잡힌다.
 * 패키지로 만들려면:
 *
 *   mkdir -p stage/bin && cp hello stage/bin/
 *   tools/mkpkg.sh hello 1.0 stage
 */
#include "stdio.h"
#include "string.h"
#include "unistd.h"

int main(int argc, char **argv)
{
    printf("hello from LP-zero OS\n");

    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        char host[64] = "?";
        proc_read("/proc/sys/kernel/hostname", host, sizeof(host));
        char *nl = strchr(host, '\n');
        if (nl) *nl = '\0';
        printf("  pid %d on %s, uptime %ld ms\n",
               (int)lp_getpid(), host, (long)lp_monotonic_ms());
    }
    return 0;
}
EXAMPLE

cat > "${SDK}/README.md" <<'READMEEOF'
# LP-zero OS SDK

A C program for this system is not an ordinary Linux program. There is
no dynamic linker anywhere in the image, no shared libraries, and the
libc is the one in `userland/libc` rather than glibc or musl. So a
binary from a normal aarch64 cross compiler will not run here: the
kernel looks for its interpreter, finds nothing, and refuses it.

That is also the reason it is worth having an SDK at all. The same
property that keeps other people's binaries out keeps yours out.

## Building

    sdk/bin/lp-gcc -o myprog myprog.c

`lp-gcc` is a wrapper around clang. Read it - it is twenty lines, and
every flag on it is there for a reason that is written next to it.

Set `CC` to use a different compiler; it has to be one that can target
`aarch64-linux-gnu` and link with lld.

## Putting it on the machine

    scp myprog root@<address>:/data/bin/

`/data/bin` is on PATH already, and `/data` survives a reboot; the root
filesystem does not - it is unpacked into RAM from the kernel image on
every boot, so anything written to `/bin` is gone by morning.

To hand it to somebody else, make it a package:

    mkdir -p stage/bin && cp myprog stage/bin/
    tools/mkpkg.sh myprog 1.0 stage
    python3 -m http.server -d repo 8000

and on the machine:

    pkg repo http://<your address>:8000
    pkg update && pkg install myprog

## What the libc has

Look in `include/`. It is small and there is no manual, but the headers
are commented and say what each thing is for and what it does not do.

    stdio.h    printf, snprintf, readline, and the pager
    string.h   the usual, plus UTF-8 aware cursor movement
    stdlib.h   malloc, getenv, strtol
    unistd.h   files, processes, signals, the terminal, time, mounts
    net.h      sockets, interfaces, DNS, and HTTP/HTTPS in one call
    types.h    u8..u64, s8..s64, bool
    syscall.h  the raw system calls, if you need one that has no wrapper

There is no errno. A system call returns the kernel's negative errno
directly, so `if (rc < 0)` and `-rc` is the number.

## What this SDK is not for

Python extension modules and prebuilt Linux binaries have nothing to do
with it. Those run against the glibc on `/data`, which
`tools/build-python.sh` puts there. Two worlds, deliberately: the
operating system is static and self-contained, and the parts that have
to speak to the rest of the world carry their own baggage on the data
partition, where deleting it puts things back.
READMEEOF

step "예제 빌드로 검증"
( cd "${SDK}/example" && ../bin/lp-gcc -o hello hello.c )
FILE_OUT=$(file "${SDK}/example/hello" 2>/dev/null || echo "?")
case "$FILE_OUT" in
    *"ARM aarch64"*static*) log "hello: $(stat -c%s "${SDK}/example/hello") bytes, 정적 aarch64" ;;
    *) die "예제가 기대한 모양이 아닙니다: $FILE_OUT" ;;
esac

echo ""
echo "SDK: ${SDK}"
echo "  ${SDK}/bin/lp-gcc -o myprog myprog.c"
