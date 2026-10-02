#!/usr/bin/env bash
#
# mkdist.sh - the three things somebody else gets.
#
#   dist/test_a_123_LPzero2W_linux-utm.zip     arm64, virtual machines.
#         QEMU, UTM and UTM SE. UEFI only, so it carries one compressed
#         kernel (vmlinuz.efi, 11MB) and no GPU firmware.
#
#   dist/linux-LP_arm64_Zero2W.img.xz          arm64, a real Pi Zero 2 W.
#         Also boots in a VM. It has to carry the uncompressed 22MB
#         kernel and start.elf, because the Broadcom GPU firmware loads
#         the kernel itself and cannot decompress one.
#
#   dist/linux-LP_armv6_ZeroW.img.xz           armv6, a real Pi Zero W.
#         A different instruction set, not a smaller version of the same
#         one: an ARM1176 runs no aarch64 instruction at all. Its own
#         userland, its own kernel, its own dropbear, and a zImage the
#         GPU firmware loads.
#
#   dist/linux-LP_desktop.img.xz               amd64, the desktop OS for a PC
#         (the owner's Dell XPS 15 9550). Written to a USB stick it boots
#         on UEFI into the installer, with a persistent root on the
#         stick; the installer copies it onto the internal disk (GPT: ESP,
#         LP-RECOVERY, LP-ROOT). Nothing of it runs from RAM. Built by
#         tools/mkdesktop.sh, which calls tools/mkdisk.sh.
#
#         Before booting it on the XPS: BIOS (F2) -> Secure Boot off, and
#         System Configuration -> SATA Operation -> AHCI; lp-install says
#         so on screen too, and the ESP's README.txt.
#
# (The RAM-live amd64 image, linux-LP_amd64.img.xz, is no longer made:
# the amd64 system is installed on a disk, like any desktop OS.)
#
# Why the arm64 pair and not one image: the universal one has to hold a
# kernel the GPU can read, and a VM has no use for either that or
# start.elf, since UEFI looks only at EFI/BOOT/BOOTAA64.EFI.
#
# Usage:
#   ./tools/mkdist.sh          everything
#   ./tools/mkdist.sh utm      arm64 VM only
#   ./tools/mkdist.sh sd       arm64 Pi image only  (Zero 2 W)
#   ./tools/mkdist.sh armv6    armv6 Pi image only  (Zero W)
#   ./tools/mkdist.sh amd64    the amd64 desktop image only
#
# Every image is built here, by this script, and nowhere else. The two
# Pi images used to be assembled by hand from the notes in a session
# log, which is another way of saying they could not be rebuilt.
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
DIST="${REPO_ROOT}/dist"
IMG="${REPO_ROOT}/sdcard/lp-zero.img"

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }
log()  { printf '  %s\n' "$*"; }

WHAT="${1:-all}"
case "$WHAT" in all|utm|sd|armv6|amd64) ;; *) die "알 수 없는 인자: $WHAT" ;; esac

command -v xz  >/dev/null || die "xz 가 없습니다 (apt install xz-utils)"
command -v zip >/dev/null || die "zip 이 없습니다 (apt install zip)"

mkdir -p "$DIST"

# ── 가상머신용 ───────────────────────────────────────────────────
if [[ "$WHAT" == "all" || "$WHAT" == "utm" ]]; then
    step "가상머신용 이미지 (UEFI 만)"
    "${REPO_ROOT}/tools/mksdcard.sh" --linux --uefi-only > /dev/null
    [[ -f "$IMG" ]] || die "이미지가 만들어지지 않았습니다"

    # zip 은 sparse 를 모른다. 이미지를 그대로 넣으면 256MB 를 통째로
    # 압축하게 되지만, 빈 곳은 0 이라 실제로는 잘 줄어든다.
    ( cd "${REPO_ROOT}/sdcard" \
      && rm -f "${DIST}/test_a_123_LPzero2W_linux-utm.zip" \
      && cp lp-zero.img test_a_123_LPzero2W_linux.img \
      && zip -q -9 "${DIST}/test_a_123_LPzero2W_linux-utm.zip" test_a_123_LPzero2W_linux.img \
      && rm -f test_a_123_LPzero2W_linux.img )
    log "test_a_123_LPzero2W_linux-utm.zip  $(stat -c%s "${DIST}/test_a_123_LPzero2W_linux-utm.zip") bytes"
fi

# ── SD 카드용 ────────────────────────────────────────────────────
if [[ "$WHAT" == "all" || "$WHAT" == "sd" ]]; then
    step "Pi Zero 2 W 이미지 (arm64, 실기 + 가상머신)"

    # Its inputs are built here too. Assuming they are current is how a
    # kernel with last week's initramfs in it gets shipped: the rootfs
    # is compiled into the kernel image, so a stale kernel is a stale
    # userland with no way to tell from the outside.
    make -C "${REPO_ROOT}/userland" >/dev/null
    ( cd "${REPO_ROOT}/userland" && ./mkrootfs.sh >/dev/null )
    "${REPO_ROOT}/kernel/build.sh" >/dev/null
    "${REPO_ROOT}/tools/mksdcard.sh" --linux > /dev/null
    [[ -f "$IMG" ]] || die "이미지가 만들어지지 않았습니다"

    rm -f "${DIST}/linux-LP_arm64_Zero2W.img.xz"
    # -T0: 코어 수만큼 스레드. 256MB 를 한 스레드로 짜면 오래 걸린다.
    xz -9 -T0 -c "$IMG" > "${DIST}/linux-LP_arm64_Zero2W.img.xz"
    log "linux-LP_arm64_Zero2W.img.xz  $(stat -c%s "${DIST}/linux-LP_arm64_Zero2W.img.xz") bytes"
fi

# ── armv6 (Pi Zero W) ────────────────────────────────────────────
if [[ "$WHAT" == "all" || "$WHAT" == "armv6" ]]; then
    step "Pi Zero W 이미지 (armv6)"

    # Everything on this line differs from arm64: the compiler target,
    # the binaries, the root filesystem, the kernel and the boot files.
    # Nothing is shared but the source.
    make -C "${REPO_ROOT}/userland" ARCH=armv6 >/dev/null
    ( cd "${REPO_ROOT}/userland" \
      && LP_ARCH=armv6 LP_BINDIR=bin-armv6 LP_ROOTFS_DIR=rootfs-armv6 \
         LP_CPIO_NAME=initramfs-armv6.cpio.gz ./mkrootfs.sh >/dev/null )
    LP_ARCH=armv6 LP_ROOTFS_DIR=rootfs-armv6 "${REPO_ROOT}/kernel/build.sh" >/dev/null
    LP_ARCH=armv6 LP_ROOTFS_DIR=rootfs-armv6 \
        "${REPO_ROOT}/tools/mksdcard.sh" --linux >/dev/null
    [[ -f "$IMG" ]] || die "armv6 이미지가 만들어지지 않았습니다"

    rm -f "${DIST}/linux-LP_armv6_ZeroW.img.xz"
    xz -9 -T0 -c "$IMG" > "${DIST}/linux-LP_armv6_ZeroW.img.xz"
    log "linux-LP_armv6_ZeroW.img.xz  $(stat -c%s "${DIST}/linux-LP_armv6_ZeroW.img.xz") bytes"

    # Put the arm64 userland and rootfs back: the Pi images share
    # kernel/out and userland/build only through this script's ordering.
    # (The amd64 desktop has its own - kernel/out-amd64-disk,
    # userland/build-amd64 - and needs no such step.)
    make -C "${REPO_ROOT}/userland" >/dev/null
    ( cd "${REPO_ROOT}/userland" && ./mkrootfs.sh >/dev/null )
fi

# ── amd64: the desktop ───────────────────────────────────────────
if [[ "$WHAT" == "all" || "$WHAT" == "amd64" ]]; then
    step "amd64 desktop image (installer USB stick, installs to the NVMe)"

    # Its inputs: the amd64 userland and rootfs, and lp-base made from
    # them - an older package would put last week's commands on the
    # image. The Debian base is tools/apply-packages.sh's.
    make -C "${REPO_ROOT}/userland" ARCH=amd64 >/dev/null
    ( cd "${REPO_ROOT}/userland" \
      && LP_ARCH=amd64 LP_BINDIR=bin-amd64 LP_ROOTFS_DIR=rootfs-amd64 \
         LP_CPIO_NAME=initramfs-amd64.cpio.gz \
         LP_HOSTNAME=linux-lp LP_OS_NAME=linux-LP ./mkrootfs.sh >/dev/null )
    "${REPO_ROOT}/tools/mkdeb.sh" amd64 >/dev/null
    "${REPO_ROOT}/tools/mkdesktop.sh"
    DESK="${REPO_ROOT}/sdcard/linux-LP_desktop.img"
    [[ -f "$DESK" ]] || die "the desktop image was not made"

    rm -f "${DIST}/linux-LP_desktop.img.xz" "${DIST}/linux-LP_amd64.img.xz"
    # The image is sparse: the recovery partition and the root's free
    # space are holes, and xz reads them as the zeros they are.
    xz -9 -T0 -c "$DESK" > "${DIST}/linux-LP_desktop.img.xz"
    log "linux-LP_desktop.img.xz  $(stat -c%s "${DIST}/linux-LP_desktop.img.xz") bytes"
    # The raw image is several GB; the compressed one is the product.
    rm -f "$DESK"
fi

# 체크섬은 여기서 만든다.
#
# 손으로 적어두었더니 이미지를 다시 빌드할 때마다 조용히 어긋났다.
# 받는 사람 입장에서 맞지 않는 sha256 은 없느니만 못하다 - 파일이
# 깨진 것인지 목록이 낡은 것인지 구별할 방법이 없기 때문이다.
# 이미지를 만든 자리에서 같이 만들어야 어긋날 수가 없다.
step "체크섬"
( cd "$DIST" && rm -f SHA256SUMS.txt \
  && sha256sum *.img.xz *.zip > SHA256SUMS.txt 2>/dev/null || true )
if [[ -f "${DIST}/linux-LP_arm64_Zero2W.img.xz" ]]; then
    ( cd "$DIST" && sha256sum linux-LP_arm64_Zero2W.img.xz \
        > PI_IMAGE_SHA256.txt )
fi
while read -r _ name; do log "$name"; done < "${DIST}/SHA256SUMS.txt"

# 다운로드 페이지도 여기서 같이 만든다.
#
# 페이지에는 파일 크기 셋과 sha256 셋이 들어간다. 이미지를 다시
# 빌드하고 페이지를 잊으면 그 숫자들이 조용히 거짓말을 하기 시작한다 -
# 체크섬 파일이 정확히 그렇게 네 번의 재빌드 동안 방치됐었다.
# 이미지를 만드는 자리에서 같이 만들면 어긋날 수가 없다.
step "다운로드 페이지"
"${REPO_ROOT}/tools/mkweb.sh"

step "결과"
for f in "${DIST}"/*; do
    [[ -f "$f" ]] || continue
    printf '  %-30s %10s bytes\n' "$(basename "$f")" "$(stat -c%s "$f")"
done
echo ""
echo "  SD 카드에 굽기:"
echo "    xz -d < dist/linux-LP_arm64_Zero2W.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress"
echo "    xz -d < dist/linux-LP_armv6_ZeroW.img.xz  | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress   # Zero W"
echo "  UTM/QEMU:"
echo "    unzip dist/test_a_123_LPzero2W_linux-utm.zip   그리고 test_a_123_LPzero2W_linux.img 를 디스크로 붙인다"
