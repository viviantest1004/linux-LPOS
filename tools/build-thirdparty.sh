#!/usr/bin/env bash
#
# build-thirdparty.sh - 이미지에 들어가는 남의 코드 두 개를 짓는다.
#
#   dropbear         SSH 서버
#   wpa_supplicant   WPA2 인증 (libnl 을 먼저 짓는다)
#
# 이 두 개만 남의 것인 이유는 하나다: 암호 구현을 직접 쓰는 것은
# 이 프로젝트에서 유일하게 "틀려도 조용한" 종류의 코드이기 때문이다.
# 나머지 - 커널 설정, libc, 셸, 모든 명령어 - 는 전부 우리 것이다.
#
# 설정 파일은 저장소 안에 있다:
#   thirdparty/dropbear-localoptions.h   비밀번호 인증을 뺀 빌드 옵션
#   thirdparty/wpa_supplicant.config     WPA2 에 필요한 것만
#
# 여기 있는 이유: 이 두 파일이 보안 결정을 담고 있는데, 빌드 트리에만
# 두면 저장소를 새로 받은 사람에게는 전해지지 않는다. 비밀번호 인증을
# 껐다는 사실이 아무 데도 적혀 있지 않은 채로 dropbear 를 다시 지으면,
# 다음 이미지에는 비밀번호 인증이 켜져 있게 된다.
#
# 사용법:
#   ./tools/build-thirdparty.sh            없는 것만
#   ./tools/build-thirdparty.sh --force    다시 짓는다
#   ./tools/build-thirdparty.sh --verify   받은 소스 체크섬만 검증

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"

WORK="${WORK:-${LPZERO_WORK}/thirdparty}"
SYSROOT="${SYSROOT:-${WORK}/sysroot}"
SUMS_FILE="${REPO_ROOT}/tools/thirdparty.sha256"
CONF_DIR="${REPO_ROOT}/thirdparty"

JOBS="${JOBS:-$(nproc)}"

# LP_ARCH picks the machine these two binaries are for. They are the only
# compiled code in the image that is not ours, and a binary for the wrong
# machine does not fail to copy - it fails at exec, once every five
# seconds for ever, because init never gives up on dropbear.
#
#   arm64  Pi Zero 2 W, arm64 VMs
#   armv6  Pi Zero W - ARM1176, 32-bit. A different instruction set
#          entirely, not a subset: an aarch64 binary will not run one
#          instruction on it.
#   amd64  a PC or EC2
LP_ARCH="${LP_ARCH:-arm64}"
case "$LP_ARCH" in
    arm64)
        CROSS=aarch64-linux-gnu-
        HOST=aarch64-linux-gnu
        WANT_FORMAT=aarch64
        # arm64 builds where the sources are unpacked, as it always has.
        # Moving it would strand every tree already on a disk somewhere.
        TREE="${WORK}"
        ;;
    armv6)
        # armel, not armhf, and the reason took a while to find.
        #
        # Debian's armhf port has an ARMv7 baseline. Setting
        # -march=armv6 fixes the code the compiler generates, but not
        # the code that comes prebuilt: libgcc.a - the division and
        # 64-bit-shift helpers every ARM program links - is Thumb-2
        # there. dropbear built that way passes every check that looks
        # at file formats and dies with SIGILL on the first divide.
        #
        # armel's baseline is ARMv5TE, which an ARM1176 runs. Its libgcc
        # is ARM mode plus Thumb-1 interworking stubs, both fine here.
        # The cost is the soft-float calling convention; dropbear and
        # wpa_supplicant do no floating point worth the name.
        CROSS=arm-linux-gnueabi-
        HOST=arm-linux-gnueabi
        WANT_FORMAT=elf32-littlearm
        TREE="${WORK}/armv6"
        ARCH_CFLAGS="-marm -march=armv6"
        ;;
    amd64)
        CROSS=
        HOST=x86_64-linux-gnu
        WANT_FORMAT=x86-64
        TREE="${WORK}/amd64"
        ;;
    *)  printf 'error: LP_ARCH 는 arm64 / armv6 / amd64 중 하나여야 합니다\n' >&2
        exit 2 ;;
esac
ARCH_CFLAGS="${ARCH_CFLAGS:-}"
BUILD=x86_64-linux-gnu

# One sysroot per machine. libnl-3.a is a static library, so an arm64 one
# in an armv6 link is not a warning, it is 200 lines of "incompatible
# architecture" at the very end of a build that otherwise looked fine.
SYSROOT="${LP_SYSROOT:-${TREE}/sysroot}"

DROPBEAR_VER=2024.86
MUSL_VER=1.2.5
WPA_VER=2.11
LIBNL_VER=3.11.0

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }

FORCE=false
VERIFY=false
case "${1:-}" in
    --force)  FORCE=true ;;
    --verify) VERIFY=true ;;
    "")       ;;
    *)        die "알 수 없는 인자: $1" ;;
esac

command -v curl >/dev/null          || die "curl 이 필요합니다"
command -v "${CROSS:-}gcc" >/dev/null || die "${CROSS:-}gcc 가 없습니다"
[[ -f "${CONF_DIR}/dropbear-localoptions.h" ]] \
    || die "${CONF_DIR}/dropbear-localoptions.h 가 없습니다"
[[ -f "${CONF_DIR}/wpa_supplicant.config" ]] \
    || die "${CONF_DIR}/wpa_supplicant.config 가 없습니다"

mkdir -p "$WORK" "$TREE" "$SYSROOT/lib" "$SYSROOT/include"

# 이름|URL|압축을푼디렉터리|확장자
PKGS=(
  "dropbear|https://matt.ucc.asn.au/dropbear/releases/dropbear-${DROPBEAR_VER}.tar.bz2|dropbear-${DROPBEAR_VER}|tar.bz2"
  "wpa|https://w1.fi/releases/wpa_supplicant-${WPA_VER}.tar.gz|wpa_supplicant-${WPA_VER}|tar.gz"
  "libnl|https://github.com/thom311/libnl/releases/download/libnl3_11_0/libnl-${LIBNL_VER}.tar.gz|libnl-${LIBNL_VER}|tar.gz"
)
# musl is only fetched for the ARMv6 build, and the reason is worth
# writing down. Debian's armhf port has an ARMv7 baseline: its glibc is
# compiled with movw/movt and Thumb-2, neither of which exists on the
# ARM1176 in a Pi Zero W. Linking against it produces a binary that
# builds cleanly, passes every check that looks at file formats, and
# dies with SIGILL on the board. musl compiled here with -march=armv6
# is the whole libc these two programs need, built for the machine they
# will actually run on.
if [[ "$LP_ARCH" == "armv6" ]]; then
    PKGS+=("musl|https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz|musl-${MUSL_VER}|tar.gz")
fi

fetch() {   # fetch <이름> <url> <디렉터리> <확장자>
    local name="$1" url="$2" dir="$3" ext="$4"
    local tarball="${WORK}/${name}.${ext}"

    if [[ ! -f "$tarball" ]]; then
        log "GET  ${name}"
        # --fail 이 없으면 404 HTML 을 tarball 로 저장해놓고
        # 나중에 압축 풀 때 엉뚱한 오류가 난다.
        curl --fail --location --silent --show-error \
             --retry 4 --retry-delay 2 --retry-all-errors \
             --output "$tarball" "$url" || die "${name} 다운로드 실패"
    fi

    local want have
    have="$(sha256sum "$tarball" | cut -d' ' -f1)"
    if [[ -f "$SUMS_FILE" ]] && want="$(awk -v n="${name}.${ext}" '$2==n {print $1}' "$SUMS_FILE")" \
       && [[ -n "$want" ]]; then
        [[ "$want" == "$have" ]] || die "${name} 체크섬 불일치
       기대: ${want}
       실제: ${have}"
    else
        printf '%s  %s.%s\n' "$have" "$name" "$ext" >> "$SUMS_FILE"
        log "체크섬 기록: ${name}"
    fi

    # The tarball is shared between machines; the unpacked tree is not.
    # configure writes its answers into the source directory, so one tree
    # built for two machines is a tree configured for whichever ran last.
    [[ -d "${TREE}/${dir}" ]] || tar -xf "$tarball" -C "$TREE"
}

step "소스 받기 (${LP_ARCH})"
for p in "${PKGS[@]}"; do
    IFS='|' read -r name url dir ext <<< "$p"
    fetch "$name" "$url" "$dir" "$ext"
done
sort -u -o "$SUMS_FILE" "$SUMS_FILE" 2>/dev/null || true

if $VERIFY; then
    step "체크섬 검증"
    ( cd "$WORK" && sha256sum -c "$SUMS_FILE" )
    exit 0
fi

# ── musl (ARMv6 만) ──────────────────────────────────────────────
if [[ "$LP_ARCH" == "armv6" ]]; then
    MUSL_PREFIX="${TREE}/musl"
    MUSL_CC="${TREE}/musl-gcc"
    if [[ -f "${MUSL_PREFIX}/lib/libc.a" ]] && ! $FORCE; then
        log "musl 이미 있음"
    else
        step "musl ${MUSL_VER} (armv6)"
        ( cd "${TREE}/musl-${MUSL_VER}"
          CC="${CROSS}gcc" \
          CFLAGS="-Os ${ARCH_CFLAGS}" \
          ./configure --target=arm-linux-gnueabihf --prefix="$MUSL_PREFIX" \
              --disable-shared > /tmp/musl-conf.log 2>&1 \
              || { tail -15 /tmp/musl-conf.log; die "musl configure 실패"; }
          make -j"$JOBS" > /tmp/musl-make.log 2>&1 \
              || { grep -iE "error" /tmp/musl-make.log | head -10; die "musl 빌드 실패"; }
          make install > /dev/null 2>&1 || die "musl 설치 실패" )
        log "libc.a $(stat -c%s "${MUSL_PREFIX}/lib/libc.a") bytes"
    fi

    # Kernel headers, from the kernel this image ships.
    #
    # musl is a libc, not a kernel interface: it has no asm/types.h, and
    # libnl includes it on the first line. Debian would supply one
    # through linux-libc-dev, but that package is for the distribution's
    # kernel, not ours. Taking them from our own tree means the headers
    # and the running kernel are the same thing by construction.
    if [[ ! -f "${MUSL_PREFIX}/include/asm/types.h" ]]; then
        LINUX_SRC="${LINUX_SRC:-${LPZERO_WORK}/linux}"
        [[ -d "$LINUX_SRC" ]] || \
            die "커널 소스가 없습니다: ${LINUX_SRC} (tools/fetch-kernel.sh)"
        # headers_install copies with rsync and says only "Error 127"
        # when it is missing, from inside a kernel Makefile, which is
        # not a message anybody can act on.
        command -v rsync >/dev/null || die "rsync 가 필요합니다 (apt install rsync)"
        step "커널 UAPI 헤더 (arm)"
        # O=: headers_install without it writes usr/include and
        # include/generated into the kernel source tree, and every
        # later build there stops with "the source tree is not clean".
        # The arm64 and amd64 kernels share that tree.
        mkdir -p "${TREE}/hdr-build"
        make -C "$LINUX_SRC" O="${TREE}/hdr-build" ARCH=arm \
             INSTALL_HDR_PATH="$MUSL_PREFIX" headers_install \
             > /tmp/hdr-make.log 2>&1 \
            || { tail -10 /tmp/hdr-make.log; die "headers_install 실패"; }
        log "asm/, linux/ 설치됨"
    fi

    # The compiler driver everything below uses.
    #
    # -nostartfiles is not an optimisation. gcc's own crtbegin.o and
    # crtend.o come prebuilt from Debian, which means Thumb-2 - the one
    # part of the link we cannot recompile. Dropping them costs nothing
    # for C (they register exception frames, and there are none), and
    # musl runs .init_array itself, so constructors and atexit still
    # work. Everything else - crt1, crti, crtn - comes from the musl we
    # just built, for the right machine.
    GCC_INC="$("${CROSS}gcc" -print-file-name=include)"
    cat > "$MUSL_CC" <<WRAPEOF
#!/bin/sh
# Generated by tools/build-thirdparty.sh. gcc, pointed at the ARMv6 musl.
COMMON="${ARCH_CFLAGS} -nostdinc -isystem ${MUSL_PREFIX}/include -isystem ${GCC_INC} -B${MUSL_PREFIX}/lib -L${MUSL_PREFIX}/lib"
for a in "\$@"; do
    case "\$a" in
        -c|-E|-S|-M|-MM) exec ${CROSS}gcc \$COMMON "\$@" ;;
    esac
done
exec ${CROSS}gcc \$COMMON -static -nostartfiles \
     "${MUSL_PREFIX}/lib/crt1.o" "${MUSL_PREFIX}/lib/crti.o" \
     "\$@" "${MUSL_PREFIX}/lib/crtn.o"
WRAPEOF
    chmod +x "$MUSL_CC"
    CC_FOR_TARGET="$MUSL_CC"
else
    CC_FOR_TARGET="${CROSS:-}gcc"
fi

# ── libnl ────────────────────────────────────────────────────────
# wpa_supplicant 의 nl80211 드라이버가 요구한다. 정적으로만 쓴다.
if [[ -f "${SYSROOT}/lib/libnl-3.a" ]] && ! $FORCE; then
    log "libnl 이미 있음"
else
    step "libnl ${LIBNL_VER}"
    ( cd "${TREE}/libnl-${LIBNL_VER}"
      ./configure --host="$HOST" --build="$BUILD" --prefix="$SYSROOT" \
          --disable-shared --enable-static --disable-cli \
          CC="$CC_FOR_TARGET" CFLAGS="-Os" > /tmp/libnl-conf.log 2>&1 \
          || { tail -15 /tmp/libnl-conf.log; die "libnl configure 실패"; }
      make -j"$JOBS" > /tmp/libnl-make.log 2>&1 \
          || { grep -iE "error" /tmp/libnl-make.log | head -10; die "libnl 빌드 실패"; }
      make install > /dev/null 2>&1 || die "libnl 설치 실패" )
    log "libnl-3.a $(stat -c%s "${SYSROOT}/lib/libnl-3.a") bytes"
fi

# ── dropbear ─────────────────────────────────────────────────────
DB_DIR="${TREE}/dropbear-${DROPBEAR_VER}"
if [[ -f "${DB_DIR}/dropbear" ]] && ! $FORCE; then
    log "dropbear 이미 있음"
else
    step "dropbear ${DROPBEAR_VER}"
    # 설정은 저장소에서 복사한다. 빌드 트리 쪽을 고쳐봐야 다음에 지울 때
    # 사라지고, 그 사실을 알아차릴 방법이 없다.
    cp "${CONF_DIR}/dropbear-localoptions.h" "${DB_DIR}/localoptions.h"
    ( cd "$DB_DIR"
      ./configure --host="$HOST" --build="$BUILD" \
          --disable-zlib --disable-pam --disable-utmp --disable-wtmp \
          --disable-lastlog --disable-loginfunc --disable-harden \
          CC="$CC_FOR_TARGET" CFLAGS="-Os" > /tmp/db-conf.log 2>&1 \
          || { tail -15 /tmp/db-conf.log; die "dropbear configure 실패"; }
      make -j"$JOBS" PROGRAMS="dropbear dropbearkey" \
          STATIC=1 SCPPROGRESS=0 > /tmp/db-make.log 2>&1 \
          || { grep -iE "error" /tmp/db-make.log | head -10; die "dropbear 빌드 실패"; }
      "${CROSS:-}strip" dropbear dropbearkey )
    log "dropbear $(stat -c%s "${DB_DIR}/dropbear") bytes"
fi

# 비밀번호 인증이 정말 빠졌는지 확인한다. 이것이 이 스크립트가 있는
# 이유의 절반이다: 설정을 놓친 빌드는 아무 오류도 내지 않고, 그냥
# 비밀번호로 들어올 수 있는 SSH 서버가 된다.
#
# 무엇을 찾는가가 중요하다. 예전에는 "^password$" 를 찾았는데, 그
# 문자열은 auth.h 의 AUTH_METHOD_PASSWORD 정의라서 비밀번호 인증을
# 꺼서 빌드해도 바이너리에 남는다. 즉 이 검사는 항상 참이었고, 검사가
# 실제로 돌기 시작한 순간 정상 빌드를 전부 막았다 - 보안 검사가
# 거짓 양성이면 사람들은 검사를 끄지, 원인을 찾지 않는다.
#
# svr-authpasswd.c 의 로그 문구를 찾는다. 이 파일의 내용은 통째로
# DROPBEAR_SVR_PASSWORD_AUTH 안에 들어 있으므로, 이 문구가 있다는 것은
# 비밀번호 인증 코드가 실제로 링크됐다는 뜻이다.
if "${CROSS:-}strings" "${DB_DIR}/dropbear" 2>/dev/null \
   | grep -qiE "Password auth succeeded|Bad password attempt"; then
    die "dropbear 에 비밀번호 인증이 들어 있습니다. localoptions.h 가 적용되지 않았습니다."
fi
log "비밀번호 인증 없음 (공개키만)"

# ── wpa_supplicant ───────────────────────────────────────────────
WPA_DIR="${TREE}/wpa_supplicant-${WPA_VER}/wpa_supplicant"
if [[ -f "${WPA_DIR}/wpa_supplicant" ]] && ! $FORCE; then
    log "wpa_supplicant 이미 있음"
else
    step "wpa_supplicant ${WPA_VER}"
    sed "s|@SYSROOT@|${SYSROOT}|g" "${CONF_DIR}/wpa_supplicant.config" \
        > "${WPA_DIR}/.config"
    ( cd "$WPA_DIR"
      make clean > /dev/null 2>&1 || true
      make -j"$JOBS" CC="$CC_FOR_TARGET" wpa_supplicant wpa_cli \
          > /tmp/wpa-make.log 2>&1 \
          || { grep -iE "error" /tmp/wpa-make.log | head -15; die "wpa_supplicant 빌드 실패"; }
      "${CROSS:-}strip" wpa_supplicant wpa_cli )
    log "wpa_supplicant $(stat -c%s "${WPA_DIR}/wpa_supplicant") bytes"
fi

step "결과"
MISSING=0
for f in "${DB_DIR}/dropbear" "${DB_DIR}/dropbearkey" \
         "${WPA_DIR}/wpa_supplicant" "${WPA_DIR}/wpa_cli"; do
    if [[ -f "$f" ]]; then
        # 이 기계용으로 정적 빌드됐는지 확인한다. 크로스 빌드가 조용히 호스트
        # 컴파일러로 넘어가면 여기서 잡힌다.
        arch=$("${CROSS:-}objdump" -f "$f" 2>/dev/null \
               | awk '/file format/ { print $NF }')
        printf '  %-32s %9s bytes  %s\n' \
            "$(basename "$f")" "$(stat -c%s "$f")" "$arch"
        [[ "$arch" == *"$WANT_FORMAT"* ]] || \
            { echo "     !! ${WANT_FORMAT} 가 아닙니다"; MISSING=1; }
        # "32-bit ARM" is not one machine, and every check above -
        # file format, ABI, size - passes for a binary that cannot
        # execute one instruction on an ARM1176. So run it on one.
        #
        # qemu's arm1176 model raises SIGILL for anything the real chip
        # does not have, which is exactly the failure we are trying not
        # to ship. A static scan cannot replace this: musl's atomics
        # carry an ARMv7 barrier that is selected from AT_HWCAP at
        # startup and never runs here, so the instruction is in the
        # file and is not a problem.
        if [[ "$LP_ARCH" == "armv6" ]]; then
            if command -v qemu-arm-static >/dev/null 2>&1; then
                # || true: these print a usage message and exit
                # non-zero for an option they do not know, and under
                # set -e that ends the script instead of the check.
                out="$(qemu-arm-static -cpu arm1176 "$f" -V 2>&1 || true)"
                if [[ "$out" == *"Illegal instruction"* ]]; then
                    echo "     !! ARM1176 에서 SIGILL - ARMv6 에 없는 명령이 있습니다"
                    MISSING=1
                else
                    printf '     ARM1176 에서 실행됨\n'
                fi
            else
                echo "     경고: qemu-arm-static 이 없어 ARM1176 실행 확인을 건너뜁니다"
            fi
        fi
    else
        printf '  %-32s %s\n' "$(basename "$f")" "없음"
        MISSING=1
    fi
done
echo ""
[[ "$MISSING" == "0" ]] || die "빠진 것이 있습니다"
echo "  준비 완료. userland/mkrootfs.sh 가 여기서 가져갑니다."
