#!/usr/bin/env bash
#
# build-python.sh - CPython 을 aarch64 정적 바이너리로 크로스 빌드한다.
#
# 결과는 /data 파티션에 들어간다. 시스템(initramfs)에 넣지 않는 이유:
# CPython 은 인터프리터 + 표준 라이브러리가 15~25MB 다. 커널에 내장하면
# 이미지가 23MB 에서 40MB 이상으로 커진다. 반면 /data 는 첫 부팅에
# 카드 전체로 늘어나므로 크기가 문제되지 않는다.
#
# 크로스 빌드의 핵심:
#   --with-build-python  빌드 도중 파이썬 코드를 실행해야 하는데
#                        타겟 바이너리는 여기서 돌지 않는다. 그래서
#                        호스트에 같은 **major.minor** 버전이 있어야 한다.
#   ac_cv_file__dev_*    configure 가 타겟에서 파일 존재를 시험하려 하지만
#                        크로스 환경에서는 불가능하다. 답을 미리 준다.
#
# 환경변수:
#   PY_SRC     소스 경로 (기본 .build/thirdparty/Python-3.12.3)
#   BUILD_PY   호스트 파이썬 (기본 /usr/bin/python3.12)
#   JOBS       병렬 빌드 수

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"

PY_SRC="${PY_SRC:-${WORK}/Python-3.12.3}"
BUILD_PY="${BUILD_PY:-/usr/bin/python3.12}"
STAGE="${STAGE:-${PYSTAGE_ROOT}}"
# For a cross build this is the sysroot we built the libraries into.
# For a native amd64 build there is no sysroot: the host's own /usr is
# where openssl, readline, sqlite3 and libffi already live.
SYSROOT="${SYSROOT:-${WORK}/sysroot}"
JOBS="${JOBS:-$(nproc)}"

# LP_ARCH picks the machine. arm64 is a cross build against a sysroot we
# built earlier; amd64 is native, because this build host is x86-64, so
# there is no cross compiler and no sysroot - the system's own headers
# and libraries are the right ones.
LP_ARCH="${LP_ARCH:-arm64}"
if [[ "$LP_ARCH" == "amd64" ]]; then
    CROSS=
    HOST_TRIPLE=x86_64-linux-gnu
    SYSROOT=/usr        # unconditional: the default above already fired
    # The dynamic loader has a different name on every architecture, and
    # Debian puts the shared libraries under a multiarch directory.
    LOADER=ld-linux-x86-64.so.2
    GLIBC_SRC="${GLIBC_SRC:-/lib/x86_64-linux-gnu}"
    # Deliberately a different spelling of the same machine.
    #
    # The link flags point the interpreter at /data/glibc, which does not
    # exist on this build host, so a test program configure compiles
    # cannot be run here. With --build and --host spelled identically
    # autoconf calls it a native build, tries to run one, and stops with
    # "cannot run C compiled programs". Spelled differently it treats
    # this as a cross build and answers from the ac_cv_* values below
    # instead - which is what the arm64 build has always done, and the
    # only reason it never hit this.
    BUILD_TRIPLE=x86_64-pc-linux-gnu
else
    CROSS=aarch64-linux-gnu-
    HOST_TRIPLE=aarch64-linux-gnu
    LOADER=ld-linux-aarch64.so.1
    BUILD_TRIPLE=x86_64-linux-gnu
fi

# ── Static or dynamic ────────────────────────────────────────────────
#
# It was static, and that was the right first answer: one file, no
# loader, nothing outside our own libc anywhere on the system.
#
# It also meant pip could only ever install packages written purely in
# Python. Anything with a C extension ships a .so, a static binary
# cannot load one, and every wheel on PyPI is built against glibc, which
# this system does not have. numpy, cryptography, pillow - all refused
# at the door.
#
# So the default is now dynamic, and glibc is staged alongside Python on
# /data. This does not change what the operating system is made of: the
# kernel, init, the shell and every command still run on the libc in
# userland/libc with nothing else linked in. glibc is baggage that one
# external program - CPython - carries for its own packages, on the data
# partition, where the system image never sees it.
#
# STATIC=1 builds the old way. Smaller, self-contained, and pure-Python
# packages only.
GLIBC_DIR="/data/glibc"
GLIBC_SRC="${GLIBC_SRC:-/usr/${HOST_TRIPLE}/lib}"
STATIC="${STATIC:-0}"

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

[[ -d "$PY_SRC" ]] || die "소스가 없습니다: $PY_SRC"
# _ctypes 는 libffi, zlib 모듈은 zlib 를 요구한다. 둘 다 aarch64 로
# 미리 크로스 빌드해 sysroot 에 넣어두어야 한다.
# Only for the cross build. A native amd64 build takes these from the
# host's own /usr, where the multiarch layout puts them under
# lib/x86_64-linux-gnu rather than lib, so this list would not match
# even when everything is present.
if [[ "$LP_ARCH" != "amd64" ]]; then
    for need in include/ffi.h include/zlib.h include/openssl/ssl.h \
                include/readline/readline.h include/sqlite3.h include/lzma.h \
                include/bzlib.h lib/libssl.a lib/libreadline.a lib/libncurses.a \
                lib/libsqlite3.a lib/liblzma.a lib/libbz2.a; do
        [[ -e "${SYSROOT}/${need}" ]] \
            || die "${need} 가 없습니다. './tools/build-sysroot.sh' 를 먼저 실행하세요."
    done
fi
command -v "$BUILD_PY" >/dev/null || die "$BUILD_PY 가 없습니다"
command -v "${CROSS}gcc" >/dev/null || die "${CROSS}gcc 가 없습니다"

# 호스트와 타겟의 major.minor 가 같아야 한다
SRC_VER=$(basename "$PY_SRC" | sed 's/Python-//')
HOST_VER=$("$BUILD_PY" -c 'import sys; print("%d.%d" % sys.version_info[:2])')
case "$SRC_VER" in
    "$HOST_VER".*) ;;
    *) die "버전 불일치: 소스 $SRC_VER, 호스트 $HOST_VER" ;;
esac

# The link line, and the one decision that follows from it.
#
#   --dynamic-linker  the loader's path is written into the binary. It
#                     is on /data because that is where glibc lives, and
#                     the system image has no /lib to put it in.
#   -rpath            where to look for libc.so.6 and friends.
#   --disable-new-dtags  emit DT_RPATH rather than DT_RUNPATH. RUNPATH
#                     applies only to the binary's own dependencies, and
#                     a .so that pip installs later is not one of those.
#                     RPATH is inherited through the whole search, which
#                     is exactly what an extension module needs.
if [[ "$STATIC" == "1" ]]; then
    LINK_FLAGS="-static -L${SYSROOT}/lib"
    LINK_DESC="static (pure-Python packages only)"
else
    LINK_FLAGS="-L${SYSROOT}/lib"
    LINK_FLAGS+=" -Wl,--dynamic-linker=${GLIBC_DIR}/${LOADER}"
    LINK_FLAGS+=" -Wl,-rpath,${GLIBC_DIR} -Wl,--disable-new-dtags"
    LINK_DESC="dynamic against glibc in ${GLIBC_DIR}"
    [[ -f "${GLIBC_SRC}/${LOADER}" ]] \
        || die "${LP_ARCH} glibc 가 없습니다: ${GLIBC_SRC}/${LOADER}"
fi

step "설정 (소스 ${SRC_VER}, 호스트 파이썬 ${HOST_VER}, ${LINK_DESC})"
cd "$PY_SRC"

# 설정을 바꿔 다시 빌드할 때는 이전 Makefile 을 지워야 한다
[[ -f Makefile && "${RECONFIGURE:-0}" == "1" ]] && make distclean >/dev/null 2>&1 || true

# And the same, unconditionally, when the last build was for a different
# machine. This has to sit OUTSIDE the "if there is no Makefile" block
# below - a tree configured for arm64 has a Makefile, so putting the
# check inside meant it never ran, configure never re-ran, and `make`
# happily relinked the old aarch64 objects into a binary the build then
# reported as x86-64. It was: the ELF header said x86-64 and the
# interpreter it asked for was ld-linux-aarch64.so.1.
# If this tree was last configured for a different machine, start
# over. CPython builds in its source directory, so without this the
# objects and the linked binary from the previous architecture are
# reused: the amd64 build produced an x86-64 python whose ELF
# interpreter was still /data/glibc/ld-linux-aarch64.so.1, which
# would have failed to start with no useful message at all.
ARCH_STAMP=".lp-built-for"
WAS=""
[[ -f "$ARCH_STAMP" ]] && WAS="$(cat "$ARCH_STAMP")"
# No stamp but an existing build means a tree from before this check
# existed, and there is no way to tell what it was for. Clean it:
# a wasted rebuild costs minutes, a silently mixed one costs an
# image that cannot start its interpreter.
if [[ "$WAS" != "$LP_ARCH" ]] && [[ -n "$WAS" || -f Makefile ]]; then
    step "이전 빌드는 ${WAS:-알 수 없는 아키텍처} 용 - 트리를 비웁니다"
    make distclean >/dev/null 2>&1 || true
fi
printf '%s\n' "$LP_ARCH" > "$ARCH_STAMP"

if [[ ! -f Makefile ]]; then
    # sysroot 에 라이브러리가 없는 모듈만 끈다. 켜져 있는데 라이브러리가
    # 없으면 configure 가 조용히 건너뛰고, 기기에서 import 할 때야
    # ModuleNotFoundError 로 드러난다.
    #
    #   _dbm/_gdbm  BerkeleyDB/GDBM  sqlite3 가 있으므로 필요 없다
    #   _tkinter    Tk               화면이 없다
    #   nis         NIS              쓸 일이 없다
    #   _uuid       libuuid          uuid 모듈이 os.urandom 으로 대신한다
    #   _curses     ncurses          라이브러리는 있지만 TUI 를 쓸 일이 없다
    #
    # 아래는 tools/build-sysroot.sh 로 크로스 빌드해 두었으므로 켠다:
    #   _ssl·_hashlib(OpenSSL)  _sqlite3(SQLite)  readline(readline+ncurses)
    #   _lzma(xz)  _bz2(bzip2)  _ctypes(libffi)  zlib
    DISABLED=(
        py_cv_module__curses=n/a   py_cv_module__curses_panel=n/a
        py_cv_module__dbm=n/a      py_cv_module__gdbm=n/a
        py_cv_module__tkinter=n/a  py_cv_module_nis=n/a
        py_cv_module__uuid=n/a      py_cv_module__crypt=n/a
    )

    # 호스트의 .pc 를 보면 x86 라이브러리 경로가 섞여 들어온다.
    # pkg-config 가 sysroot 만 보게 가둔다.
    if [[ "$LP_ARCH" != "amd64" ]]; then
        export PKG_CONFIG_LIBDIR="${SYSROOT}/lib/pkgconfig"
    fi

    # readline 과 sqlite3 는 링크 플래그를 직접 준다.
    #
    # configure 는 pkg-config --libs 를 쓰는데, 그건 Libs 만 주고
    # Libs.private / Requires.private 는 빼놓는다. 공유 라이브러리라면
    # 그게 맞다 - .so 안에 의존성이 기록되어 링커가 알아서 따라간다.
    # 그런데 우리는 전부 정적(.a)이라 그 정보가 없다. 그래서
    #   readline -> tputs/tgetent (ncurses)
    #   sqlite3  -> log/pow/sin   (libm)
    # 이 undefined reference 로 터지고, configure 는 "라이브러리가
    # 없다"고 판단해 모듈을 조용히 빼버린다. 기기에서 import 할 때야
    # 없다는 걸 알게 된다.
    #
    # 변수 이름의 접두사가 READLINE 이 아니라 LIBREADLINE 인 점에 주의.
    # (CPython 의 PKG_CHECK_MODULES 첫 인자가 LIBREADLINE 이다)
    #
    # LIBS="-lm" 은 순서 때문이다. 정적 링크에서 링커는 라이브러리를
    # 왼쪽에서 오른쪽으로 한 번만 훑으면서, 그 시점에 필요한 심볼만
    # 꺼내 쓰고 넘어간다. configure 가 만드는 시험 링크 줄은
    #     ... -lsqlite3 -lm -static ... conftest.c -lsqlite3 -ldl
    # 이라서 -lm 이 뒤쪽 -lsqlite3 보다 앞에 온다. 그래서 sqlite3 가
    # 요구하는 log/pow/sin 이 undefined 로 남고, configure 는 sqlite3 가
    # 없다고 판단한다. LIBS 는 링크 줄 맨 뒤에 붙으므로 여기에 두면
    # 순서가 맞는다.


    # -D_GNU_SOURCE, explicitly.
    #
    # CPython builds with -std=c11, which is the STRICT standard: glibc
    # then hides everything POSIX adds, including stack_t, dev_t and
    # ino_t, and the build stops with "unknown type name" in its own
    # headers. configure normally works out that it needs _GNU_SOURCE
    # and writes it into pyconfig.h; on a fresh cross-configure here it
    # decided otherwise, and the failure names types rather than the
    # missing feature macro, which is a long way from the cause.
    ./configure \
        --host=${HOST_TRIPLE} \
        --build=${BUILD_TRIPLE} \
        --with-build-python="$BUILD_PY" \
        --prefix=/data/python \
        --disable-shared \
        --disable-test-modules \
        --with-ensurepip=no \
        --with-openssl="$SYSROOT" \
        --with-readline=readline \
        --enable-loadable-sqlite-extensions=no \
        ac_cv_file__dev_ptmx=yes \
        ac_cv_file__dev_ptc=no \
        ac_cv_buggy_getaddrinfo=no \
        CC="${CROSS}gcc" \
        CXX="${CROSS}g++" \
        AR="${CROSS}ar" \
        RANLIB="${CROSS}ranlib" \
        READELF="${CROSS}readelf" \
        CFLAGS="-D_GNU_SOURCE -Os -fno-semantic-interposition -I${SYSROOT}/include" \
        CPPFLAGS="-D_GNU_SOURCE -I${SYSROOT}/include" \
        LDFLAGS="$LINK_FLAGS" \
        LIBFFI_INCLUDEDIR="${SYSROOT}/include" \
        LIBREADLINE_CFLAGS="-I${SYSROOT}/include -I${SYSROOT}/include/ncursesw" \
        LIBREADLINE_LIBS="-L${SYSROOT}/lib -lreadline -lncursesw" \
        LIBSQLITE3_CFLAGS="-I${SYSROOT}/include" \
        LIBSQLITE3_LIBS="-L${SYSROOT}/lib -lsqlite3 -lm" \
        LIBS="-lm" \
        "${DISABLED[@]}" \
        > /tmp/py-conf.log 2>&1 || { tail -25 /tmp/py-conf.log; die "configure 실패"; }
    echo "  완료"
else
    echo "  이미 설정됨 (다시 하려면 RECONFIGURE=1)"
fi

# ── 확장 모듈을 전부 바이너리 안에 넣는다 ────────────────────────
#
# CPython 은 --disable-shared 로 지어도 표준 확장 모듈(array, math,
# socket, _ctypes ...)은 여전히 .so 로 만들려고 한다. LDFLAGS 에 -static
# 이 있으면 그 링크가 이렇게 깨진다:
#
#   undefined reference to `_start'
#   hidden symbol `__fini_array_end' isn't defined
#
# 정적 libc 를 공유 오브젝트로 링크하려 해서 나는 오류다. 게다가 정적
# 바이너리는 dlopen 을 못 하므로 .so 를 만들어봐야 부를 수도 없다.
#
# 해결: 모듈 정의를 Modules/Setup.local 로 복사하면서 맨 위의 *shared*
# 를 *static* 으로 바꾼다. makesetup 은 파일을 준 순서대로 읽고 먼저
# 나온 정의를 쓰므로, Setup.local 의 정적 정의가 Setup.stdlib 의 공유
# 정의를 덮는다.
#
# Setup.stdlib 뒷부분의 xxlimited 는 '반드시 공유'여야 하는 테스트용
# 모듈이라 아예 주석 처리한다. 우리에게 필요 없다.
step "확장 모듈을 정적으로 (Setup.local 생성)"
SECOND_SHARED=$(grep -n '^\*shared\*' Modules/Setup.stdlib | sed -n '2p' | cut -d: -f1)
[[ -n "$SECOND_SHARED" ]] || die "Modules/Setup.stdlib 의 형식이 예상과 다릅니다"
{
    echo "# tools/build-python.sh 가 자동 생성. 직접 고치지 마세요."
    echo "# 정적 링크 바이너리는 .so 를 못 부른다. 모든 모듈을 안에 넣는다."
    sed -n "1,$((SECOND_SHARED - 1))p" Modules/Setup.stdlib \
        | sed 's/^\*shared\*$/*static*/'
} > Modules/Setup.local
sed -i 's/^\(xxlimited.*\)$/#\1/' Modules/Setup.stdlib
rm -f Modules/*.so
make Makefile > /tmp/py-makefile.log 2>&1 || { tail -20 /tmp/py-makefile.log; die "Makefile 재생성 실패"; }
# 남은 공유 모듈이 있으면 링크에서 또 깨진다. 여기서 잡는다.
if grep -qE '^\s*Modules/[A-Za-z_]+\.cpython.*\.so:' Makefile; then
    grep -oE 'Modules/[A-Za-z_]+\.cpython[^:]*\.so' Makefile | sort -u
    die "아직 공유로 빌드되는 모듈이 있습니다"
fi
echo "  $(grep -c '^[a-z_]' Modules/Setup.local)개 모듈을 정적으로"

step "빌드 (-j${JOBS}) — 오래 걸립니다"
if ! make -j"$JOBS" > /tmp/py-make.log 2>&1; then
    # 'error' 로만 거르면 안 된다. gcc 명령줄마다 들어 있는
    # -Werror=implicit-function-declaration 이 전부 걸려서 진짜 오류가
    # 파묻힌다. 실제 오류 형태만 고른다.
    #
    # 또 '| head' 를 쓰면 head 가 파이프를 닫아 grep 이 SIGPIPE 로 죽고,
    # pipefail 때문에 이 블록이 141 로 끝나면서 아래 die 가 실행되지도
    # 못한 채 스크립트가 조용히 종료된다. awk 로 세면서 자른다.
    echo "  --- 오류 ---"
    grep -E "error:|undefined reference|cannot find -l|No such file or directory|^make.*Error" \
        /tmp/py-make.log | awk 'NR <= 25' || true
    die "빌드 실패 (전체 로그: /tmp/py-make.log)"
fi
echo "  완료"

step "스테이징 디렉터리에 설치"
rm -rf "$STAGE"
make install DESTDIR="$STAGE" > /tmp/py-install.log 2>&1 \
    || { tail -20 /tmp/py-install.log; die "설치 실패"; }

PYDIR="${STAGE}/data/python"
[[ -d "$PYDIR" ]] || die "설치 결과가 없습니다: $PYDIR"

# ── pip ──────────────────────────────────────────────────────────────
#
# Not through ensurepip: that runs the target interpreter to install
# itself, and the target interpreter does not run here. But a wheel is a
# zip of files that belong in site-packages, and pip is pure Python - so
# unpacking the wheel CPython already ships is the same install, done by
# hand and without needing to run anything.
if [[ "$STATIC" != "1" ]]; then
    step "pip 설치 (번들 휠에서)"
    PIP_WHEEL=$(ls "${PY_SRC}/Lib/ensurepip/_bundled/"pip-*.whl 2>/dev/null | head -1 || true)
    SITE="${PYDIR}/lib/python${HOST_VER}/site-packages"

    if [[ -n "$PIP_WHEEL" ]]; then
        mkdir -p "$SITE"
        "$BUILD_PY" -c "
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
" "$PIP_WHEEL" "$SITE"

        # The console script the wheel does not carry. Its shebang is the
        # path on the device, not on this machine.
        cat > "${PYDIR}/bin/pip" <<PIPEOF
#!/data/python/bin/python${HOST_VER}
import sys
from pip._internal.cli.main import main
sys.exit(main())
PIPEOF
        chmod 755 "${PYDIR}/bin/pip"
        cp "${PYDIR}/bin/pip" "${PYDIR}/bin/pip3"
        echo "  $(basename "$PIP_WHEEL")"
    else
        echo "  경고: 번들 pip 휠이 없습니다 (${PY_SRC}/Lib/ensurepip/_bundled/)"
    fi
fi

step "불필요한 것 정리"
BEFORE=$(du -sm "$PYDIR" | cut -f1)

# 타겟에서 쓰지 않는 것들.
#
# libpython3.12.a 가 68MB 로 설치본의 4분의 3이다. C 확장 모듈을 새로
# 컴파일할 때 쓰는 정적 라이브러리인데, 기기에 컴파일러가 없으므로
# 쓸 일이 없다. 지우면 88MB -> 20MB 가 된다.
#
# tkinter/idlelib/turtle 은 GUI 용이라 화면 없는 이 시스템에서 못 쓴다.
# python3.12-config 와 include/ 는 확장 모듈 빌드용이라 같이 나간다.
rm -rf "${PYDIR}/lib/libpython${HOST_VER}.a" \
       "${PYDIR}/lib/python${HOST_VER}/idlelib" \
       "${PYDIR}/lib/python${HOST_VER}/tkinter" \
       "${PYDIR}/lib/python${HOST_VER}/turtledemo" \
       "${PYDIR}/lib/python${HOST_VER}/turtle.py" \
       "${PYDIR}/lib/python${HOST_VER}/lib2to3" \
       "${PYDIR}/lib/python${HOST_VER}/ensurepip" \
       "${PYDIR}/lib/python${HOST_VER}/config-"* \
       "${PYDIR}/lib/pkgconfig" \
       "${PYDIR}/share" \
       "${PYDIR}/include" \
       "${PYDIR}/bin/idle"* \
       "${PYDIR}/bin/2to3"* \
       "${PYDIR}/bin/python${HOST_VER}-config" \
       "${PYDIR}/bin/python3-config" 2>/dev/null || true

# 소스(.py)를 지우고 바이트코드만 남기면 절반쯤 줄지만, 오류 메시지에
# 소스 줄이 안 나와 디버깅이 어려워진다. 지금은 소스를 남긴다.
find "$PYDIR" -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
find "$PYDIR" -name '*.pyc' -delete 2>/dev/null || true

"${CROSS}strip" "${PYDIR}/bin/python${HOST_VER}" 2>/dev/null || true

AFTER=$(du -sm "$PYDIR" | cut -f1)
echo "  ${BEFORE}MB -> ${AFTER}MB"

# ── glibc ────────────────────────────────────────────────────────────
#
# What a dynamically linked Python and the extension modules pip
# installs actually need at runtime. Not the whole of glibc - the
# loader, the C library, and the two libraries a compiled extension is
# likely to have been built against.
#
# libm and libpthread stopped being separate files in glibc 2.34; they
# are inside libc.so.6 now, and the .so.6 / .so.0 names are kept only so
# that older binaries still resolve. We copy them when they are there.
if [[ "$STATIC" != "1" ]]; then
    step "glibc 스테이징 (${GLIBC_SRC} -> ${STAGE}${GLIBC_DIR})"
    GDIR="${STAGE}${GLIBC_DIR}"
    rm -rf "$GDIR"
    mkdir -p "$GDIR"

    # The manylinux policy is a list of libraries a wheel is allowed to
    # assume the system provides, instead of carrying its own copy. This
    # is that list, minus the X11 and OpenGL half, which is of no use to
    # a board with no display:
    #
    #   libnsl, libatomic, libanl   on the list, and cheap
    #   libgomp                     OpenMP. scikit-learn and lightgbm
    #                               reach for it
    #   libmvec                     glibc's vectorised math, which gcc
    #                               emits calls to at -O2 on aarch64
    #
    # Anything absent is skipped silently; the set differs between
    # glibc versions, and libm/libpthread stopped being separate files
    # in glibc 2.34 (they are inside libc.so.6 now, and the .so.6/.so.0
    # names survive only so older binaries still resolve).
    for lib in "${LOADER}" libc.so.6 libm.so.6 libdl.so.2 \
               libpthread.so.0 librt.so.1 libutil.so.1 libresolv.so.2 \
               libgcc_s.so.1 libstdc++.so.6 libnsl.so.1 libcrypt.so.1 \
               libatomic.so.1 libgomp.so.1 libmvec.so.1 libanl.so.1; do
        if [[ -e "${GLIBC_SRC}/${lib}" ]]; then
            cp -L "${GLIBC_SRC}/${lib}" "${GDIR}/${lib}"
            "${CROSS}strip" --strip-unneeded "${GDIR}/${lib}" 2>/dev/null || true
        fi
    done

    # zlib, which is not glibc and not on the manylinux list, and is
    # needed anyway.
    #
    # numpy's wheel carries its own OpenBLAS, and that OpenBLAS has
    # libz.so.1 in its NEEDED list. Without this file, `pip install
    # numpy` succeeds, prints "Successfully installed", and then
    # `import numpy` dies with a linker error four frames deep in
    # numpy's own troubleshooting text. It is the single most installed
    # compiled package on PyPI, so 140KB is not a hard trade.
    # Two places to look, because they are different on the two builds:
    # the cross build has it in the sysroot we made, and a native amd64
    # build takes it from the host - where Debian's multiarch layout
    # puts it under lib/x86_64-linux-gnu, not lib. Looking only in
    # ${SYSROOT}/lib found nothing on amd64, and the warning below was
    # printed and then scrolled past. Python did not start at all:
    # "libz.so.1: cannot open shared object file".
    for dir in "${SYSROOT}/lib" "$GLIBC_SRC"; do
        for zlib in libz.so.1 libz.so.1.3 libz.so.1.3.1; do
            if [[ -e "${dir}/${zlib}" ]]; then
                cp -L "${dir}/${zlib}" "${GDIR}/libz.so.1"
                "${CROSS}strip" --strip-unneeded "${GDIR}/libz.so.1" 2>/dev/null || true
                break 2
            fi
        done
    done
    # And die rather than warn. Python links against it, so without it
    # the interpreter does not start - this is not a numpy nicety.
    [[ -e "${GDIR}/libz.so.1" ]] \
        || die "libz.so.1 을 찾지 못했습니다 (${SYSROOT}/lib, ${GLIBC_SRC}).
       파이썬이 아예 시작하지 못합니다."

    # ── Whatever else it actually asks for ──
    #
    # The list above is the manylinux set plus the ones we knew about,
    # and it was assembled for the cross build, where every optional
    # library came from a sysroot we had built ourselves as a static
    # archive. A native build links against whatever the host has as a
    # shared object instead, so it ends up needing libraries nobody
    # wrote down: libz first, then libbz2, and there is no reason to
    # believe that is the end of the list.
    #
    # So stop guessing. Read DT_NEEDED out of the interpreter and every
    # extension module, follow it transitively, and copy anything that
    # is missing. Guessing produced two rounds of "Python does not
    # start", each one found only by booting the image.
    python3 - "$GDIR" "$PYDIR" "$GLIBC_SRC" /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu <<'NEEDED'
import os, struct, sys, shutil

gdir, pytree = sys.argv[1], sys.argv[2]
search = [d for d in sys.argv[3:] if os.path.isdir(d)]

def needed(path):
    """DT_NEEDED names of an ELF, without a library to read ELF."""
    try:
        d = open(path, 'rb').read()
    except OSError:
        return []
    if d[:4] != b'\x7fELF' or d[4] != 2:
        return []
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shent, shnum = struct.unpack_from('<HH', d, 0x3A)
    out = []
    for i in range(shnum):
        off = shoff + i * shent
        s_type, = struct.unpack_from('<I', d, off + 4)
        if s_type != 6:                     # SHT_DYNAMIC
            continue
        s_off, s_size = struct.unpack_from('<QQ', d, off + 0x18)
        s_link, = struct.unpack_from('<I', d, off + 0x28)
        str_off, = struct.unpack_from('<Q', d, shoff + s_link * shent + 0x18)
        for e in range(0, s_size, 16):
            tag, val = struct.unpack_from('<qQ', d, s_off + e)
            if tag == 0:
                break
            if tag == 1:                    # DT_NEEDED
                end = d.index(b'\x00', str_off + val)
                out.append(d[str_off + val:end].decode())
    return out

roots = []
for base, _, files in os.walk(pytree):
    for f in files:
        if f.endswith('.so') or '/bin/' in os.path.join(base, f):
            roots.append(os.path.join(base, f))

seen, queue, added = set(), list(roots), 0
while queue:
    item = queue.pop()
    for name in needed(item):
        if name in seen:
            continue
        seen.add(name)
        if os.path.exists(os.path.join(gdir, name)):
            queue.append(os.path.join(gdir, name))
            continue
        for d in search:
            cand = os.path.join(d, name)
            if os.path.exists(cand):
                shutil.copyfile(os.path.realpath(cand),
                                os.path.join(gdir, name))
                os.chmod(os.path.join(gdir, name), 0o755)
                print(f"  + {name}")
                added += 1
                queue.append(os.path.join(gdir, name))
                break
        else:
            print(f"  ! {name} 을 찾지 못했습니다", file=sys.stderr)
print(f"  의존성 {added}개 추가")
NEEDED

    echo "  $(ls "$GDIR" | wc -l)개 파일, $(du -sh "$GDIR" | cut -f1)"

    # The loader path written into the binary has to be the one we just
    # staged, or the device gets "No such file or directory" for a file
    # that is plainly there - which is the loader missing, not the
    # program.
    INTERP=$("${CROSS}readelf" -l "${PYDIR}/bin/python${HOST_VER}" 2>/dev/null \
             | sed -n 's/.*interpreter: \([^]]*\)\]/\1/p')
    [[ "$INTERP" == "${GLIBC_DIR}/${LOADER}" ]] \
        || die "인터프리터 경로가 다릅니다: '${INTERP}'"
    echo "  인터프리터: $INTERP"

    # Extension modules resolve PyObject_* out of the executable, which
    # only works if it exports them. Without -export-dynamic every
    # "import numpy" would fail on an undefined symbol.
    # -W: without it readelf elides long names to "P[...]@GLIBC" and no
    # symbol we are looking for is ever spelled out.
    #
    # grep -c rather than grep -q, and the count kept in a variable: with
    # pipefail set, grep -q closes the pipe the moment it matches,
    # readelf dies of SIGPIPE, and the pipeline reports failure for the
    # one case that actually succeeded.
    SYMS=$("${CROSS}readelf" -W --dyn-syms "${PYDIR}/bin/python${HOST_VER}" \
           2>/dev/null | grep -c "PyObject_Init" || true)
    [[ "$SYMS" != "0" ]] \
        || die "동적 심볼이 없습니다 - C 확장 모듈을 import 할 수 없습니다"
    echo "  동적 심볼: 있음 (C 확장 모듈 import 가능)"
fi

step "결과"
file "${PYDIR}/bin/python${HOST_VER}" 2>/dev/null | cut -c1-100 || true
echo ""
echo "  설치 위치: ${PYDIR}"
echo "  (SD 이미지의 /data 파티션에 넣으려면 make sdcard-linux)"
