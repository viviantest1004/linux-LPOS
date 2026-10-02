#!/usr/bin/env bash
#
# mkdisk.sh - build the disk-rooted amd64 image.
#
# The other images this project makes carry their whole system inside
# the kernel: a cpio unpacked into RAM at boot, nothing on disk that the
# running system depends on. That is what makes a board survive having
# its power pulled, and it is the right answer for a Raspberry Pi in a
# cupboard.
#
# It is the wrong answer for a machine somebody sits in front of. This
# script builds the other kind, on a GPT disk because that is what a
# UEFI PC boots, in the layout every LP disk has (COMMON.md, "Disk
# layout and recovery" - the contract with the boot-recovery and disks
# tracks):
#
#   p1  LP-ESP       FAT32  512 MiB  the EFI system partition:
#                                    \EFI\BOOT\BOOTX64.EFI  the boot menu
#                                    (lpboot.efi) or, until there is one,
#                                    the kernel itself;
#                                    \EFI\LP\vmlinuz.efi, cmdline.txt,
#                                    lpboot.efi; the files a person edits
#                                    from another computer
#   p2  LP-RECOVERY  ext4     6 GiB  the recovery system
#                                    (tools/mkrecovery.sh fills it)
#   p3  LP-ROOT      ext4     rest   the root, typed "Linux root (x86-64)"
#
# Every partition gets a fresh random GUID on every build (write-gpt.py),
# and lp-install gives the disk it installs to fresh ones again. That is
# what lets preinit tell this disk's root from another LP disk's when
# both are plugged in - the USB stick an installation was made from is
# usually still in the laptop at the first reboot. \EFI\LP\cmdline.txt
# names the root by that PARTUUID.
#
# The kernel it builds carries a tiny initramfs holding one program,
# preinit, whose only job is to find p3, check it, mount it and
# switch_root into it. Everything after that runs from disk: /etc
# survives, packages install into /usr rather than an overlay, and the
# root can be bigger than the RAM.
#
#   ./tools/mkdisk.sh                 the console image (our userland only)
#   tools/mkdesktop.sh                the desktop image (it calls this)
#
# The result is written with dd, the same as the other images:
#   xz -d < dist/linux-LP_desktop.img.xz | sudo dd of=/dev/sdX bs=4M
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
source "${REPO_ROOT}/tools/common.sh"

# Sizes. The ESP and the recovery partition are fixed by the layout;
# the root is what goes in it plus room, and nothing more - the image is
# built sparse and compressed, but it is still what somebody downloads
# and dd's onto a stick, and an installed system gets a root partition
# the size of its whole disk from lp-install anyway.
#
#   LP_ROOT_MB=6000 ./tools/mkdisk.sh    a root partition of this size
#                                        (mkdesktop.sh sets it)
SECTOR=512
ESP_MB=512
# The recovery partition is sized from the root, once the root's size is
# known (below): it holds the root again as a zstd payload (about a fifth
# of it) and a 30MB recovery system. It used to be a fixed 6GiB, which
# with a 4GiB margin on the root kept LP off a 16GB disk.
RECOVERY_MB="${LP_RECOVERY_MB:-}"
# LP_NO_PAYLOAD=1: the recovery system without its reinstall payload
# (tools/mkrecovery.sh --no-payload) - about 100MB instead of 1.4GB. The
# 8GB stick's image (mkdesktop.sh, LP_EDITION=8g).
NO_PAYLOAD="${LP_NO_PAYLOAD:-0}"
ESP_START=2048                          # 1MiB in, where every tool aligns
ESP_SECTORS=$(( ESP_MB * 1024 * 1024 / SECTOR ))
REC_START=$(( ESP_START + ESP_SECTORS ))

# The filesystem label and the GPT names are the layout contract's. The
# root answered to LPROOT before the recovery partition existed; preinit
# accepts both spellings, so older disks and kernels still meet.
ROOT_LABEL="LP-ROOT"
REC_LABEL="LP-RECOVERY"

# The kernel command line, in one place: it is compiled into the kernel
# and a copy goes onto the FAT partition, and two spellings of it were
# two chances for them to disagree. (The kernel track keeps this line;
# every option on it was checked against the 6.12 source it builds.)
#
#   console=tty0 console=ttyS0   printk goes to both. /dev/console is the
#                                last one, ttyS0: the serial port in a VM.
#                                A PC registers the legacy port at 0x3f8
#                                whether or not a UART answers there, so
#                                on the XPS too /dev/console is ttyS0 and
#                                what is written to it goes nowhere -
#                                anything meant for a person on the
#                                screen has to name tty1 (init does)
#   quiet loglevel=3             the boot is meant to show the firmware's
#                                logo and then the desktop and nothing in
#                                between (kernel/lp-zero-amd64.fragment,
#                                "booting without a flicker"). The kernel
#                                writes to the screen only below level 3
#                                - critical, alert, panic - and so fbcon
#                                never takes the screen over on a good
#                                boot; preinit reads "quiet" too and says
#                                only failures
#   vt.global_cursor_default=0   no blinking text cursor in the corner if
#                                something does bring the console up
#                                mid-boot. A program that wants a person
#                                to type on a VT turns it back on with
#                                ESC [ ? 25 h
#   fbcon=font:TER16x32          the XPS panel is 3840x2160 at 15.6", and
#                                the default 8x16 font there is too small
#                                to read; boot messages, the rescue shell
#                                on tty1 and any text fallback all use it
#   i915.enable_psr=0            no Panel Self Refresh. With PSR the Intel
#                                GPU stops sending frames while the screen
#                                is still and the panel repeats its last
#                                one from its own memory; entering and
#                                leaving that state is a known cause of
#                                flicker and of stale patches around what
#                                just changed on Intel laptop panels. It
#                                saves a fraction of a watt at idle, and a
#                                screen that flickers is not worth it. A
#                                machine without i915 ignores the option
#
# Not here, on purpose: i915.fastboot (6.12 has no such parameter - it
# always reads back the firmware's mode), preempt=full (the kernel is
# built with PREEMPT and boots in full mode already), i915.enable_guc
# (Skylake loads neither GuC nor HuC unless told to; setting it taints
# the kernel, and HuC only serves video encoding - the blobs are in the
# initramfs for whoever wants it anyway, see tools/fetch-pc-fw.sh).
#
# A boot menu that starts the kernel as an EFI application adds to this
# line rather than replacing it: x86 puts the compiled-in line first and
# the loader's options after it (CMDLINE_OVERRIDE is off), and for root=,
# loglevel= and the rest the last one given wins. So the recovery entry
# passes root=PARTLABEL=LP-RECOVERY lp.mode=recovery and gets exactly
# that, and initrd=\EFI\LP\initrd.img there is read by the EFI stub.
KERNEL_CMDLINE="root=LABEL=${ROOT_LABEL} rw console=tty0 console=ttyS0,115200 quiet loglevel=3 vt.global_cursor_default=0 fbcon=font:TER16x32 mem_sleep_default=deep i915.enable_psr=0"

# The same label the RAM-live images use, because /etc/rc mounts /boot by
# label and there is no reason for this image to be the exception - the
# boot partition holds the same authorized_keys, firewall.conf and
# wpa_supplicant.conf on both.
ESP_LABEL="LPZERO"

OUT_DIR="${REPO_ROOT}/sdcard"
IMAGE="${OUT_DIR}/${LP_IMAGE_NAME:-linux-LP_desktop.img}"
# LP_ROOTFS_OVERRIDE points at a root built somewhere else - the merged
# Debian-plus-ours tree that mkdesktop.sh makes. Without it this builds
# the console image from our userland alone, which is the same image and
# a smaller one.
ROOTFS="${LP_ROOTFS_OVERRIDE:-${REPO_ROOT}/userland/rootfs-amd64}"
TINY="${LPZERO_WORK}/initramfs-preinit"
KERNEL_OUT="${REPO_ROOT}/kernel/out-amd64-disk"

log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

for t in mkfs.vfat mkfs.ext4 mcopy dd truncate python3; do
    command -v "$t" >/dev/null 2>&1 || die "$t 가 없습니다"
done

# ── 1. the tiny initramfs ────────────────────────────────────────
#
# One program and one device node. /dev/console has to be in the cpio
# rather than made at run time: the kernel opens it to give init its
# stdin, stdout and stderr, and without it every message preinit prints
# about what went wrong goes nowhere - which is the one situation those
# messages exist for.
step "preinit initramfs"
[[ -x "${REPO_ROOT}/userland/bin-amd64/preinit" ]] \
    || die "bin-amd64/preinit 가 없습니다. 'make ARCH=amd64' 를 먼저."

rm -rf "$TINY"
mkdir -p "$TINY/dev"
cp "${REPO_ROOT}/userland/bin-amd64/preinit" "$TINY/init"
mknod "$TINY/dev/console" c 5 1 2>/dev/null || true
mknod "$TINY/dev/null"    c 1 3 2>/dev/null || true
log "$(du -sh "$TINY" | cut -f1)  ($TINY)"

# ── 2. the kernel that boots from disk ───────────────────────────
#
# Skipped when the bzImage already there was built from the same inputs.
# Rebuilding it to package a different root filesystem is eight minutes
# for a byte-identical result.
step "커널 (디스크 루트용)"
BZ_EXISTING="${KERNEL_OUT}/bzImage"
# The desktop image's kernel is the PC config plus the drivers for every
# other machine, as modules (kernel/lp-desktop-amd64.fragment), with
# i915's firmware from the root's /usr/lib/firmware in its initramfs:
# from Tiger Lake on, i915 needs its GuC blob at probe, before the root
# is mounted. The console image has no module loader and keeps the PC
# config alone.
KCONFIG_EXTRA=""
EARLY_FW_ROOT=""
if [[ "${LP_DESKTOP:-0}" == 1 ]]; then
    KCONFIG_EXTRA="${REPO_ROOT}/kernel/lp-desktop-amd64.fragment"
    [[ -d "${ROOTFS}/usr/lib/firmware/i915" ]] && EARLY_FW_ROOT="${ROOTFS}/usr/lib/firmware"
fi
# 커널 안에 든 프로그램은 preinit 하나다. 그래서 다시 지어야 하는지는
# preinit 의 **내용**이 바뀌었는지로 정한다.
#
# 전에는 시각(-nt)으로 정했고, 그것은 틀린 질문이었다: userland 를
# 한 번 make 하면 libc 를 건드리지 않은 프로그램까지 전부 다시
# 링크되어 preinit 의 시각이 앞선다. 내용은 한 바이트도 다르지
# 않은데 커널을 다시 지으라고 하고, 커널 소스가 없는 기계에서는
# 거기서 빌드가 멈춘다.
#
# The inputs are more than preinit, though. The config was once fixed;
# now the XPS support lives in it, and a stamp that looked only at
# preinit kept shipping a kernel built from the old config after the
# config had changed - and the command line and the firmware pin are
# compiled in too, and the firmware sits in the initramfs beside
# preinit. So the stamp covers all of them, and the kernel source commit
# and the scripts that merge the config and build it, and the patches
# build.sh applies to that source. (The file keeps its old name so an
# existing stamp is simply a mismatch, not an error.)
PREINIT="${REPO_ROOT}/userland/bin-amd64/preinit"
STAMP="${KERNEL_OUT}/preinit.sha256"
NOW_SUM=""
if [[ -f "$PREINIT" ]]; then
    NOW_SUM="$( { cat "$PREINIT" \
                    "${REPO_ROOT}/kernel/lp-zero.config" \
                    "${REPO_ROOT}/kernel/lp-zero-amd64.fragment" \
                    "${REPO_ROOT}/tools/mkamd64config.sh" \
                    "${REPO_ROOT}/kernel/linux.commit" \
                    "${REPO_ROOT}/kernel/build.sh" \
                    "${REPO_ROOT}/tools/fetch-pc-fw.sh" \
                    "${REPO_ROOT}"/kernel/patches/*.patch \
                    ${KCONFIG_EXTRA:+"$KCONFIG_EXTRA"}
                  [[ -n "$EARLY_FW_ROOT" ]] &&
                      ( cd "$EARLY_FW_ROOT" && find -L i915 -type f | sort | xargs sha256sum )
                  printf '%s' "$KERNEL_CMDLINE"; } | sha256sum | cut -d' ' -f1)"
fi
OLD_SUM="$(cat "$STAMP" 2>/dev/null || true)"

# A desktop kernel is only whole with its modules beside it.
HAVE_MODULES=1
[[ -n "$KCONFIG_EXTRA" && ! -d "${KERNEL_OUT}/modules/lib/modules" ]] && HAVE_MODULES=0
if [[ -f "$BZ_EXISTING" && -n "$NOW_SUM" && "$NOW_SUM" == "$OLD_SUM" && "$HAVE_MODULES" == 1 ]]; then
    log "이미 있는 것을 씁니다 ($(stat -c%s "$BZ_EXISTING") bytes)"
    log "다시 빌드하려면 지우십시오: rm ${BZ_EXISTING}"
elif [[ -f "$BZ_EXISTING" && -z "$OLD_SUM" && \
        ! "$PREINIT" -nt "$BZ_EXISTING" ]]; then
    # 도장이 아직 없는 예전 빌드. 시각으로 판단하고 도장을 남긴다.
    log "이미 있는 것을 씁니다 ($(stat -c%s "$BZ_EXISTING") bytes)"
    printf '%s\n' "$NOW_SUM" > "$STAMP"
else
LP_ARCH=amd64 \
LP_ROOTFS_DIR="$(python3 -c "import os,sys;print(os.path.relpath(sys.argv[1], os.path.join(sys.argv[2],'userland')))" "$TINY" "$REPO_ROOT")" \
LP_BUILD_DIR="${LPZERO_WORK}/build-amd64-disk" \
LP_OUT_SUBDIR=out-amd64-disk \
LP_CMDLINE="$KERNEL_CMDLINE" \
LP_KCONFIG_EXTRA="$KCONFIG_EXTRA" \
LP_EARLY_FW_ROOT="$EARLY_FW_ROOT" LP_EARLY_FW_DIRS=i915 \
    "${REPO_ROOT}/kernel/build.sh"
fi

BZIMAGE="${KERNEL_OUT}/bzImage"
[[ -f "$BZIMAGE" ]] || die "${BZIMAGE} 가 만들어지지 않았습니다"
[[ -n "$NOW_SUM" ]] && printf '%s\n' "$NOW_SUM" > "${KERNEL_OUT}/preinit.sha256"
log "$(stat -c%s "$BZIMAGE") bytes"

# The modules go into the root, whole (kernel/build.sh made the tree with
# depmod's indexes); udev loads what the hardware asks for.
if [[ -n "$KCONFIG_EXTRA" ]]; then
    MODTREE="${KERNEL_OUT}/modules/lib/modules"
    [[ -d "$MODTREE" ]] || die "no modules in ${MODTREE} (kernel/build.sh)"
    rm -rf "${ROOTFS}/usr/lib/modules"
    mkdir -p "${ROOTFS}/usr/lib/modules"
    cp -a "${MODTREE}/." "${ROOTFS}/usr/lib/modules/"
    log "modules: $(find "${ROOTFS}/usr/lib/modules" -name '*.ko*' | wc -l), $(du -sh "${ROOTFS}/usr/lib/modules" | cut -f1)"
fi

# ── 3. the root filesystem ───────────────────────────────────────
#
# Built from the same rootfs directory the RAM-live image uses, so there
# is one userland and not two - or from the merged desktop root that
# mkdesktop.sh assembles (LP_ROOTFS_OVERRIDE).
step "root filesystem (ext4, ${ROOT_LABEL})"
[[ -d "$ROOTFS" ]] || die "${ROOTFS} is missing - userland/mkrootfs.sh first"

# 들어갈 것이 자리보다 크면 여기서 멈춘다.
#
# mke2fs 는 이 경우 "Could not allocate block in ext2 filesystem while
# writing file <아무 파일 이름>" 이라고만 말한다. 그 이름은 마침 마지막
# 으로 쓰려던 파일일 뿐이라, 읽는 사람은 그 파일이 잘못된 줄 알고 한참
# 을 엉뚱한 데서 찾는다. 무엇이 부족한지는 여기서 이미 알 수 있다.
# Not -x: the desktop's root is an overlay whose files report their
# layer's device, and -x then counted the upper layer alone - a root, and
# a recovery partition sized from it, a fraction of what goes in.
NEED_KB=$(du -sk "$ROOTFS" --exclude=proc --exclude=sys --exclude=dev --exclude=tmp 2>/dev/null | cut -f1)
ROOT_MB="${LP_ROOT_MB:-$(( NEED_KB * 5 / 4 / 1024 + 256 ))}"
ROOT_SECTORS=$(( ROOT_MB * 1024 * 1024 / SECTOR ))
# ext4 자체의 메타데이터에 5% 쯤. 여유가 없으면 마지막에 가서 터진다.
if (( NEED_KB * 105 / 100 > ROOT_MB * 1024 )); then
    die "the root does not fit: $(( NEED_KB / 1024 ))MiB into ${ROOT_MB}MiB.
       LP_ROOT_MB=$(( NEED_KB * 5 / 4 / 1024 + 256 )) ./tools/mkdisk.sh"
fi
# The backup GPT sits in the last 33 sectors; a MiB of tail keeps the
# root aligned and clear of it.
if [[ -z "$RECOVERY_MB" && "$NO_PAYLOAD" == 1 ]]; then
    RECOVERY_MB=256
elif [[ -z "$RECOVERY_MB" ]]; then
    RECOVERY_MB=$(( NEED_KB * 3 / 10 / 1024 + 384 ))
    RECOVERY_MB=$(( (RECOVERY_MB + 63) / 64 * 64 ))
fi
REC_SECTORS=$(( RECOVERY_MB * 1024 * 1024 / SECTOR ))
ROOT_START=$(( REC_START + REC_SECTORS ))
TOTAL_SECTORS=$(( ROOT_START + ROOT_SECTORS + 2048 ))

mkdir -p "$OUT_DIR"
rm -f "$IMAGE"
truncate -s $(( TOTAL_SECTORS * SECTOR )) "$IMAGE"

# Built straight into the image at the partition's offset, with no
# intermediate file.
#
# The obvious way is to mkfs a separate file and dd it in, and it costs
# twice the disk and twice the time: mke2fs takes -E offset= precisely so
# that this is unnecessary. On a machine without room to spare it is the
# difference between a build that finishes and one that fills the disk
# at 90% and leaves a corrupt image behind.
#
# -d takes the directory straight in, which keeps ownership, modes, hard
# links and extended attributes (file capabilities: ping's right to open
# a raw socket lives there) without a loop mount - and a loop mount needs
# privileges a build should not assume it has.
mkfs.ext4 -q -F -L "$ROOT_LABEL" -m 1 \
    -E offset=$(( ROOT_START * SECTOR )) \
    -d "$ROOTFS" \
    "$IMAGE" $(( ROOT_SECTORS * SECTOR / 1024 ))k
log "${ROOT_MB}MiB, $(( NEED_KB / 1024 ))MiB in it"

# ── 4. the recovery partition ────────────────────────────────────
#
# tools/mkrecovery.sh (boot-recovery track) builds its tree from the
# root that was just packed: a small recovery system of our userland and
# the disk tools, and /reinstall with this very system as a payload.
# Until that script exists the partition is made empty, with a note, so
# the layout - and everything that finds partitions by it - is already
# the final one.
step "recovery partition (ext4, ${REC_LABEL})"
# LP_REC_TREE puts it elsewhere - a build host whose work disk has no
# room for a second copy of the root (the payload) next to the first.
REC_TREE="${LP_REC_TREE:-${LPZERO_WORK}/recovery-tree}"
rm -rf "$REC_TREE"
mkdir -p "$REC_TREE"
MKREC="${REPO_ROOT}/tools/mkrecovery.sh"
REC_HAS_SYSTEM=0
MKREC_ARGS=()
[[ "$NO_PAYLOAD" == 1 ]] && MKREC_ARGS+=(--no-payload)
if [[ -x "$MKREC" && "${LP_NO_RECOVERY:-0}" != 1 ]]; then
    "$MKREC" --root "$ROOTFS" --out "$REC_TREE" --kernel "${KERNEL_OUT}/bzImage" \
        --cmdline "$KERNEL_CMDLINE" "${MKREC_ARGS[@]}" || die "tools/mkrecovery.sh failed"
    REC_HAS_SYSTEM=1
    log "tools/mkrecovery.sh: $(du -sh "$REC_TREE" | cut -f1)"
else
    printf 'LP recovery partition.\n\nThis image was built before tools/mkrecovery.sh existed, so the\nrecovery system is not here yet. The partition is in its place so the\ndisk layout is already the final one.\n' \
        > "$REC_TREE/README.txt"
    log "no tools/mkrecovery.sh yet - empty, with a README"
fi
mkfs.ext4 -q -F -L "$REC_LABEL" -m 0 \
    -E offset=$(( REC_START * SECTOR )) \
    -d "$REC_TREE" \
    "$IMAGE" $(( REC_SECTORS * SECTOR / 1024 ))k
rm -rf "$REC_TREE"

# ── 5. the partition table ───────────────────────────────────────
#
# GPT, not MBR: the partition types say what each one is, and each
# partition gets a GUID of its own that preinit, the boot menu and the
# installed system's boot entry can name. Written before the ESP is
# filled, because the ESP's cmdline.txt names the root by its PARTUUID.
# The recovery partition is typed "Linux filesystem", not "Linux root":
# preinit takes the partition typed root on the boot disk as the root.
step "GPT"
PARTUUIDS="$(python3 "${REPO_ROOT}/tools/write-gpt.py" "$IMAGE" \
    "${ESP_START}:${ESP_SECTORS}:esp:LP-ESP" \
    "${REC_START}:${REC_SECTORS}:linux:${REC_LABEL}" \
    "${ROOT_START}:${ROOT_SECTORS}:root:${ROOT_LABEL}")"
while read -r n u; do log "p${n} PARTUUID=${u}"; done <<< "$PARTUUIDS"
ROOT_PARTUUID="$(awk '$1 == 3 { print $2 }' <<< "$PARTUUIDS")"
REC_PARTUUID="$(awk '$1 == 2 { print $2 }' <<< "$PARTUUIDS")"

# ── 6. the EFI system partition ──────────────────────────────────
step "EFI system partition (FAT32, ${ESP_LABEL})"
ESP_IMG="${OUT_DIR}/.esp.img"
rm -f "$ESP_IMG"
truncate -s $(( ESP_SECTORS * SECTOR )) "$ESP_IMG"
mkfs.vfat -F 32 -n "$ESP_LABEL" "$ESP_IMG" >/dev/null
esp_put() { mcopy -o -i "$ESP_IMG" "$1" "::$2"; }
esp_text() {  # esp_text <path on the ESP>  (stdin, CRLF for other OSes)
    local t="${OUT_DIR}/.esp-text"
    sed 's/$/\r/' > "$t"
    esp_put "$t" "$1"
    rm -f "$t"
}

mmd -i "$ESP_IMG" ::EFI ::EFI/BOOT ::EFI/LP
esp_put "$BZIMAGE" EFI/LP/vmlinuz.efi

# The boot menu (boot-recovery track, boot/efi/): "LP" and "LP
# Recovery", a timeout, the bootcount fallback. It reads
# \EFI\LP\cmdline.txt for a normal boot. Until it is built, the kernel
# is its own loader at the fallback path and boots with its built-in
# command line - which finds this disk's root through the firmware's
# BootCurrent (preinit, rule 3).
LPBOOT="${LP_BOOT_EFI:-${REPO_ROOT}/boot/efi/lpboot.efi}"
if [[ -f "$LPBOOT" ]]; then
    esp_put "$LPBOOT" EFI/LP/lpboot.efi
    esp_put "$LPBOOT" EFI/BOOT/BOOTX64.EFI
    log "EFI/BOOT/BOOTX64.EFI = lpboot.efi (the boot menu)"
else
    esp_put "$BZIMAGE" EFI/BOOT/BOOTX64.EFI
    log "EFI/BOOT/BOOTX64.EFI = the kernel (no boot/efi/lpboot.efi yet)"
fi

# The normal boot's command line: the compiled-in one with its root=
# replaced by this disk's PARTUUID. The menu passes it as the kernel's
# load options; x86 appends them to the built-in line and the last
# root= wins. lp-install writes the installed disk's own.
CMDLINE_FILE="root=PARTUUID=${ROOT_PARTUUID} ${KERNEL_CMDLINE#root=* }"
printf '%s\n' "$CMDLINE_FILE" | esp_text EFI/LP/cmdline.txt
# A copy at the top, for a person with a card reader; the same line.
printf '%s\n' "$CMDLINE_FILE" | esp_text cmdline.txt
log "EFI/LP/cmdline.txt: root=PARTUUID=${ROOT_PARTUUID}"
# The boot menu offers LP Recovery - and falls back to it after two
# failed starts - only when this says p2 holds a recovery system, and it
# names p2 by PARTUUID (every LP disk has an LP-RECOVERY).
if [[ "$REC_HAS_SYSTEM" = 1 ]]; then
    printf 'root=PARTUUID=%s\n' "$REC_PARTUUID" | esp_text EFI/LP/recovery.ok
    log "EFI/LP/recovery.ok: root=PARTUUID=${REC_PARTUUID}"
fi
# The installer image says so on its EFI partition too: its boot menu
# then hands over to an LP already installed on another disk of the
# machine instead of offering the installer again (lpboot.c,
# chain_installed). lp-install never copies this file to a disk it
# installs, and removes it when it installs LP where it runs.
if [[ -e "$ROOTFS/etc/lp/installer-medium" ]]; then
    printf 'This partition is an LP installer.\n' | esp_text EFI/LP/installer
    log "EFI/LP/installer (the installer medium)"
fi

# startup.nsh, and it is not belt and braces.
#
# EFI/BOOT/BOOTX64.EFI is the removable-media path and every firmware is
# supposed to try it. Not all of them do on the first boot of a blank
# NVRAM: OVMF with fresh variables walks its own boot list, finds
# nothing it put there itself, and falls through to the EFI shell -
# which leaves a machine sitting at a Shell> prompt that most people
# will read as "it does not boot". The shell runs startup.nsh without
# being asked, so the one firmware path that looks like a dead end
# becomes the one that boots.
printf 'fs0:\nEFI\\BOOT\\BOOTX64.EFI\n' | esp_text startup.nsh

# README.txt: what a person looking at this partition from Windows or a
# Mac needs to know, and the two firmware settings that stop the XPS
# from booting or installing this at all. Neither can be fixed from the
# stick, and one cannot even be detected before the kernel is running:
# with Secure Boot on the firmware refuses the unsigned kernel before it
# starts, and with the SATA controller in RAID mode the NVMe disk is
# invisible to Linux. lp-install says the same on screen when it
# detects either.
esp_text README.txt <<'README'
linux-LP - EFI system partition
===============================

EFI/BOOT/BOOTX64.EFI   what the firmware starts: the LP boot menu, or the
                       kernel itself on a build without the menu
EFI/LP/vmlinuz.efi     the kernel (it is its own UEFI boot loader)
EFI/LP/cmdline.txt     the kernel command line for a normal boot
EFI/LP/lpboot.efi      the boot menu: LP, and LP Recovery
startup.nsh            makes the UEFI shell boot too

Before booting this stick on a Dell XPS 15 9550 (F2 opens the BIOS setup):

 1. Secure Boot -> Secure Boot Enable: OFF
    The kernel is not signed by Microsoft; with Secure Boot on, the
    firmware refuses to start it and nothing on this stick can say why.

 2. System Configuration -> SATA Operation: AHCI
    Dell ships these machines set to "RAID On". In that mode the NVMe
    disk sits behind Intel RST and Linux cannot see it, so there is
    nowhere to install to. If Windows is still on the disk, switch it to
    safe mode once before changing this, or it will not boot afterwards.

Then F12 at power-on -> the USB stick. The installer starts by itself.

-----------------------------------------------------------------------

부팅 전에 (XPS 15 9550, 전원을 켤 때 F2 로 BIOS 설정):

 1. Secure Boot -> Secure Boot Enable: 끔
    커널이 Microsoft 서명을 받지 않았습니다. 켜 두면 펌웨어가 커널을
    거부하고, 이 USB 안의 어떤 것도 그 이유를 화면에 보여 줄 수 없습니다.

 2. System Configuration -> SATA Operation: AHCI 로 바꾸세요
    "RAID On" 으로 출고됩니다. 그 상태에서는 NVMe 디스크가 Intel RST 뒤에
    숨어 리눅스가 볼 수 없고, 설치할 곳이 없습니다. 디스크에 Windows 가
    남아 있다면 바꾸기 전에 한 번 안전 모드로 부팅해 두어야 합니다.

그 다음 전원을 켤 때 F12 -> USB. 설치 프로그램이 저절로 시작됩니다.
README
log "README.txt (Secure Boot, AHCI)"

for f in authorized_keys wpa_supplicant.conf firewall.conf beacon.conf; do
    src="${REPO_ROOT}/boot/rootfs-overlay/etc/${f}"
    [[ -f "$src" ]] && esp_put "$src" "${f}" && log "$f"
done

dd if="$ESP_IMG" of="$IMAGE" bs=1M seek=$(( ESP_START / 2048 )) conv=notrunc,sparse status=none
rm -f "$ESP_IMG"

step "result"
# LP_MAX_BYTES: the stick it has to fit on. An "8GB" stick holds between
# 7.2 and 8 billion bytes, and an image even a sector too big is cut off
# at the end by dd and refused outright by Etcher - which leaves the
# stick as it was, and a PC then offers it only as a Legacy disk.
if [[ -n "${LP_MAX_BYTES:-}" ]] && (( $(stat -c%s "$IMAGE") > LP_MAX_BYTES )); then
    die "$(stat -c%s "$IMAGE") bytes is more than LP_MAX_BYTES=${LP_MAX_BYTES}"
fi
log "$(( $(stat -c%s "$IMAGE") / 1024 / 1024 ))MiB (sparse; $(du -m "$IMAGE" | cut -f1)MiB on disk)  ${IMAGE}"
log ""
log "  GPT"
log "  p1  LP-ESP       ${ESP_MB}MiB  FAT32 ${ESP_LABEL}  boot menu, kernel, command line"
log "  p2  LP-RECOVERY  ${RECOVERY_MB}MiB  ext4  the recovery system"
log "  p3  LP-ROOT      ${ROOT_MB}MiB  ext4  the root (Linux root x86-64)"
log ""
log "write:  sudo dd if=${IMAGE} of=/dev/sdX bs=4M conv=fsync status=progress"
log "QEMU:   qemu-system-x86_64 -machine q35 -m 4096 -smp 4 \\"
log "          -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \\"
log "          -drive if=pflash,format=raw,file=vars.fd \\"
log "          -device qemu-xhci -drive if=none,id=stick,file=${IMAGE},format=raw \\"
log "          -device usb-storage,drive=stick"
