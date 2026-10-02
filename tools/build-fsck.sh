#!/usr/bin/env bash
#
# build-fsck.sh - e2fsck 를 정적 바이너리로 크로스 빌드한다.
#
#   LP_ARCH=arm64 (기본) / armv6 / amd64
#
# 왜 이게 시스템 이미지(initramfs) 안에 들어가야 하는가:
#
#   ext4 가 깨졌을 때 고칠 도구가 /data 에 있으면 소용이 없다. 고쳐야 할
#   대상이 바로 그 /data 이기 때문이다. errors=remount-ro 가 손상을
#   알아채고 읽기 전용으로 바꾸는 데까지는 하지만, 그 다음에 할 수 있는
#   일이 없으면 카드를 뽑아 PC 로 가져가는 것 말고는 방법이 없다.
#   헤드리스로 선반에 올려둔 보드에서 그건 사실상 복구 불가와 같다.
#
# 정적 링크인 이유:
#
#   우리 initramfs 에는 동적 로더가 없다. 정적 바이너리는 로더가 필요
#   없으므로 glibc 로 정적 링크해도 그대로 돈다.
#
# e2fsck, mke2fs, resize2fs 만 남기고 나머지(dumpe2fs, debugfs ...)는 버린다.
#
# resize2fs 가 여기 있는 이유가 중요하다. expandfs 는 원래 커널의 온라인
# 리사이즈 ioctl(EXT4_IOC_RESIZE_FS) 하나만 썼는데, 그건 마운트된
# 파일시스템을 늘리는 방법이고 CAP_SYS_RESOURCE 와 커널의 온라인 리사이즈
# 지원을 둘 다 요구한다. 하나라도 없으면 파티션만 커지고 파일시스템은
# 그대로인 채로 조용히 끝난다 - 64GB 카드에 124MB 파일시스템이 남는다.
#
# 마운트하기 전에 resize2fs 로 오프라인으로 늘리면 그 조건이 전부
# 사라진다. 그냥 블록 장치에 쓰는 프로그램이고, 이건 실제로 시험할 수
# 있다. 온라인 ioctl 은 이미 마운트돼 있을 때만 쓴다.
#
# 환경변수:
#   E2FS_SRC   소스 경로 (기본 thirdparty/e2fsprogs-1.47.0)
#   JOBS       병렬 빌드 수

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"

THIRDPARTY="${THIRDPARTY:-${WORK}}"
JOBS="${JOBS:-$(nproc)}"

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

# LP_ARCH picks the machine. This program is run as root, at boot, on a
# filesystem that is already damaged - so it has to be built for the
# board it will run on, and a wrong one is not a build error but a card
# that cannot be repaired on the machine it belongs to.
LP_ARCH="${LP_ARCH:-arm64}"
case "$LP_ARCH" in
    arm64)
        CROSS=aarch64-linux-gnu-
        HOST=aarch64-linux-gnu
        CC_FOR_TARGET="aarch64-linux-gnu-gcc"
        # arm64 keeps the original layout so existing trees still work.
        OUT="${REPO_ROOT}/userland/prebuilt"
        SRC_ROOT="${THIRDPARTY}"
        ;;
    armv6)
        # The same story as dropbear: Debian's armhf glibc is ARMv7 and
        # its libgcc is Thumb-2, neither of which an ARM1176 can run.
        # This links against the ARMv6 musl that build-thirdparty.sh
        # builds, through the wrapper it leaves behind.
        CROSS=arm-linux-gnueabi-
        HOST=arm-linux-gnueabi
        CC_FOR_TARGET="${THIRDPARTY}/armv6/musl-gcc"
        OUT="${REPO_ROOT}/userland/prebuilt/armv6"
        SRC_ROOT="${THIRDPARTY}/armv6"
        [[ -x "$CC_FOR_TARGET" ]] || \
            die "${CC_FOR_TARGET} 가 없습니다. 'LP_ARCH=armv6 ./tools/build-thirdparty.sh' 를 먼저 실행하세요."
        ;;
    amd64)
        CROSS=
        HOST=x86_64-linux-gnu
        CC_FOR_TARGET="gcc"
        OUT="${REPO_ROOT}/userland/prebuilt/amd64"
        SRC_ROOT="${THIRDPARTY}/amd64"
        ;;
    *)  die "LP_ARCH 는 arm64 / armv6 / amd64 중 하나여야 합니다" ;;
esac

# On a 32-bit machine, llseek.c falls back to declaring the _llseek
# syscall with the kernel's old _syscall5 macro, which no libc has
# shipped for fifteen years and which does not compile. Saying that file
# offsets are 64 bits takes the branch above it instead, where plain
# lseek is already the 64-bit call.
#
# HAVE_LSEEK64 for the same file in lib/blkid, whose version of that
# fallback does not look at _FILE_OFFSET_BITS at all. musl has lseek64 -
# as `#define lseek64 lseek`, since its off_t is already 64 bits - and
# configure's test looks for a function symbol, so it decides musl does
# not have it while separately deciding the prototype is there.
#
# Through CPPFLAGS, not CFLAGS: the static-library rule these files are
# built by uses CFLAGS_STLIB, which configure sets itself and which
# ignores anything passed in as CFLAGS. CPPFLAGS is in all three.
if [[ "$LP_ARCH" == "armv6" ]]; then
    EXTRA_CFLAGS="-D_FILE_OFFSET_BITS=64 -DHAVE_LSEEK64=1"
else
    EXTRA_CFLAGS=""
fi

E2FS_SRC="${E2FS_SRC:-${SRC_ROOT}/e2fsprogs-1.47.0}"
E2FS_TAR="${THIRDPARTY}/e2fsprogs.tar.gz"
E2FS_URL="https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.0/e2fsprogs-1.47.0.tar.gz"

command -v "${CROSS:-}gcc" >/dev/null || die "${CROSS:-}gcc 가 없습니다"
mkdir -p "$SRC_ROOT"

if [[ ! -d "$E2FS_SRC" ]]; then
    if [[ ! -f "$E2FS_TAR" ]]; then
        step "소스 받기"
        curl --fail --location --silent --show-error \
             --retry 4 --retry-delay 2 --retry-all-errors \
             --output "$E2FS_TAR" "$E2FS_URL" \
            || die "e2fsprogs 다운로드 실패"
    fi
    step "압축 해제"
    # Into the per-machine directory: configure records the compiler and
    # the host triple in the tree, so one tree cannot serve two machines.
    tar xf "$E2FS_TAR" -C "$SRC_ROOT"
fi

cd "$E2FS_SRC"

if [[ ! -f Makefile || "${RECONFIGURE:-0}" == "1" ]]; then
    step "설정"
    # --disable-* 로 빼는 것들:
    #   nls        번역 파일. 영어만 쓴다
    #   uuidd      UUID 데몬. 데몬을 하나 더 돌릴 이유가 없다
    #   fsck       래퍼. 우리가 우리 것을 쓴다
    #   tdb        추적 DB. e2fsck 에 필요 없다
    #
    # libuuid 와 libblkid 는 끄지 않는다. --disable-libuuid 는 "번들
    # 라이브러리를 만들지 말고 시스템 것을 쓰라"는 뜻이라, 크로스
    # 환경에서는 없는 aarch64 libuuid 를 찾다가 configure 가 멈춘다.
    # 번들된 것을 그대로 쓰면 정적으로 함께 링크된다.
    ./configure \
        --host="$HOST" \
        --build=x86_64-linux-gnu \
        --disable-nls \
        --disable-uuidd \
        --disable-fsck \
        --disable-tdb \
        --disable-debugfs \
        --disable-imager \
        --disable-defrag \
        --disable-e2initrd-helper \
        --disable-testio-debug \
        CC="$CC_FOR_TARGET" \
        AR="${CROSS:-}ar" \
        RANLIB="${CROSS:-}ranlib" \
        CFLAGS="-Os" \
        CPPFLAGS="${EXTRA_CFLAGS}" \
        LDFLAGS="-static" \
        > /tmp/e2fs-conf.log 2>&1 \
        || { tail -25 /tmp/e2fs-conf.log; die "configure 실패"; }
    echo "  완료"
fi

step "빌드 (-j${JOBS})"
if ! make -j"$JOBS" > /tmp/e2fs-make.log 2>&1; then
    grep -E "error:|undefined reference|cannot find -l|^make.*Error" \
        /tmp/e2fs-make.log | awk 'NR <= 20' || true
    die "빌드 실패 (전체 로그: /tmp/e2fs-make.log)"
fi
echo "  완료"

# 두 개를 가져간다.
#
#   e2fsck    망가진 /data 를 부팅할 때 고친다
#   mke2fs    새 디스크를 /data 로 만든다 (datadisk --format)
#   resize2fs 카드 전체를 쓰도록 /data 를 늘린다 (expandfs)
#
# 둘 다 부트 파티션(FAT)에 실린다. 시스템 이미지에 넣으면 initramfs 가
# 램에 상주하므로 3MB 를 영구히 먹는데, 둘 다 평소에는 아무 일도 하지
# 않는 프로그램이다. 부트 파티션에 두면 램을 쓰지 않고, 읽기 전용으로
# 마운트되므로 자기가 고칠 파일시스템의 고장에 휩쓸리지도 않는다.
step "결과"
mkdir -p "$OUT"

for tool in e2fsck mke2fs resize2fs; do
    case "$tool" in
        e2fsck)   BIN="${E2FS_SRC}/e2fsck/e2fsck" ;;
        mke2fs)   BIN="${E2FS_SRC}/misc/mke2fs" ;;
        resize2fs) BIN="${E2FS_SRC}/resize/resize2fs" ;;
    esac
    [[ -f "$BIN" ]] || die "${tool} 이 만들어지지 않았습니다"

    cp "$BIN" "${OUT}/${tool}"
    "${CROSS:-}strip" "${OUT}/${tool}"

    # 동적 링크로 나오면 우리 시스템에서 실행되지 않는다. 여기서 잡는다.
    INTERP=$("${CROSS:-}readelf" -l "${OUT}/${tool}" 2>/dev/null \
             | grep -c "interpreter" || true)
    [[ "$INTERP" == "0" ]] \
        || die "${tool} 이 정적이 아닙니다 - 이 시스템에는 로더가 없습니다"

    printf '  %s  (%s bytes, 정적)\n' \
        "${OUT}/${tool}" "$(stat -c%s "${OUT}/${tool}")"
done

# mke2fs 는 /etc/mke2fs.conf 를 읽는다. 없으면 컴파일 시 박힌 기본값으로
# 돌긴 하지만, 어떤 기능을 켤지가 그 파일에 있으므로 함께 실어둔다.
if [[ -f "${E2FS_SRC}/misc/mke2fs.conf.in" ]]; then
    sed -e 's/@[A-Z_]*@//g' "${E2FS_SRC}/misc/mke2fs.conf.in" \
        > "${OUT}/mke2fs.conf"
    printf '  %s  (%s bytes)\n' \
        "${OUT}/mke2fs.conf" "$(stat -c%s "${OUT}/mke2fs.conf")"
fi
