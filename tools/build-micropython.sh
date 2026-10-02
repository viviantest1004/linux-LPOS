#!/usr/bin/env bash
#
# build-micropython.sh - MicroPython 을 정적 바이너리로 만든다 (LP_ARCH).
#
# CPython 과 나란히 /data 에 들어간다. 둘을 다 두는 이유:
#   CPython      표준 라이브러리가 전부 있다. 대신 25MB, 시작이 느리다.
#   MicroPython  1.5MB 하나로 끝. 시작이 즉시다. 표준 라이브러리는
#                축약판이고 일부 문법(예: 제너레이터 일부)이 없다.
#
# HTTPS 를 쓰려면 mbedTLS 를 함께 넣어야 한다. 넣지 않으면 socket 은
# 되는데 tls 모듈이 없어서 requests 가 ImportError 로 죽는다 - 겉보기에
# 네트워크가 되는 것 같아서 원인 찾기가 오래 걸린다.
#
# 사용법:
#   ./tools/build-micropython.sh              # 없으면 빌드
#   ./tools/build-micropython.sh --force      # 지우고 다시

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"

WORK="${WORK:-${LPZERO_WORK}/thirdparty}"
MPY_DIR="${MPY_DIR:-${WORK}/micropython}"
MPY_REF="${MPY_REF:-v1.24.1}"
JOBS="${JOBS:-$(nproc)}"
# LP_ARCH picks the machine. amd64 is a native build on this host, so
# there is no cross prefix; the build directory is per-architecture so
# the two never share objects.
LP_ARCH="${LP_ARCH:-arm64}"
if [[ "$LP_ARCH" == "amd64" ]]; then
    CROSS=
    BUILD_DIR="build-lpzero-amd64"
else
    CROSS=aarch64-linux-gnu-
    BUILD_DIR="build-lpzero"
fi

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }

FORCE=false
[[ "${1:-}" == "--force" ]] && FORCE=true

command -v "${CROSS:-}gcc" >/dev/null || die "${CROSS:-}gcc 가 없습니다"

# ── 소스 ─────────────────────────────────────────────────────────
if [[ ! -d "$MPY_DIR" ]]; then
    step "소스 받기 (${MPY_REF})"
    git clone --depth 1 --branch "$MPY_REF" \
        https://github.com/micropython/micropython.git "$MPY_DIR" \
        || die "clone 실패"
fi

cd "$MPY_DIR"

# mbedtls 는 서브모듈이다. 없으면 TLS 없이 조용히 빌드되어버린다.
if [[ ! -f lib/mbedtls/include/mbedtls/ssl.h ]]; then
    step "mbedtls 서브모듈 가져오기"
    git submodule update --init --depth 1 lib/mbedtls lib/mbedtls_errors \
        || die "mbedtls 서브모듈 실패"
fi

OUT="${MPY_DIR}/ports/unix/${BUILD_DIR}/micropython"
if [[ -f "$OUT" ]] && ! $FORCE; then
    log "이미 있습니다: $OUT"
else
    # 크로스 빌드 도구(mpy-cross)는 호스트에서 돌아야 한다.
    step "mpy-cross (호스트용)"
    make -C mpy-cross -j"$JOBS" > /tmp/mpy-cross.log 2>&1 \
        || { tail -20 /tmp/mpy-cross.log; die "mpy-cross 빌드 실패"; }

    step "MicroPython (${LP_ARCH} 정적, mbedTLS 포함)"
    $FORCE && rm -rf "ports/unix/${BUILD_DIR}"
    #  MICROPY_PY_SSL/MICROPY_SSL_MBEDTLS  tls 모듈 (HTTPS)
    #  MICROPY_PY_FFI=0                    libffi 를 요구한다. CPython 의
    #                                      ctypes 가 그 역할을 하므로 뺀다.
    #  LDFLAGS_EXTRA=-static               동적 링커가 없는 시스템이다.
    #
    #  MBEDTLS_PEM_PARSE_C / MBEDTLS_BASE64_C
    #      MicroPython 의 mbedTLS 설정에는 이 둘이 없다. 마이크로컨트롤러
    #      에서는 루트 인증서를 DER 로 미리 박아 넣기 때문이다. 그래서
    #      기본 빌드는 PEM 을 못 읽고, load_verify_locations 에 흔한
    #      cert.pem 을 주면 무조건 "invalid cert" 가 난다.
    #      pem.c 와 base64.c 는 이미 컴파일 목록에 있는데 매크로가 없어
    #      통째로 비어 있는 상태라, 정의만 켜주면 된다 (약 5KB).
    #      이걸 켜야 CPython 과 같은 /data/ssl/cert.pem 을 쓸 수 있다.
    make -C ports/unix -j"$JOBS" \
        BUILD="${BUILD_DIR}" \
        CROSS_COMPILE="$CROSS" \
        MICROPY_PY_SSL=1 \
        MICROPY_SSL_MBEDTLS=1 \
        MICROPY_PY_FFI=0 \
        CFLAGS_EXTRA="-Os -DMBEDTLS_PEM_PARSE_C -DMBEDTLS_BASE64_C" \
        LDFLAGS_EXTRA="-static" \
        > /tmp/mpy-make.log 2>&1 \
        || { grep -E "error:|undefined reference|No such file" /tmp/mpy-make.log \
             | awk 'NR <= 20'; die "빌드 실패 (전체 로그: /tmp/mpy-make.log)"; }

    "${CROSS}strip" "$OUT" 2>/dev/null || true
fi

# ── 확인 ─────────────────────────────────────────────────────────
step "결과"
ARCH=$( { "${CROSS}objdump" -f "$OUT" 2>/dev/null || true; } \
        | awk '/file format/ { a = $NF } END { print a }' )
case "$LP_ARCH" in
    amd64) [[ "$ARCH" == *x86-64* ]]  || die "x86-64 바이너리가 아닙니다: $ARCH" ;;
    *)     [[ "$ARCH" == *aarch64* ]] || die "aarch64 바이너리가 아닙니다: $ARCH" ;;
esac

# mbedTLS 가 실제로 들어갔는지 본다. MICROPY_SSL_MBEDTLS 를 빠뜨리면
# 빌드는 멀쩡히 성공하고 socket 도 되는데 tls 모듈만 없다. 그러면
# requests 가 "no module named 'tls'" 로 죽는데, 겉보기에 네트워크가
# 되는 것 같아서 원인을 찾기 어렵다.
#
# 바이너리 검사로는 확인할 수 없다. strip 하면 심볼이 없고,
# 'tls'/'ssl' 문자열은 mbedTLS 없이도 frozen 모듈 때문에 들어 있다.
# 링크에 들어간 오브젝트 파일을 직접 센다.
MBED_OBJS=$(ls "ports/unix/${BUILD_DIR}/lib/mbedtls/library/"*.o 2>/dev/null | wc -l)
[[ "$MBED_OBJS" -gt 50 ]] \
    || die "mbedTLS 가 링크되지 않았습니다 (오브젝트 ${MBED_OBJS}개).
       lib/mbedtls 서브모듈과 MICROPY_SSL_MBEDTLS=1 을 확인하세요."
log "mbedTLS 포함 (오브젝트 ${MBED_OBJS}개) - HTTPS 사용 가능"

printf '  %s\n' "$(file "$OUT" | cut -d, -f1-3)"
log "$(stat -c%s "$OUT") bytes"
log "출력: $OUT"
