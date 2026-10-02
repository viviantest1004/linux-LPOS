#!/usr/bin/env bash
#
# build-sysroot.sh - CPython 이 필요로 하는 C 라이브러리를 aarch64 정적
# 라이브러리로 크로스 빌드해 sysroot 에 넣는다.
#
# 왜 필요한가:
#   CPython 의 표준 라이브러리 중 상당수는 겉은 파이썬이지만 속은 C 이고,
#   그 C 가 외부 라이브러리를 부른다. 그 라이브러리가 없으면 configure 가
#   해당 모듈을 조용히 건너뛰고, 기기에서 import 할 때야 ModuleNotFoundError
#   로 드러난다.
#
#     ssl, hashlib   OpenSSL     없으면 HTTPS 접속 자체가 안 된다
#     sqlite3        SQLite
#     readline       readline + ncurses   없으면 REPL 에서 방향키가
#                    ^[[A 로 찍힌다 (히스토리·줄편집 없음)
#     lzma           xz
#     bz2            bzip2
#     ctypes         libffi      (이미 있음)
#     zlib           zlib        (이미 있음)
#
# 전부 정적(.a)으로 짓는다. 우리 시스템에는 동적 링커도 공유 라이브러리
# 경로도 없고, CPython 자체를 정적으로 링크하기 때문이다.
#
# 사용법:
#   ./tools/build-sysroot.sh              # 없는 것만 빌드
#   ./tools/build-sysroot.sh --force      # 있어도 다시 빌드
#   ./tools/build-sysroot.sh --verify     # 받은 소스 체크섬만 검증

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"

WORK="${WORK:-${LPZERO_WORK}/thirdparty}"
SYSROOT="${SYSROOT:-${WORK}/sysroot}"
SUMS_FILE="${REPO_ROOT}/tools/sysroot.sha256"
JOBS="${JOBS:-$(nproc)}"
CROSS=aarch64-linux-gnu-
HOST=aarch64-linux-gnu
BUILD=x86_64-linux-gnu

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

command -v curl >/dev/null            || die "curl 이 필요합니다"
command -v "${CROSS}gcc" >/dev/null   || die "${CROSS}gcc 가 없습니다"

mkdir -p "$WORK" "$SYSROOT"

# 이름  버전  URL  압축을푼디렉터리
PKGS=(
    "openssl|3.5.1|https://www.openssl.org/source/openssl-3.5.1.tar.gz|openssl-3.5.1"
    "ncurses|6.5|https://ftp.gnu.org/gnu/ncurses/ncurses-6.5.tar.gz|ncurses-6.5"
    "readline|8.2|https://ftp.gnu.org/gnu/readline/readline-8.2.tar.gz|readline-8.2"
    "sqlite|3.50.1|https://www.sqlite.org/2025/sqlite-autoconf-3500100.tar.gz|sqlite-autoconf-3500100"
    "xz|5.6.3|https://github.com/tukaani-project/xz/releases/download/v5.6.3/xz-5.6.3.tar.gz|xz-5.6.3"
    "bzip2|1.0.8|https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz|bzip2-1.0.8"
)

fetch() {   # fetch <이름> <url> <디렉터리>
    local name="$1" url="$2" dir="$3"
    local tarball="${WORK}/${name}.tar.gz"

    if [[ ! -f "$tarball" ]]; then
        log "GET  ${name}"
        # --fail 이 없으면 404 HTML 을 tarball 로 저장해놓고
        # 나중에 압축 풀 때 엉뚱한 오류가 난다.
        curl --fail --location --silent --show-error \
             --retry 4 --retry-delay 2 --retry-all-errors \
             --output "$tarball" "$url" || die "${name} 다운로드 실패"
    fi

    # 체크섬이 기록되어 있으면 검증한다. 없으면 이번 것을 기록한다.
    local want have
    have="$(sha256sum "$tarball" | cut -d' ' -f1)"
    if [[ -f "$SUMS_FILE" ]] && want="$(awk -v n="${name}.tar.gz" '$2==n {print $1}' "$SUMS_FILE")" \
       && [[ -n "$want" ]]; then
        [[ "$want" == "$have" ]] || die "${name} 체크섬 불일치
       기대: ${want}
       실제: ${have}"
    else
        printf '%s  %s.tar.gz\n' "$have" "$name" >> "$SUMS_FILE"
        log "체크섬 기록: ${name}"
    fi

    [[ -d "${WORK}/${dir}" ]] || tar -xzf "$tarball" -C "$WORK"
}

# ── 소스 받기 ────────────────────────────────────────────────────
step "소스 받기"
for p in "${PKGS[@]}"; do
    IFS='|' read -r name ver url dir <<< "$p"
    fetch "$name" "$url" "$dir"
done
sort -u -o "$SUMS_FILE" "$SUMS_FILE" 2>/dev/null || true

if $VERIFY; then
    step "체크섬 검증"
    ( cd "$WORK" && sha256sum -c "$SUMS_FILE" )
    exit 0
fi

have_lib() { [[ -f "${SYSROOT}/lib/$1" ]] && ! $FORCE; }

# ── OpenSSL ──────────────────────────────────────────────────────
# no-shared  정적만. no-tests/no-docs 는 빌드 시간을 크게 줄인다.
# no-asm 은 쓰지 않는다. Cortex-A53 의 암호 가속을 쓰는 편이
# TLS 핸드셰이크에서 체감으로 빠르다.
if have_lib libssl.a; then
    log "openssl 이미 있음"
else
    step "OpenSSL 3.5.1"
    ( cd "${WORK}/openssl-3.5.1"
      ./Configure linux-aarch64 no-shared no-tests no-docs no-legacy \
          --prefix="$SYSROOT" --openssldir="/data/ssl" \
          --cross-compile-prefix="$CROSS" -Os > /tmp/ssl-conf.log 2>&1 \
          || { tail -20 /tmp/ssl-conf.log; die "OpenSSL configure 실패"; }
      make -j"$JOBS" > /tmp/ssl-make.log 2>&1 \
          || { grep -iE "error" /tmp/ssl-make.log | head -10; die "OpenSSL 빌드 실패"; }
      make install_sw > /tmp/ssl-install.log 2>&1 || die "OpenSSL 설치 실패" )
    log "libssl.a $(stat -c%s "${SYSROOT}/lib/libssl.a") bytes"
fi

# ── ncurses (readline 이 요구한다) ───────────────────────────────
# --with-termlib 를 쓰지 않는다. 라이브러리가 libncurses/libtinfo 로
# 갈라지면 정적 링크에서 순서 문제가 생긴다. 하나로 둔다.
if have_lib libncurses.a; then
    log "ncurses 이미 있음"
else
    step "ncurses 6.5"
    ( cd "${WORK}/ncurses-6.5"
      ./configure --host="$HOST" --build="$BUILD" --prefix="$SYSROOT" \
          --without-shared --without-debug --without-ada --without-cxx-binding \
          --without-manpages --without-tests --without-progs \
          --enable-widec --with-default-terminfo-dir=/data/terminfo \
          CC="${CROSS}gcc" CFLAGS="-Os" > /tmp/ncurses-conf.log 2>&1 \
          || { tail -20 /tmp/ncurses-conf.log; die "ncurses configure 실패"; }
      make -j"$JOBS" > /tmp/ncurses-make.log 2>&1 \
          || { grep -iE "error" /tmp/ncurses-make.log | head -10; die "ncurses 빌드 실패"; }
      make install > /tmp/ncurses-install.log 2>&1 || die "ncurses 설치 실패" )
    # --enable-widec 는 libncursesw.a 로 설치한다. readline 의 configure 가
    # libncurses 를 찾으므로 이름을 맞춰준다.
    [[ -f "${SYSROOT}/lib/libncursesw.a" && ! -f "${SYSROOT}/lib/libncurses.a" ]] \
        && cp "${SYSROOT}/lib/libncursesw.a" "${SYSROOT}/lib/libncurses.a"

    # ncurses 는 --enable-pc-files 없이는 .pc 를 설치하지 않는다.
    # 그런데 readline.pc 에 'Requires.private: ncurses' 가 들어 있어서
    # ncurses.pc 가 없으면 pkg-config 가 readline 조회 자체를 실패시키고,
    # CPython 의 configure 가 readline 을 못 찾은 것으로 처리한다.
    # (그러면 기기에서 REPL 방향키가 안 된다.) 직접 써준다.
    mkdir -p "${SYSROOT}/lib/pkgconfig"
    for n in ncurses ncursesw; do
        cat > "${SYSROOT}/lib/pkgconfig/${n}.pc" <<PC
prefix=${SYSROOT}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: ncurses
Description: ncurses (wide character, static)
Version: 6.5
Libs: -L\${libdir} -lncursesw
Cflags: -I\${includedir} -I\${includedir}/ncursesw
PC
    done
    log "libncurses.a $(stat -c%s "${SYSROOT}/lib/libncurses.a") bytes"
    log "ncurses.pc / ncursesw.pc 직접 작성"
fi

# ── readline ─────────────────────────────────────────────────────
# 이게 있어야 python REPL 에서 방향키·히스토리·Ctrl-R 이 된다.
# bash_cv_* 는 크로스 환경에서 configure 가 타겟을 실행해 보려는
# 시험들이다. 답을 미리 주지 않으면 configure 가 멈추거나 틀린 답을 쓴다.
if have_lib libreadline.a; then
    log "readline 이미 있음"
else
    step "readline 8.2"
    ( cd "${WORK}/readline-8.2"
      ./configure --host="$HOST" --build="$BUILD" --prefix="$SYSROOT" \
          --disable-shared --enable-static --with-curses \
          CC="${CROSS}gcc" CFLAGS="-Os -I${SYSROOT}/include" \
          LDFLAGS="-L${SYSROOT}/lib" \
          bash_cv_wcwidth_broken=no \
          bash_cv_func_sigsetjmp=present \
          > /tmp/rl-conf.log 2>&1 \
          || { tail -20 /tmp/rl-conf.log; die "readline configure 실패"; }
      make -j"$JOBS" > /tmp/rl-make.log 2>&1 \
          || { grep -iE "error" /tmp/rl-make.log | head -10; die "readline 빌드 실패"; }
      make install > /tmp/rl-install.log 2>&1 || die "readline 설치 실패" )
    log "libreadline.a $(stat -c%s "${SYSROOT}/lib/libreadline.a") bytes"
fi

# ── SQLite ───────────────────────────────────────────────────────
if have_lib libsqlite3.a; then
    log "sqlite 이미 있음"
else
    step "SQLite 3.50.1"
    ( cd "${WORK}/sqlite-autoconf-3500100"
      ./configure --host="$HOST" --build="$BUILD" --prefix="$SYSROOT" \
          --disable-shared --enable-static --disable-readline \
          CC="${CROSS}gcc" CFLAGS="-Os -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1" \
          > /tmp/sq-conf.log 2>&1 \
          || { tail -20 /tmp/sq-conf.log; die "SQLite configure 실패"; }
      make -j"$JOBS" > /tmp/sq-make.log 2>&1 \
          || { grep -iE "error" /tmp/sq-make.log | head -10; die "SQLite 빌드 실패"; }
      make install > /tmp/sq-install.log 2>&1 || die "SQLite 설치 실패" )
    log "libsqlite3.a $(stat -c%s "${SYSROOT}/lib/libsqlite3.a") bytes"
fi

# ── xz (lzma) ────────────────────────────────────────────────────
if have_lib liblzma.a; then
    log "xz 이미 있음"
else
    step "xz 5.6.3"
    ( cd "${WORK}/xz-5.6.3"
      ./configure --host="$HOST" --build="$BUILD" --prefix="$SYSROOT" \
          --disable-shared --enable-static --disable-xz --disable-xzdec \
          --disable-lzmadec --disable-lzmainfo --disable-scripts --disable-doc \
          --disable-nls \
          CC="${CROSS}gcc" CFLAGS="-Os" > /tmp/xz-conf.log 2>&1 \
          || { tail -20 /tmp/xz-conf.log; die "xz configure 실패"; }
      make -j"$JOBS" > /tmp/xz-make.log 2>&1 \
          || { grep -iE "error" /tmp/xz-make.log | head -10; die "xz 빌드 실패"; }
      make install > /tmp/xz-install.log 2>&1 || die "xz 설치 실패" )
    log "liblzma.a $(stat -c%s "${SYSROOT}/lib/liblzma.a") bytes"
fi

# ── bzip2 ────────────────────────────────────────────────────────
# autotools 를 안 쓴다. Makefile 에 CC/AR/RANLIB 을 직접 넘긴다.
if have_lib libbz2.a; then
    log "bzip2 이미 있음"
else
    step "bzip2 1.0.8"
    ( cd "${WORK}/bzip2-1.0.8"
      make clean > /dev/null 2>&1 || true
      make -j"$JOBS" libbz2.a \
          CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
          CFLAGS="-Os -D_FILE_OFFSET_BITS=64" > /tmp/bz-make.log 2>&1 \
          || { grep -iE "error" /tmp/bz-make.log | head -10; die "bzip2 빌드 실패"; }
      install -m 644 libbz2.a "${SYSROOT}/lib/"
      install -m 644 bzlib.h  "${SYSROOT}/include/" )
    log "libbz2.a $(stat -c%s "${SYSROOT}/lib/libbz2.a") bytes"
fi

# ── zlib 공유 라이브러리 ─────────────────────────────────────────
#
# 나머지는 전부 정적이면 되지만 libz 만은 공유본이 필요하다.
# PyPI 의 manylinux 휠은 자기가 링크한 외부 라이브러리를 함께 넣어
# 배포하는데, libz 는 예외적으로 시스템에 있다고 가정하는 경우가 있다.
# numpy 가 그렇다: 휠 안의 libscipy_openblas 가 libz.so.1 을 NEEDED 로
# 걸고 있어서, 이것이 없으면 설치는 성공하고 import 에서 죽는다.
#
# 이 파일은 /data/glibc 에 함께 놓인다(tools/build-python.sh). 시스템
# 이미지에는 여전히 공유 라이브러리가 하나도 없다.
if [[ -f "${SYSROOT}/lib/libz.so.1" ]]; then
    log "libz.so.1 이미 있음"
else
    step "zlib 공유본"
    ( cd "${WORK}/zlib"
      make distclean > /dev/null 2>&1 || true
      CHOST=aarch64-linux-gnu CC="${CROSS}gcc" AR="${CROSS}ar" \
          RANLIB="${CROSS}ranlib" ./configure --prefix="${SYSROOT}" \
          > /tmp/zlib-conf.log 2>&1 \
          || { tail -10 /tmp/zlib-conf.log; die "zlib configure 실패"; }
      make -j"$JOBS" > /tmp/zlib-make.log 2>&1 \
          || { grep -iE "error" /tmp/zlib-make.log | head -10; die "zlib 빌드 실패"; }
      make install > /tmp/zlib-inst.log 2>&1 \
          || { tail -10 /tmp/zlib-inst.log; die "zlib 설치 실패"; } )
    [[ -f "${SYSROOT}/lib/libz.so.1" ]] || die "libz.so.1 이 만들어지지 않았습니다"
    log "libz.so.1 $(stat -c%s "$(readlink -f "${SYSROOT}/lib/libz.so.1")") bytes"
fi

# ── 루트 인증서 ──────────────────────────────────────────────────
# HTTPS 를 쓰려면 서버 인증서를 검증할 루트 CA 목록이 있어야 한다.
# 없으면 파이썬이 CERTIFICATE_VERIFY_FAILED 로 죽는다.
#
# 주의: 호스트의 /etc/ssl/certs/ca-certificates.crt 를 그대로 쓰면 안 된다.
# 사내 프록시나 CI 컨테이너는 자기 TLS 가로채기 CA 를 거기에 끼워넣는다.
# 그 CA 가 기기에 들어가면 그 프록시가 기기의 모든 HTTPS 를 볼 수 있게
# 된다. 배포판이 관리하는 Mozilla 원본 디렉터리만 쓴다.
step "루트 인증서 (Mozilla)"
CA_DIR=/usr/share/ca-certificates/mozilla
CA_OUT="${WORK}/ca/cert.pem"
mkdir -p "$(dirname "$CA_OUT")"
if [[ -d "$CA_DIR" ]]; then
    cat "${CA_DIR}"/*.crt > "$CA_OUT"

    # 호스트 신뢰 저장소에는 있지만 Mozilla 원본에는 없는 인증서가
    # 결과물에 섞이지 않았는지 확인한다. 섞였다면 위 경로가 오염된 것이다.
    HOST_BUNDLE=/etc/ssl/certs/ca-certificates.crt
    if [[ -f "$HOST_BUNDLE" ]] && command -v openssl >/dev/null 2>&1; then
        fps() {   # 파일 안 모든 인증서의 SHA256 지문
            awk -v d="$2" 'BEGIN{n=0} /BEGIN CERT/{n++} n>0{print > (d "/c-" n ".pem")}' "$1"
            for f in "$2"/c-*.pem; do
                openssl x509 -in "$f" -noout -fingerprint -sha256 2>/dev/null
            done | sed 's/.*=//' | sort -u
        }
        T1=$(mktemp -d); T2=$(mktemp -d)
        EXTRA=$(comm -13 <(fps "$CA_OUT" "$T1") <(fps "$HOST_BUNDLE" "$T2") | wc -l)
        rm -rf "$T1" "$T2"
        [[ "$EXTRA" -gt 0 ]] && log "호스트에만 있는 CA ${EXTRA}개는 제외했습니다 (프록시 CA 로 보입니다)"
    fi
    log "cert.pem  $(grep -c 'BEGIN CERTIFICATE' "$CA_OUT")개 인증서"
else
    log "경고: ${CA_DIR} 가 없습니다 - HTTPS 검증이 안 됩니다"
    log "  (apt install ca-certificates)"
fi

# ── 확인 ─────────────────────────────────────────────────────────
step "결과"
MISSING=0
for l in libssl.a libcrypto.a libncurses.a libreadline.a libsqlite3.a \
         liblzma.a libbz2.a libffi.a libz.a; do
    if [[ -f "${SYSROOT}/lib/$l" ]]; then
        # 크로스 빌드가 실수로 호스트 컴파일러를 쓰면 x86 오브젝트가
        # 들어간다. 링크할 때가 아니라 여기서 잡는다.
        #
        # grep -m1 을 쓰면 안 된다. 아카이브는 멤버마다 한 줄씩 나오는데
        # grep 이 첫 줄에서 파이프를 닫아 objdump 가 SIGPIPE 로 죽고,
        # pipefail 이 그걸 실패로 보아 스크립트가 조용히 끝나버린다.
        # awk 로 끝까지 읽고 마지막 값을 쓴다.
        arch=$( { "${CROSS}objdump" -f "${SYSROOT}/lib/$l" 2>/dev/null || true; } \
                | awk '/file format/ { a = $NF } END { print a }' )
        printf '  %-16s %10s bytes  %s\n' "$l" "$(stat -c%s "${SYSROOT}/lib/$l")" "$arch"
        [[ "$arch" == *aarch64* ]] || { echo "     !! aarch64 가 아닙니다"; MISSING=1; }
    else
        printf '  %-16s %s\n' "$l" "없음"
        MISSING=1
    fi
done
echo ""
if [[ "$MISSING" == "0" ]]; then
    echo "  sysroot 준비 완료: ${SYSROOT}"
    echo "  이제 RECONFIGURE=1 ./tools/build-python.sh 로 CPython 을 다시 지으세요."
else
    die "빠진 라이브러리가 있습니다"
fi
