#!/usr/bin/env bash
#
# build.sh - 최소 리눅스 커널을 빌드한다.
#
# 공식 bcm2711_defconfig 에서 출발해 lp-zero.config 조각을 덮어쓴다.
# 처음부터(tinyconfig) 쌓아올리는 방법도 있지만, 부팅에 꼭 필요한 옵션을
# 하나씩 찾아내느라 시간이 훨씬 많이 든다. 검증된 defconfig 에서 깎는 쪽이
# 확실하다.
#
# 우리 유저랜드(userland/rootfs)를 커널 이미지에 내장하므로 별도의
# initramfs 파일이 필요 없다 - 커널 하나만 SD 에 넣으면 부팅된다.
#
# 환경변수:
#   LINUX_SRC   커널 소스 경로 (기본 .build/linux — tools/fetch-kernel.sh 가 받는다)
#   JOBS        병렬 빌드 수 (기본 nproc)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"

LINUX_SRC="${LINUX_SRC:-${LPZERO_WORK}/linux}"
# (set below, once LP_ARCH is known - one tree per architecture,
#  because arm64 and x86 object files in one directory is a
#  link error at best and a wrong kernel at worst)
ROOTFS="${REPO_ROOT}/userland/${LP_ROOTFS_DIR:-rootfs}"
# LP_ARCH picks the machine: arm64 (the Pi and arm64 VMs) or amd64 (an
# ordinary PC). The two differ in the config file, the cross compiler,
# and what the kernel image is called when it comes out - nothing else
# in this script.
LP_ARCH="${LP_ARCH:-arm64}"
# Unconditional, not ${BUILD_DIR:-...}: tools/common.sh has already set
# BUILD_DIR to the shared default, so a ":-" here would silently keep it
# and both architectures would compile into one directory - which is a
# link error if you are lucky and a kernel built from a mixture if you
# are not. LP_BUILD_DIR overrides it if somebody really means to.
BUILD_DIR="${LP_BUILD_DIR:-${LPZERO_WORK}/build-${LP_ARCH}}"

if [[ "$LP_ARCH" == "armv6" ]]; then
    # A Pi Zero W. ARCH=arm, not arm64 - a different instruction set and
    # a different kernel entirely, sharing only the Broadcom peripherals.
    ARCH=arm
    CROSS=arm-linux-gnueabihf-
    KCONFIG_NAME=lp-zero-armv6.config
    # Regenerated every time, from the arm64 config and the armv6
    # fragment, for the same reason as amd64: hand-merging gets the
    # order wrong and merge_config keeps the last answer.
    "${REPO_ROOT}/tools/mkarmv6config.sh" >/dev/null
    OUT_SUBDIR="${LP_OUT_SUBDIR:-out-armv6}"
elif [[ "$LP_ARCH" == "amd64" ]]; then
    ARCH=x86_64
    CROSS=
    KCONFIG_NAME=lp-zero-amd64.config
    # Regenerate it every time, from the arm64 config and the amd64
    # fragment. Keeping the merged file in the tree by hand is what put
    # a Pi's "no 8250 serial" answer after the PC's "yes 8250" one.
    "${REPO_ROOT}/tools/mkamd64config.sh" >/dev/null
    OUT_SUBDIR="${LP_OUT_SUBDIR:-out-amd64}"
else
    ARCH=arm64
    CROSS=aarch64-linux-gnu-
    KCONFIG_NAME=lp-zero.config
    OUT_SUBDIR="${LP_OUT_SUBDIR:-out}"
fi

FRAGMENT="${REPO_ROOT}/kernel/${KCONFIG_NAME}"
OUT_DIR="${REPO_ROOT}/kernel/${OUT_SUBDIR}"

JOBS="${JOBS:-$(nproc)}"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

[[ -d "$LINUX_SRC" ]]  || die "커널 소스가 없습니다: $LINUX_SRC"
[[ -f "$FRAGMENT" ]]   || die "설정 조각이 없습니다: $FRAGMENT"
command -v "${CROSS:-}gcc" >/dev/null || die "${CROSS:-}gcc 가 없습니다"
# EFI_ZBOOT 가 vmlinuz.efi 를 만들 때 커널 Makefile 이 hexdump 로 이미지
# 크기를 읽는다. 없으면 "truncate: Invalid number" 라는, 원인이 전혀
# 드러나지 않는 오류로 7분짜리 빌드가 끝난다.
command -v hexdump >/dev/null || die "hexdump 가 없습니다 (apt install bsdextrautils)"

# 유저랜드가 준비되어 있어야 커널에 내장할 수 있다
if [[ ! -d "$ROOTFS" ]]; then
    step "유저랜드 rootfs 가 없어 먼저 만듭니다"
    make -C "${REPO_ROOT}/userland" >/dev/null
    ( cd "${REPO_ROOT}/userland" && ./mkrootfs.sh >/dev/null )
fi
[[ -e "${ROOTFS}/dev/console" ]] || \
    echo "경고: ${ROOTFS}/dev/console 이 없습니다. init 이 출력을 못 낼 수 있습니다."

# ── Our patches to the source ──
#
# kernel/patches/*.patch, in name order: fixes the pinned commit does not
# have yet (each one's header says what and why). They go into LINUX_SRC
# itself, once - a patch whose reverse applies cleanly is already there -
# so every build directory sees the same source, and make recompiles
# only what they touch. A patch that neither applies nor is already
# applied stops the build rather than producing a kernel without it.
for p in "${REPO_ROOT}"/kernel/patches/*.patch; do
    [[ -f "$p" ]] || continue
    if patch -d "$LINUX_SRC" -p1 -R -s -f --dry-run < "$p" >/dev/null 2>&1; then
        continue
    fi
    patch -d "$LINUX_SRC" -p1 -s -f --dry-run < "$p" >/dev/null \
        || die "$(basename "$p") does not apply to ${LINUX_SRC}"
    step "패치 $(basename "$p")"
    patch -d "$LINUX_SRC" -p1 -s -f --no-backup-if-mismatch < "$p"
done

MAKE_ARGS=(-C "$LINUX_SRC" O="$BUILD_DIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS")

# CLEAN=1 이면 빌드 디렉터리를 비운다.
# 설정을 바꿔 다시 빌드하면 이전 설정으로 만든 .o 가 남아 있어서,
# 나중에 "무엇이 이미지를 키우나"를 오브젝트 크기로 재려 할 때 왜곡된다.
if [[ "${CLEAN:-0}" == "1" ]]; then
    step "빌드 디렉터리 비우기 (CLEAN=1)"
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR" "$OUT_DIR"

# ── 1. 공식 defconfig ────────────────────────────────────────────
if [[ "$LP_ARCH" == "armv6" ]]; then
    # bcmrpi_defconfig is the Pi 1 / Pi Zero / Zero W config: ARMv6,
    # one core. bcm2835_defconfig is the mainline-style one and does not
    # carry the downstream board support this tree has.
    step "bcmrpi_defconfig 적용 (Pi 1 / Zero / Zero W 공용 ARMv6 설정)"
    make "${MAKE_ARGS[@]}" bcmrpi_defconfig >/dev/null
elif [[ "$LP_ARCH" == "amd64" ]]; then
    # There is no vendor defconfig for "a PC", so start from the
    # kernel's own x86_64_defconfig and cut it down with the fragment.
    step "x86_64_defconfig 적용"
    make "${MAKE_ARGS[@]}" x86_64_defconfig >/dev/null
else
    step "bcm2711_defconfig 적용 (Pi 3 / Zero 2 W / Pi 4 공용 arm64 설정)"
    make "${MAKE_ARGS[@]}" bcm2711_defconfig >/dev/null
fi

BEFORE_M=$(grep -c '=m$' "${BUILD_DIR}/.config" || true)
BEFORE_Y=$(grep -c '=y$' "${BUILD_DIR}/.config" || true)
echo "    기준: =y ${BEFORE_Y}개, =m ${BEFORE_M}개"

# ── 2. 우리 조각 병합 ────────────────────────────────────────────
step "${KCONFIG_NAME} 병합"

# ── Firmware for the disk-rooted PC kernel ──
#
# The amd64 kernel that boots from disk has every driver built in, so
# every driver probes while the only filesystem is this kernel's own
# tiny initramfs - seconds before preinit mounts the root. A driver that
# asks for firmware then gets "not found", and the ones on the Dell XPS
# do not ask twice: i915 without its DMC firmware turns runtime power
# management off until the next boot, ath10k and brcmfmac never register
# a WiFi interface, cfg80211 without regulatory.db stays in the world
# domain, and btusb's setup fails before the Bluetooth controller is
# ever announced to userspace.
#
# So the same files go into the initramfs, under /lib/firmware, where
# the firmware loader finds them before the root exists. The image
# carries them in /lib/firmware on disk as well; after preinit chroots,
# the loader reads that one (PID 1 shares its fs_struct with the kernel,
# so the chroot moves the loader's root too), and that is what a reset
# or a resume re-reads. A disk-rooted kernel is the one built with
# root= in LP_CMDLINE (tools/mkdisk.sh); the RAM-live amd64 kernel's
# initramfs is the whole root, and it gets its firmware from there.
#
# tools/fetch-pc-fw.sh is all-or-nothing and checks every file against
# a pinned sha256; run with the set already present, it touches no
# network. A missing set stops the build rather than producing a kernel
# whose WiFi silently does not exist.
EARLY_FW_LIST=""
if [[ "$LP_ARCH" == "amd64" && " ${LP_CMDLINE:-} " == *" root="* ]]; then
    step "PC 펌웨어 (initramfs 의 /lib/firmware)"
    "${REPO_ROOT}/tools/fetch-pc-fw.sh" \
        || die "PC firmware set is incomplete (tools/fetch-pc-fw.sh)"
    FW_DIR="${PC_FW_DIR:-${REPO_ROOT}/blobs/pc-fw}"
    EARLY_FW_LIST="${BUILD_DIR}/lp-early-firmware.list"
    # gen_init_cpio's list format: every directory before what is in it.
    {
        echo "# build.sh 가 생성. tools/fetch-pc-fw.sh --list 의 파일들."
        "${REPO_ROOT}/tools/fetch-pc-fw.sh" --list | while read -r f; do
            d="lib/firmware"
            echo "dir /lib 0755 0 0"
            echo "dir /${d} 0755 0 0"
            for part in $(dirname "$f" | tr '/' ' '); do
                [[ "$part" == "." ]] && continue
                d="${d}/${part}"
                echo "dir /${d} 0755 0 0"
            done
            echo "file /lib/firmware/${f} ${FW_DIR}/${f} 0644 0 0"
        done
        # LP_EARLY_FW_ROOT + LP_EARLY_FW_DIRS: whole firmware directories
        # from the root being built (the desktop image: Debian's
        # /usr/lib/firmware), for built-in drivers on machines other
        # than the XPS. i915 is the one: from Gen12 (Tiger Lake) on it
        # submits work through GuC and loads it at probe, before the
        # root is there, so without its blob there the GPU draws
        # nothing. Files the pinned set above already has are left to
        # it; links are followed, since the initramfs gets the bytes.
        if [[ -n "${LP_EARLY_FW_ROOT:-}" ]]; then
            for sub in ${LP_EARLY_FW_DIRS:-}; do
                [[ -d "${LP_EARLY_FW_ROOT}/${sub}" ]] || continue
                echo "dir /lib 0755 0 0"
                echo "dir /lib/firmware 0755 0 0"
                ( cd "$LP_EARLY_FW_ROOT" && find -L "$sub" -type d | sort ) |
                    while read -r d; do echo "dir /lib/firmware/${d} 0755 0 0"; done
                ( cd "$LP_EARLY_FW_ROOT" && find -L "$sub" -type f | sort ) |
                    while read -r f; do
                        [[ -f "${FW_DIR}/${f}" ]] && continue
                        echo "file /lib/firmware/${f} $(readlink -f "${LP_EARLY_FW_ROOT}/${f}") 0644 0 0"
                    done
            done
        fi
    } | awk '!seen[$0]++' > "$EARLY_FW_LIST"
    echo "    $(grep -c '^file ' "$EARLY_FW_LIST")개 파일, $(du -ch $(awk '/^file /{print $3}' "$EARLY_FW_LIST") | tail -1 | cut -f1)"
fi

# initramfs 경로는 환경마다 다르므로 여기서 만들어 붙인다.
GEN="${BUILD_DIR}/lp-zero-generated.config"
{
    echo "# build.sh 가 생성. 직접 수정하지 말 것."
    # INITRAMFS_SOURCE takes a space-separated list: the root directory,
    # then (disk kernel only) the firmware list above.
    echo "CONFIG_INITRAMFS_SOURCE=\"${ROOTFS}${EARLY_FW_LIST:+ ${EARLY_FW_LIST}}\""
    echo "CONFIG_INITRAMFS_ROOT_UID=0"
    echo "CONFIG_INITRAMFS_ROOT_GID=0"

    # LP_CMDLINE builds a kernel that knows where its root is.
    #
    # The images that carry their whole system in the initramfs need no
    # root= at all. A disk-rooted one does, and it cannot come from a
    # bootloader here: UEFI loads the kernel's own EFI stub directly,
    # with nothing in between to pass a command line. So it is compiled
    # in, and cmdline.txt on the FAT partition can still override it
    # where a real bootloader is involved.
    # CPU microcode, built into the kernel. Linux loads Intel microcode
    # at the very start of boot from an uncompressed cpio in front of the
    # initrd, or from the kernel's built-in firmware - and these kernels
    # are started by UEFI with no initrd at all, so built in it is. The
    # XPS 9550's BIOS leaves its i7-6700HQ at revision 0xd6; this brings
    # it to the one Ubuntu loads (0xf0) before anything else runs.
    if [[ -n "$EARLY_FW_LIST" ]]; then
        UCODE=$("${REPO_ROOT}/tools/fetch-pc-fw.sh" --list | grep '^intel-ucode/' | tr '\n' ' ')
        if [[ -n "$UCODE" ]]; then
            echo "CONFIG_EXTRA_FIRMWARE=\"${UCODE% }\""
            echo "CONFIG_EXTRA_FIRMWARE_DIR=\"${FW_DIR}\""
        fi
    fi

    if [[ -n "${LP_CMDLINE:-}" ]]; then
        echo "CONFIG_CMDLINE_BOOL=y"
        echo "CONFIG_CMDLINE=\"${LP_CMDLINE}\""
    fi
} > "$GEN"

# LP_KCONFIG_EXTRA: more fragments on top, space-separated - the desktop
# image's own (kernel/lp-desktop-amd64.fragment, from tools/mkdisk.sh).
EXTRA_FRAGMENTS=()
for f in ${LP_KCONFIG_EXTRA:-}; do
    [[ -f "$f" ]] || die "설정 조각이 없습니다: $f"
    EXTRA_FRAGMENTS+=("$f")
done
(( ${#EXTRA_FRAGMENTS[@]} )) && echo "    + ${EXTRA_FRAGMENTS[*]##*/}"

"${LINUX_SRC}/scripts/kconfig/merge_config.sh" -m -O "$BUILD_DIR" \
    "${BUILD_DIR}/.config" "$FRAGMENT" "${EXTRA_FRAGMENTS[@]}" "$GEN" >/dev/null

# merge_config 는 의존성을 풀지 않는다. olddefconfig 가 정리한다.
make "${MAKE_ARGS[@]}" olddefconfig >/dev/null

AFTER_M=$(grep -c '=m$' "${BUILD_DIR}/.config" || true)
AFTER_Y=$(grep -c '=y$' "${BUILD_DIR}/.config" || true)
echo "    결과: =y ${AFTER_Y}개, =m ${AFTER_M}개"

# 조각이 실제로 반영됐는지 전부 대조한다.
#
# kconfig 의 select 는 사용자 설정을 무시하고 심볼을 강제로 켠다. 그래서
# 조각에 CONFIG_X=n 을 적어도 다른 켜진 옵션이 X 를 select 하면 y 로
# 남는다. 이 경우 X 가 아니라 "X 를 select 하는 쪽"을 꺼야 한다.
#
# 조용히 무시되면 왜 이미지가 안 줄어드는지 알 수 없으므로 전부 보고한다.
#
# Each symbol is checked against the LAST value the file gives it, which
# is the one merge_config applies. The amd64 and armv6 configs are the
# arm64 answers followed by their own (tools/mk*config.sh), so a
# symbol can be there twice, and checking every line reported each Pi
# answer the PC overrides on purpose - forty-odd lines of noise on every
# amd64 build, in which the two real misses (virtio-scsi and Hyper-V
# storage, off because the Pi turned their menu off) sat unread. What
# still shows up is a Pi answer the PC cannot take - a symbol this
# kernel version no longer has, or one with no prompt on x86 (AIO and
# IO_URING are forced on without EXPERT) - and a choice member the Pi
# picked differently, unless the PC fragment says =n to it (the amd64
# one does).
step "조각 반영 상태 대조"

MISSED=0
while read -r line; do
    sym="${line%%=*}"
    want="${line##*=}"
    case "$want" in
    n)
        if grep -qx "${sym}=y" "${BUILD_DIR}/.config" 2>/dev/null; then
            echo "    미반영(y)  ${sym}"
            MISSED=$((MISSED + 1))
        elif grep -qx "${sym}=m" "${BUILD_DIR}/.config" 2>/dev/null; then
            echo "    미반영(m)  ${sym}"
            MISSED=$((MISSED + 1))
        fi
        ;;
    y|m)
        grep -qx "${sym}=${want}" "${BUILD_DIR}/.config" 2>/dev/null || {
            echo "    미반영      ${sym} (=${want} 를 원했음)"
            MISSED=$((MISSED + 1))
        }
        ;;
    esac
done < <(cat "$FRAGMENT" "${EXTRA_FRAGMENTS[@]}" | grep -E '^CONFIG_[A-Z0-9_]+=(y|m|n)$' | awk -F= '
            !($1 in last) { order[++n] = $1 }
            { last[$1] = $2 }
            END { for (i = 1; i <= n; i++) print order[i] "=" last[order[i]] }')

TOTAL=$(cat "$FRAGMENT" "${EXTRA_FRAGMENTS[@]}" | grep -E '^CONFIG_[A-Z0-9_]+=(y|m|n)$' | cut -d= -f1 | sort -u | wc -l)
if [[ "$MISSED" == "0" ]]; then
    echo "    전부 반영됨 (${TOTAL}개)"
else
    echo ""
    echo "    ${TOTAL}개 중 ${MISSED}개가 반영되지 않았습니다."
    echo "    대부분 다른 옵션이 select 로 강제하는 경우입니다."
    echo "    선택자를 찾으려면:"
    echo "      grep -rn --include='Kconfig*' 'select <심볼이름>\\b' ${LINUX_SRC}"
fi

# ── 3. 빌드 ──────────────────────────────────────────────────────
step "빌드 시작 (-j${JOBS}) — 몇 분 걸립니다"
# vmlinuz.efi 는 EFI_ZBOOT 의 산물이다. Image 를 압축해 EFI 스텁으로
# 감싼 것으로, UEFI 부팅 경로(QEMU/UTM)에서 FAT 파티션 공간을 절반 넘게
# 아낀다. 실기 Pi 는 GPU 펌웨어가 압축을 풀 줄 모르므로 Image 를 그대로
# 쓴다 - 그래서 둘 다 만든다.
if [[ "$LP_ARCH" == "armv6" ]]; then
    # zImage, not Image: on 32-bit ARM the kernel carries its own
    # decompressor and the GPU firmware jumps straight into it. There is
    # no EFI path on this board, so there is nothing to build twice.
    time make "${MAKE_ARGS[@]}" -j"$JOBS" zImage dtbs

    step "결과"
    cp "${BUILD_DIR}/arch/arm/boot/zImage" "${OUT_DIR}/zImage"
    printf "  zImage %s bytes (%.1f MB)\n" \
        "$(stat -c%s "${OUT_DIR}/zImage")" \
        "$(echo "scale=2; $(stat -c%s "${OUT_DIR}/zImage")/1048576" | bc)"

    DTB_SRC="${BUILD_DIR}/arch/arm/boot/dts/broadcom/bcm2708-rpi-zero-w.dtb"
    if [[ -f "$DTB_SRC" ]]; then
        cp "$DTB_SRC" "${OUT_DIR}/"
        echo "  DTB   $(stat -c%s "${OUT_DIR}/bcm2708-rpi-zero-w.dtb") bytes"
    else
        echo "  경고: Zero W DTB 를 찾지 못했습니다"
    fi

    # disable-bt moves PL011 off the Bluetooth chip and onto the header
    # pins; without it the board boots with nothing on the serial
    # console. dwc2 puts the one micro-USB port into OTG mode.
    OVL_DIR="${BUILD_DIR}/arch/arm/boot/dts/overlays"
    mkdir -p "${OUT_DIR}/overlays"
    for ovl in disable-bt dwc2; do
        if [[ -f "${OVL_DIR}/${ovl}.dtbo" ]]; then
            cp "${OVL_DIR}/${ovl}.dtbo" "${OUT_DIR}/overlays/"
            echo "  오버레이 ${ovl}.dtbo"
        else
            echo "  경고: ${ovl}.dtbo 를 찾지 못했습니다"
        fi
    done
    echo
    echo "완료: ${OUT_DIR}"
    exit 0
fi

if [[ "$LP_ARCH" == "amd64" ]]; then
    # bzImage carries the EFI stub itself on x86, so there is one image
    # rather than the Image + vmlinuz.efi pair arm64 needs, and there
    # are no device trees - a PC describes itself through ACPI.
    time make "${MAKE_ARGS[@]}" -j"$JOBS" bzImage

    step "결과"
    cp "${BUILD_DIR}/arch/x86/boot/bzImage" "${OUT_DIR}/bzImage"
    echo "  bzImage  $(stat -c%s "${OUT_DIR}/bzImage") bytes"

    # The drivers for machines other than the XPS (kernel/lp-desktop-
    # amd64.fragment): built, stripped, signed with this build's key
    # (certs/signing_key.pem, made by the kernel build on first use and
    # kept in the build directory, never in the repo), compressed, and
    # put under OUT_DIR/modules/lib/modules/<release> with depmod's
    # indexes - the tree tools/mkdisk.sh copies into the root. Always the
    # whole tree: a module left over from an earlier config would carry
    # another build's signature and never load.
    rm -rf "${OUT_DIR}/modules"
    if grep -q '^CONFIG_MODULES=y' "${BUILD_DIR}/.config"; then
        step "모듈"
        command -v depmod >/dev/null || die "depmod 가 없습니다 (apt install kmod)"
        time make "${MAKE_ARGS[@]}" -j"$JOBS" modules
        make "${MAKE_ARGS[@]}" INSTALL_MOD_PATH="${OUT_DIR}/modules" \
            INSTALL_MOD_STRIP=1 modules_install >/dev/null
        KREL=$(cat "${BUILD_DIR}/include/config/kernel.release")
        rm -f "${OUT_DIR}/modules/lib/modules/${KREL}/build" \
              "${OUT_DIR}/modules/lib/modules/${KREL}/source"
        [[ -s "${OUT_DIR}/modules/lib/modules/${KREL}/modules.alias" ]] \
            || die "modules_install left no modules.alias (depmod)"
        echo "  모듈 $(find "${OUT_DIR}/modules" -name '*.ko*' | wc -l)개, $(du -sh "${OUT_DIR}/modules" | cut -f1), ${KREL}"
    fi
    echo
    echo "완료: ${OUT_DIR}"
    exit 0
fi

time make "${MAKE_ARGS[@]}" -j"$JOBS" Image vmlinuz.efi dtbs

# ── 4. 결과 수집 ─────────────────────────────────────────────────
step "결과"
cp "${BUILD_DIR}/arch/arm64/boot/Image" "${OUT_DIR}/Image"

ZBOOT="${BUILD_DIR}/arch/arm64/boot/vmlinuz.efi"
if [[ -f "$ZBOOT" ]]; then
    cp "$ZBOOT" "${OUT_DIR}/vmlinuz.efi"
else
    rm -f "${OUT_DIR}/vmlinuz.efi"
    echo "  경고: vmlinuz.efi 가 없습니다 (CONFIG_EFI_ZBOOT)."
    echo "        UEFI 경로도 압축되지 않은 Image 를 쓰게 됩니다."
fi

DTB_SRC="${BUILD_DIR}/arch/arm64/boot/dts/broadcom/bcm2710-rpi-zero-2-w.dtb"
if [[ -f "$DTB_SRC" ]]; then
    cp "$DTB_SRC" "${OUT_DIR}/"
    echo "  DTB   $(stat -c%s "${OUT_DIR}/bcm2710-rpi-zero-2-w.dtb") bytes"
else
    echo "  경고: Zero 2 W DTB 를 찾지 못했습니다"
fi

# disable-bt 오버레이. Zero 2 W 에서 PL011 을 40핀 헤더로 돌리는 데
# 필요하다. 없으면 시리얼 콘솔이 나오지 않는다.
OVL_DIR="${BUILD_DIR}/arch/arm64/boot/dts/overlays"
mkdir -p "${OUT_DIR}/overlays"
for ovl in disable-bt dwc2; do
    if [[ -f "${OVL_DIR}/${ovl}.dtbo" ]]; then
        cp "${OVL_DIR}/${ovl}.dtbo" "${OUT_DIR}/overlays/"
        echo "  오버레이 ${ovl}.dtbo"
    else
        echo "  경고: ${ovl}.dtbo 를 찾지 못했습니다"
    fi
done

IMG_SIZE=$(stat -c%s "${OUT_DIR}/Image")
printf "  Image %s bytes (%.1f MB)\n" "$IMG_SIZE" "$(echo "scale=2; $IMG_SIZE/1048576" | bc)"
if [[ -f "${OUT_DIR}/vmlinuz.efi" ]]; then
    Z_SIZE=$(stat -c%s "${OUT_DIR}/vmlinuz.efi")
    printf "  vmlinuz.efi %s bytes (%.1f MB) - UEFI 부팅용\n" \
        "$Z_SIZE" "$(echo "scale=2; $Z_SIZE/1048576" | bc)"
fi
echo ""
echo "  출력: ${OUT_DIR}"
