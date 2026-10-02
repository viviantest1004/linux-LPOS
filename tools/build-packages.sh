#!/usr/bin/env bash
#
# build-packages.sh - 저장소에 올릴 패키지들을 세 아키텍처 모두에 대해 짓는다.
#
# 왜 있는가: 패키지 관리자가 있는데 깔 게 하나도 없으면 기능이 있는지
# 없는지 알 수가 없다. 여기 있는 것들은 "이미지에 넣기는 아깝지만 있으면
# 좋은" 것들이다 - 루트 파일시스템은 램에 상주하므로 안 쓰는 프로그램을
# 넣어두면 기계의 수명 내내 그 메모리를 쓴다.
#
# 결과는 저장소 디렉터리에 아키텍처별로 들어간다:
#
#   repo/arm64/index  repo/arm64/*.tar
#   repo/armv6/index  repo/armv6/*.tar
#   repo/amd64/index  repo/amd64/*.tar
#
# 이 디렉터리를 그대로 git 에 올리면 raw.githubusercontent.com 이
# 정적 파일로 HTTPS 에 실어준다. 기기 쪽 pkg 의 기본 저장소가 바로
# 그 주소다 (userland/pkg/pkg.c 의 DEFAULT_REPO).
#
# 시험 삼아 띄워보려면:
#   ./tools/build-packages.sh
#   python3 -m http.server -d repo 8000
# 기기에서:
#   pkg repo http://<이 컴퓨터>:8000
#   pkg install hexdump
#
# 사용법:
#   ./tools/build-packages.sh [저장소디렉터리]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USERLAND="${REPO_ROOT}/userland"
OUT="${1:-${REPO_ROOT}/repo}"
STAGE_ROOT="$(mktemp -d)"
trap 'rm -rf "$STAGE_ROOT"' EXIT

CC="${CC:-clang}"
LD="${LD:-ld.lld}"
STRIP="${STRIP:-llvm-strip}"
OBJDUMP="${OBJDUMP:-llvm-objdump}"

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }

VERSION=1.0
ARCHES="arm64 armv6 amd64"

# 아키텍처 하나의 컴파일 설정. userland/Makefile 과 같은 값이어야 한다 -
# 다르면 여기서 지은 프로그램만 조용히 다르게 동작한다.
arch_setup() {
    case "$1" in
        arm64) TRIPLE=aarch64-linux-gnu
               CPUFLAGS="-mcpu=cortex-a53"
               OBJDIR="${USERLAND}/build"
               WANT="aarch64" ;;
        armv6) TRIPLE=arm-linux-gnueabihf
               CPUFLAGS="-march=armv6kz -mfpu=vfp -mfloat-abi=hard -marm"
               OBJDIR="${USERLAND}/build-armv6"
               WANT="arm" ;;
        amd64) TRIPLE=x86_64-linux-gnu
               CPUFLAGS="-march=x86-64"
               OBJDIR="${USERLAND}/build-amd64"
               WANT="x86_64" ;;
        *) die "모르는 아키텍처: $1" ;;
    esac
}

# 우리 libc 로 프로그램 하나를 짓는다. SDK 를 쓰지 않는 이유는 SDK 가
# arm64 하나만 만들기 때문이고, 여기서 필요한 것은 셋 다이기 때문이다.
lp_build() {
    local arch="$1" out="$2" src="$3"
    arch_setup "$arch"
    [[ -d "$OBJDIR/libc" ]] || die "$OBJDIR/libc 가 없습니다 (make 를 먼저)"
    local obj="${STAGE_ROOT}/.$(basename "$out").$arch.o"
    "$CC" --target="$TRIPLE" $CPUFLAGS \
        -ffreestanding -nostdlibinc -fno-builtin \
        -fno-stack-protector -fno-pic -fno-pie \
        -ffunction-sections -fdata-sections \
        -std=c11 -Os -Wall -Wextra \
        -I"${USERLAND}/libc/include" \
        -I"${REPO_ROOT}/thirdparty/bearssl/inc" \
        -c "$src" -o "$obj"
    "$LD" -static --gc-sections -e _start \
        "$obj" "$OBJDIR"/libc/*.o "$OBJDIR/libbearssl.a" -o "$out"
    "$STRIP" "$out"
    rm -f "$obj"

    # 기기에서 exec 이 실패하는 것보다 여기서 실패하는 편이 낫다.
    # 잘못된 아키텍처의 바이너리는 설치까지 멀쩡히 되고 실행에서만 죽는다.
    local got
    got=$("$OBJDUMP" -f "$out" 2>/dev/null | sed -n 's/^architecture: *\([^,]*\).*/\1/p')
    [[ "$got" == *"$WANT"* ]] || die "$out: $WANT 가 아니라 $got 입니다"
}

step "libc 오브젝트 준비"
for a in $ARCHES; do
    case "$a" in
        arm64) make -C "$USERLAND" -j"$(nproc)" bin/cat >/dev/null ;;
        armv6) make -C "$USERLAND" ARCH=armv6 -j"$(nproc)" bin-armv6/cat >/dev/null ;;
        amd64) make -C "$USERLAND" ARCH=amd64 -j"$(nproc)" bin-amd64/cat >/dev/null ;;
    esac
    log "$a"
done

mkdir -p "$OUT"

# ── C 로 된 것들 ─────────────────────────────────────────────────
for name in hexdump; do
    step "$name"
    src="${REPO_ROOT}/packages/${name}/${name}.c"
    [[ -f "$src" ]] || die "$src 가 없습니다"
    for a in $ARCHES; do
        stage="${STAGE_ROOT}/${name}-${a}"
        mkdir -p "${stage}/bin"
        lp_build "$a" "${stage}/bin/${name}" "$src"
        log "$a  $(stat -c%s "${stage}/bin/${name}") bytes"
        out=$("${REPO_ROOT}/tools/mkpkg.sh" "$name" "$VERSION" "$stage" "$a" "$OUT")
        printf '%s\n' "$out" | sed -n 1p
    done
done

# ── 스크립트 예제들 ──────────────────────────────────────────────
# 아키텍처와 무관하지만 인덱스는 아키텍처별이므로 셋 다에 넣는다.
# 같은 파일이 세 번 들어가는 대신, 어느 기계에서든 `pkg install examples`
# 가 그냥 된다.
step "examples"
stage="${STAGE_ROOT}/examples"
mkdir -p "${stage}/examples"
cp "${REPO_ROOT}/examples/"*.sh "${stage}/examples/"
cp "${REPO_ROOT}/examples/README.md" "${stage}/examples/"
log "$(ls -1 "${stage}/examples" | wc -l) 개 파일"
for a in $ARCHES; do
    out=$("${REPO_ROOT}/tools/mkpkg.sh" examples "$VERSION" "$stage" "$a" "$OUT")
    printf '%s\n' "$out" | sed -n 1p
done

step "결과"
for a in $ARCHES; do
    echo "  --- $a"
    sed 's/^/    /' "${OUT}/${a}/index"
done
