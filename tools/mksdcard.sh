#!/usr/bin/env bash
#
# mksdcard.sh - 부팅 가능한 SD카드 이미지를 만든다.
#
# root 권한도 loop 마운트도 쓰지 않는다. mtools 가 FAT 파일시스템을
# 유저스페이스에서 직접 조작하고, MBR 은 파이썬으로 바이트를 찍는다.
# 덕분에 컨테이너/CI 안에서도 그대로 돌아간다.
#
# 두 가지 모드가 있다:
#   (기본)   베어메탈 펌웨어를 부팅하는 이미지
#   --linux  우리가 빌드한 리눅스 커널을 부팅하는 이미지
#
# 결과: sdcard/lp-zero.img  (dd 로 SD카드에 그대로 쓰면 부팅됨)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"
BLOB_DIR="${REPO_ROOT}/blobs"
OUT_DIR="${REPO_ROOT}/sdcard"
IMAGE="${OUT_DIR}/lp-zero.img"
MODE=firmware
WITH_MPY=true          # MicroPython 을 데이터 파티션에 넣을지
# --uefi-only: 커널을 EFI/BOOT/BOOTAA64.EFI 로만 넣고, 라즈베리파이 GPU
# 펌웨어가 읽는 이름으로는 넣지 않는다. 커널이 24MB 라 두 벌이면 이미지가
# 그만큼 커지는데, 가상머신에서만 쓸 이미지에는 한 벌이면 충분하다.
# 이 이미지는 실기에서 부팅하지 않는다.
UEFI_ONLY=false
for arg in "$@"; do
    case "$arg" in
        --linux)           MODE=linux ;;
        --no-micropython)  WITH_MPY=false ;;
        --uefi-only)       UEFI_ONLY=true ;;
        *) printf 'error: 알 수 없는 인자: %s\n' "$arg" >&2
           printf '사용법: mksdcard.sh [--linux] [--no-micropython] [--uefi-only]\n' >&2
           exit 2 ;;
    esac
done
PART_IMG="${OUT_DIR}/.boot-part.img"

# 이미지 레이아웃
#
#   섹터 0                 MBR
#   섹터 8192   (4MiB)     파티션 1: FAT32 부트 (GPU 블롭, 커널, config.txt)
#   섹터 270336 (132MiB)   파티션 2: ext4 데이터 (WiFi 설정, SSH 키, 파일)
#
# 루트는 커널에 내장된 initramfs(RAM)이고 데이터만 여기에 남는다.
# 루트에 쓰기가 없으므로 전원을 갑자기 뽑아도 시스템이 깨지지 않는다.
#
# 부트 파티션이 128MiB 인 이유: 이 시스템 전체 - 커널과 유저랜드 - 가
# 파일 하나(22MB)다. 업데이트를 원자적으로 하려면 그 파일이 두 개 있어야
# 한다. 새 것을 다 쓰고 검증한 다음에야 이름을 바꾸고, 실패하면 예전
# 것으로 돌아간다(update). 64MiB 로는 두 개가 안 들어간다.
# 256MB is the size a Pi Zero 2 W image wants to be: it has to fit any
# card somebody has lying around, and expandfs grows /data into whatever
# the card really is on the first boot.
#
# A virtual machine has no such constraint - the disk is a file - and
# 124MB of /data is not much once CPython (29MB) and its glibc are on
# it. LP_IMAGE_MB overrides it; the arm64 VM image is built at 16GB.
# The file stays sparse, so a 16GB image is no bigger to download.
IMAGE_MB="${LP_IMAGE_MB:-256}"
SECTOR_SIZE=512
BOOT_START_SECTOR=8192          # 4MiB
BOOT_SIZE_MB=128
DATA_START_SECTOR=270336        # 4 + 128 = 132MiB

# LP_ARCH picks which build goes on the card.
#
#   arm64  the Pi Zero 2 W and arm64 VMs: an uncompressed Image the GPU
#          firmware can load, plus EFI/BOOT/BOOTAA64.EFI for UEFI.
#   amd64  a PC or a desktop VM: one bzImage, which carries the EFI stub
#          itself, as EFI/BOOT/BOOTX64.EFI. No GPU firmware, no device
#          tree - a PC describes itself through ACPI.
#   armv6  a Pi Zero W: a zImage the GPU firmware loads, and nothing
#          else. There is no UEFI on that board, and a 32-bit ARM
#          zImage is not a PE executable, so there is no second copy.
LP_ARCH="${LP_ARCH:-arm64}"
if [[ "$LP_ARCH" == "amd64" ]]; then
    EFI_BOOT_NAME="BOOTX64.EFI"
    KERNEL_OUT_DIR="${REPO_ROOT}/kernel/out-amd64"
elif [[ "$LP_ARCH" == "armv6" ]]; then
    EFI_BOOT_NAME=""
    KERNEL_OUT_DIR="${REPO_ROOT}/kernel/out-armv6"
else
    EFI_BOOT_NAME="BOOTAA64.EFI"
    KERNEL_OUT_DIR="${REPO_ROOT}/kernel/out"
fi
VOLUME_LABEL="LPZERO"
DATA_LABEL="LPZERODATA"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
log() { printf '  %s\n' "$*"; }

for t in mkfs.vfat mcopy python3 dd; do
    command -v "$t" >/dev/null 2>&1 || die "$t 가 필요합니다 (apt install mtools dosfstools)"
done

# ── 입력 확인 ────────────────────────────────────────────────────
if [[ "$MODE" == "linux" && "$LP_ARCH" == "amd64" ]]; then
    # A PC boots one way: UEFI finds EFI/BOOT/BOOTX64.EFI. There is no
    # second copy for a GPU to load, no config.txt and no device tree.
    KERNEL="${KERNEL_OUT_DIR}/bzImage"
    KERNEL_NAME=""
    CONFIG_SRC=""
    DTB=""
    [[ -f "$KERNEL" ]] || die "kernel/out-amd64/bzImage 가 없습니다. 'make kernel-amd64' 를 먼저 실행하세요."
elif [[ "$MODE" == "linux" && "$LP_ARCH" == "armv6" ]]; then
    KERNEL="${KERNEL_OUT_DIR}/zImage"
    KERNEL_NAME="$LINUX_IMAGE_ARMV6"
    CONFIG_SRC="${REPO_ROOT}/boot/config-armv6.txt"
    DTB="${KERNEL_OUT_DIR}/bcm2708-rpi-zero-w.dtb"
    [[ -f "$KERNEL" ]] || die "kernel/out-armv6/zImage 가 없습니다. 'LP_ARCH=armv6 ./kernel/build.sh' 를 먼저 실행하세요."
    [[ -f "$DTB" ]]    || die "kernel/out-armv6 에 Zero W DTB 가 없습니다."
elif [[ "$MODE" == "linux" ]]; then
    KERNEL="${KERNEL_OUT_DIR}/Image"
    KERNEL_NAME="$LINUX_IMAGE"
    CONFIG_SRC="${REPO_ROOT}/boot/config-linux.txt"
    DTB="${KERNEL_OUT_DIR}/bcm2710-rpi-zero-2-w.dtb"
    [[ -f "$KERNEL" ]] || die "kernel/out/Image 가 없습니다. 'make kernel' 을 먼저 실행하세요."
    [[ -f "$DTB" ]]    || die "kernel/out 에 Zero 2 W DTB 가 없습니다."
else
    KERNEL="${REPO_ROOT}/firmware/${KERNEL_IMAGE}"
    KERNEL_NAME="$KERNEL_IMAGE"
    CONFIG_SRC="${REPO_ROOT}/boot/config.txt"
    DTB=""
    [[ -f "$KERNEL" ]] || die "firmware/${KERNEL_IMAGE} 가 없습니다. 먼저 'make firmware' 를 실행하세요."
fi

# config.txt 의 kernel= 과 실제 파일명이 다르면 GPU 가 커널을 못 찾는다.
# 그 경우 화면도 시리얼도 아무 것도 안 나와서 원인 찾기가 매우 어렵다.
# 이미지를 굽기 전에 여기서 잡는다.
if [[ -n "$CONFIG_SRC" ]]; then
CFG_KERNEL="$(sed -n 's/^[[:space:]]*kernel=\(.*\)$/\1/p' "$CONFIG_SRC" \
              | tail -1 | tr -d '"'"'[:space:]'"'"')"
else
CFG_KERNEL="$KERNEL_NAME"
fi
if [[ "$CFG_KERNEL" != "$KERNEL_NAME" ]]; then
    die "이름 불일치: config.mk 는 '${KERNEL_NAME}', $(basename "$CONFIG_SRC") 는 '${CFG_KERNEL}'.
       둘을 같게 맞추세요. 다르면 부팅 시 아무 출력 없이 멈춥니다."
fi

# Broadcom GPU firmware, and only a Pi has one.
if [[ "$LP_ARCH" == "amd64" ]]; then
    BLOBS=()
else
    BLOBS=(bootcode.bin start.elf fixup.dat)
    for b in "${BLOBS[@]}"; do
        [[ -f "${BLOB_DIR}/${b}" ]] || die "blobs/${b} 가 없습니다. './tools/fetch-blobs.sh' 를 먼저 실행하세요."
    done
fi

mkdir -p "$OUT_DIR"
rm -f "$IMAGE" "$PART_IMG"

# ── 1. FAT32 부트 파티션 만들기 ──────────────────────────────────
PART_SECTORS=$(( BOOT_SIZE_MB * 1024 * 1024 / SECTOR_SIZE ))
PART_BYTES=$(( PART_SECTORS * SECTOR_SIZE ))

echo "SD카드 이미지 생성 중 (${IMAGE_MB}MiB, 모드: ${MODE})"
log "부트 파티션: ${PART_SECTORS} 섹터 ($(( PART_BYTES / 1024 / 1024 ))MiB, FAT32)"

truncate -s "$PART_BYTES" "$PART_IMG"
mkfs.vfat -F 32 -n "$VOLUME_LABEL" "$PART_IMG" >/dev/null

# ── 2. 파일 복사 ────────────────────────────────────────────────
# mtools 는 이미지 파일을 드라이브처럼 다룬다. 설정 파일 검사는 건너뛴다.
export MTOOLS_SKIP_CHECK=1

if (( ${#BLOBS[@]} )); then
    log "복사: bootcode.bin, start.elf, fixup.dat  (Broadcom GPU 펌웨어)"
    for b in "${BLOBS[@]}"; do
        mcopy -i "$PART_IMG" "${BLOB_DIR}/${b}" ::
    done
fi

if [[ -n "$CONFIG_SRC" ]]; then
    log "복사: config.txt                          (GPU 부팅 설정)"
    mcopy -i "$PART_IMG" "$CONFIG_SRC" ::config.txt
fi

if [[ -z "$KERNEL_NAME" ]]; then
    :   # amd64: the only copy is the EFI one, written below
elif $UEFI_ONLY; then
    log "건너뜀: ${KERNEL_NAME}                (--uefi-only: 가상머신 전용)"
else
    log "복사: ${KERNEL_NAME}"
    mcopy -i "$PART_IMG" "$KERNEL" "::${KERNEL_NAME}"
fi

if [[ "$MODE" == "linux" && -n "$DTB" ]]; then
    log "복사: $(basename "$DTB")            (디바이스 트리)"
    mcopy -i "$PART_IMG" "$DTB" ::

    log "복사: cmdline.txt                        (커널 커맨드라인)"
    mcopy -i "$PART_IMG" "${REPO_ROOT}/boot/cmdline.txt" ::

    # disable-bt 오버레이가 있어야 PL011 이 헤더 핀으로 나온다.
    # 없으면 부팅은 되는데 시리얼에 아무것도 안 보인다.
    OVL_DIR="${KERNEL_OUT_DIR}/overlays"
    if [[ -d "$OVL_DIR" ]] && compgen -G "${OVL_DIR}/*.dtbo" >/dev/null; then
        mmd -i "$PART_IMG" ::overlays 2>/dev/null || true
        for o in "${OVL_DIR}"/*.dtbo; do
            mcopy -i "$PART_IMG" "$o" ::overlays/
            log "복사: overlays/$(basename "$o")"
        done
    else
        echo ""
        echo "  경고: disable-bt.dtbo 가 없습니다."
        echo "        이 오버레이 없이는 PL011(ttyAMA0)이 블루투스에 물려 있어"
        echo "        40핀 헤더의 시리얼 콘솔에 아무것도 나오지 않습니다."
        echo ""
    fi
fi

if [[ "$MODE" == "linux" ]]; then
    # 부트 파티션은 FAT32 라 윈도우·맥에서도 그냥 보인다. 기기에 리눅스
    # 없이 접근할 수 있는 유일한 통로이므로, 첫 설정에 필요한 파일을
    # 여기에 둔다. /etc/rc 가 부팅할 때 읽어간다.
    log "복사: authorized_keys                     (SSH 공개키 - 여기에 붙이세요)"
    mcopy -i "$PART_IMG" "${REPO_ROOT}/boot/rootfs-overlay/etc/authorized_keys" ::

    log "복사: wpa_supplicant.conf                 (무선 설정)"
    mcopy -i "$PART_IMG" "${REPO_ROOT}/boot/rootfs-overlay/etc/wpa_supplicant.conf" ::

    log "복사: firewall.conf                       (방화벽 정책)"
    mcopy -i "$PART_IMG" "${REPO_ROOT}/boot/rootfs-overlay/etc/firewall.conf" ::

    log "복사: beacon.conf                         (상태 보고 주소)"
    mcopy -i "$PART_IMG" "${REPO_ROOT}/boot/rootfs-overlay/etc/beacon.conf" ::

    # ── UEFI 로도 부팅되게 ──────────────────────────────────────
    # 같은 카드 하나로 셋 다 부팅시키기 위한 장치다.
    #
    #   실기 Pi     GPU 펌웨어(start.elf)가 config.txt 의 kernel= 을 읽는다
    #   QEMU/UTM    PCI 도 GPU 펌웨어도 없다. 대신 UEFI 펌웨어가
    #               FAT 파티션에서 EFI/BOOT/BOOTAA64.EFI 를 찾아 실행한다
    #
    # arm64 커널은 CONFIG_EFI_STUB=y 로 지으면 그 자체가 PE 실행 파일이다
    # (오프셋 0x40 에 PE 헤더가 있다). 그래서 같은 Image 를 이름만 바꿔
    # 넣으면 UEFI 가 바로 부팅한다. 별도의 부트로더가 필요 없다.
    #
    # 커맨드라인은 커널에 박힌 CONFIG_CMDLINE 을 쓴다. UEFI 로 올 때는
    # 부트로더가 bootargs 를 주지 않기 때문이다.
    #
    # UEFI 경로에는 vmlinuz.efi(EFI_ZBOOT 산물)를 쓴다. 같은 커널을
    # 압축해 EFI 스텁으로 감싼 것이라 크기가 절반 이하다. 압축을 푸는
    # 주체가 EFI 부트 서비스이므로 실기 Pi 에서는 쓸 수 없고, 그쪽은
    # config.txt 의 kernel= 이 가리키는 압축되지 않은 Image 를 그대로
    # 쓴다. 두 파일이 같은 커널이라는 점이 중요하다.
    if [[ "$LP_ARCH" == "armv6" ]]; then
        # A 32-bit ARM zImage is not a PE executable and this board has
        # no UEFI to load one. The GPU firmware copy is the only copy.
        EFI_KERNEL=""
        EFI_LABEL=""
    elif [[ "$LP_ARCH" == "amd64" ]]; then
        # bzImage already carries the EFI stub and its own decompressor,
        # so there is nothing to pair it with.
        EFI_KERNEL="$KERNEL"
        EFI_LABEL="bzImage"
    else
        EFI_KERNEL="${KERNEL_OUT_DIR}/vmlinuz.efi"
        EFI_LABEL="vmlinuz.efi - 압축"
        if [[ ! -f "$EFI_KERNEL" ]]; then
            EFI_KERNEL="$KERNEL"
            EFI_LABEL="Image - 비압축"
        fi
    fi
    if [[ -z "$EFI_KERNEL" ]]; then
        :   # armv6: no UEFI path on this board
    elif python3 -c "
import struct, sys
d = open('$EFI_KERNEL','rb').read(0x100)
sys.exit(0 if d[:2] == b'MZ' and d[struct.unpack_from('<I', d, 0x3c)[0]:][:4] == b'PE\\0\\0' else 1)
" 2>/dev/null; then
        mmd -i "$PART_IMG" ::EFI 2>/dev/null || true
        mmd -i "$PART_IMG" ::EFI/BOOT 2>/dev/null || true
        mcopy -o -i "$PART_IMG" "$EFI_KERNEL" "::EFI/BOOT/${EFI_BOOT_NAME}"
        log "복사: EFI/BOOT/${EFI_BOOT_NAME}            (UEFI 부팅용, ${EFI_LABEL})"
    else
        echo "  경고: 커널에 EFI 스텁이 없습니다 (CONFIG_EFI_STUB)."
        echo "        QEMU/UTM 에서 UEFI 로 부팅되지 않습니다."
    fi

    README="${OUT_DIR}/.README.txt"
    cat > "$README" <<'READMEEOF'
LP-zero OS

This partition is FAT32, so Windows and macOS can both see it.
Edit the two files below and the next boot picks them up. You do not
have to burn the card again.

  authorized_keys       The SSH public keys allowed to log in.

                        You do not have to fill this in. On its first
                        boot, a card with no key in it makes one for
                        itself and leaves the private half here as

                            lp_ssh_key

                        along with the ssh command to use it, printed on
                        the console. That key is generated on the board,
                        is different on every machine, and has never
                        existed anywhere else - so copy it off, keep it,
                        and it is yours:

                            ssh -i lp_ssh_key root@<the address it got>

                        (Copy it somewhere your PC keeps private and
                        chmod 600 it; ssh refuses a key file the whole
                        machine can read.)

                        To use your own key instead, put its public half
                        here before the first boot. On the PC you will
                        connect from:
                            ssh-keygen -t ed25519 -f ~/.ssh/lpzero
                            type %USERPROFILE%\.ssh\lpzero.pub   (Windows)
                            cat ~/.ssh/lpzero.pub                (macOS/Linux)
                        Paste that one line at the end of this file.

                        This file is the source of truth for SSH keys and
                        is copied to the machine on every boot. To manage
                        keys on the machine itself instead, delete it.

  wpa_supplicant.conf   Your WiFi network name and password.

                        Like authorized_keys, this file is the source of
                        truth while it exists here: it is copied to the
                        machine on every boot, so editing the copy there
                        does nothing. To manage WiFi from the machine
                        instead, delete this file.

  wpa-debug             Create this file - it can be empty - and the
                        next boot writes every step of the wireless
                        connection to /data/log/wpa.log: each frame in
                        and out, the association, and the four-way
                        handshake that follows it.

                        It exists because when the wireless will not
                        connect there is no other way to ask for that
                        log, since asking needs the wireless. Any PC can
                        make the file - this partition is FAT32.

                        Delete it again when you are done. On a board
                        that is working it writes about a megabyte an
                        hour of nothing.

  firewall.conf         Which ports this machine accepts. The firewall
                        is on by default and always keeps SSH open;
                        this file is where you open anything else, or
                        turn the whole thing off.

  beacon.conf           Where the board reports how it is doing. Put a
                        URL here and it says so every five minutes - if
                        the reports stop, that is your alarm. Without a
                        URL it still keeps the same numbers locally, in
                        /data/log/status.json.

  config.txt            GPU boot settings, screen resolution and so on.
  cmdline.txt           The kernel command line.

How to connect
  1. Fill in wpa_supplicant.conf (authorized_keys is optional - see above)
  2. Put the card in and apply power
  3. Find the address it was given, in your router's admin page
     (it is also printed on the serial console)
  4. ssh -i ~/.ssh/lpzero root@<that address>
     (or ssh -i lp_ssh_key, using the key the board made for itself)

If the WiFi will not connect
  1. touch wpa-debug on this partition (an empty file is enough)
  2. Boot the board and let it try for a minute
  3. Put the card back in a PC. The log is on the second partition, at
     /data/log/wpa.log - or read it on the machine with
     `more /data/log/wpa.log` if you can still get in over USB.

  The line that decides everything is "RX EAPOL": if it is there, the
  access point answered and the problem is the key or the cipher; if it
  is not, nothing came back and the problem is earlier than that.

Password authentication was left out of the build entirely. On a machine
open to a network a password is just a target for brute force. Only a
key gets in - so keep lp_ssh_key, or replace it with your own.

This partition is labelled LPZERO, and the machine will not mount one
that is not. That matters when booting from USB with some other SD card
still in the slot: without the check, that card's FAT partition would
become the boot partition, and everything above - the SSH key, the
firewall policy, the WiFi password, and a program run as root - would
come from it. Renaming this drive in Windows is harmless; the label the
check reads is the one written when the card was made.

Using it
  help              every command there is, a screen at a time
  help <command>    how one of them works
  sysinfo           what this machine is and how it is doing
  top               what is running, and how to stop it

  The shell completes with Tab, remembers what you typed (arrow keys),
  expands * and ?, takes NAME=value, and runs a command in the
  background with & at the end. Pipes, < > >> and && || ; all work.

  There is no scrollback on a screen, so long output pauses at the
  bottom - space for the next screen, q to stop. Piped or redirected it
  runs straight through.

Where files live
  To keep a file on the machine, put it under /data or in /root.
  Everything else is in RAM and disappears on reboot.

The boot screen and the kernel log
  cmdline.txt carries loglevel=4, so only warnings and worse reach the
  screen - otherwise kernel messages land in the middle of whatever you
  are typing. Nothing is lost: 'dmesg' shows the whole log and logd
  keeps a copy in /data/log/messages. Take loglevel=4 out of
  cmdline.txt to watch a boot in full.

Installing things
  pkg add <file.tar>     install a package you already have
  pkg repo <url>         where packages come from
  pkg update             fetch its index
  pkg install <name>     fetch, check the SHA-256, install
  pkg list / remove      what is installed, and take it out again

  A package is a plain uncompressed tar. Paths inside it land under
  /data, so bin/foo becomes /data/bin/foo, already on PATH.

The clock
  This board has no battery-backed clock. At power-on it starts at 1970.
  It fetches the time from the internet at boot (ntp), but that fails if
  your router or network blocks UDP 123. A wrong clock makes every HTTPS
  certificate check fail, so if Python reports a certificate error, check
  the time first with 'date'.

Python
  /data/bin/micropython        small and quick to start
  /data/python/bin/python3.12  full CPython (in the images that carry it)
  One of them is 'python'.

Commands you put in /data/rc.local run at every boot.
  If a mistake in that file hangs the machine, it is not fatal: after
  five boots that did not last five minutes the file is skipped and the
  system comes up plain. Fix it, then run 'bootcount -c'.

Where it boots from
  SD card, USB (a stick or a disk in an enclosure), NVMe and VirtIO all
  work from the same image. On the board, USB is worth using for /data:
  it is faster than the card and it does not wear out the way a cheap
  card does.

If it looks after itself
  'guard' watches memory, temperature, the power supply, runaway
  processes and disk space, and it keeps SSH reachable while any of them
  goes wrong. 'sysinfo' shows all of it, including whether the power
  supply has ever sagged - which is the usual reason SD cards go bad,
  and is otherwise completely silent. A thin cable or a phone charger
  will do it. Use 5V at 2.5A or better.

  A board that stops answering resets itself after 15 seconds.
READMEEOF
    if $UEFI_ONLY; then
        # ${EFI_BOOT_NAME}, not a fixed string: the amd64 image carries
        # BOOTX64.EFI and this text said BOOTAA64.EFI on it, which is
        # the sort of documentation that sends somebody looking for a
        # file that is not there.
        if [[ "$LP_ARCH" == "amd64" ]]; then
            cat >> "$README" <<UEFIEOF

────────────────────────────────────────────────────────────
이 이미지는 amd64 PC / 데스크톱 가상머신용입니다 (linux-LP).

UEFI 가 EFI/BOOT/${EFI_BOOT_NAME} 를 찾아 부팅합니다. MBR 부트스트랩
영역은 비어 있으므로 legacy BIOS 로는 부팅하지 않습니다 - EC2 등에
올릴 때 부트 모드를 uefi 로 지정하세요.

라즈베리파이용은 arm64 이미지를 받으세요.
────────────────────────────────────────────────────────────
UEFIEOF
        else
            cat >> "$README" <<UEFIEOF

────────────────────────────────────────────────────────────
이 이미지는 arm64 가상머신(UTM / QEMU / UTM SE) 전용입니다.

라즈베리파이 보드에서는 부팅하지 않습니다. 크기를 줄이려고 GPU
펌웨어가 읽는 커널 사본을 빼고, UEFI 가 읽는 EFI/BOOT/${EFI_BOOT_NAME}
한 벌만 넣었습니다.

실기에 쓰시려면 범용 이미지(.xz)를 받으세요.
────────────────────────────────────────────────────────────
UEFIEOF
        fi
    fi
    # e2fsck repairs the data partition; mke2fs makes a new one on a disk
    # you attach yourself ("datadisk --format"). Both go here rather than
    # into the system image, because the image is unpacked into RAM and
    # stays there: 2.6MB of memory for the life of the board, for two
    # programs that do nothing at all on a healthy boot. Here they cost
    # none, and this partition is mounted read-only, so neither can be
    # damaged by the failure it exists to repair.
    #
    # Both are checked against /etc/boot-tools.sha256 - which is inside
    # the kernel image - before anything runs them as root.
    # One directory per machine. arm64 keeps the original path; the
    # others are underneath it.
    #
    # This used to be one directory for everything, and the amd64 image
    # therefore carried an aarch64 e2fsck. Nothing complained: the file
    # copied fine, the card looked right, and the only way to find out
    # was to damage /data on a PC and watch the repair fail at exec.
    # So the architecture is checked here, and a tool for the wrong
    # machine is left off the card rather than shipped.
    PREBUILT="${REPO_ROOT}/userland/prebuilt"
    [[ "$LP_ARCH" == "arm64" ]] || PREBUILT="${PREBUILT}/${LP_ARCH}"
    for tool in e2fsck mke2fs resize2fs mke2fs.conf; do
        SRC="${PREBUILT}/${tool}"
        if [[ ! -f "$SRC" ]]; then
            echo "  경고: ${tool} 이 ${PREBUILT} 에 없습니다"
            echo "        'LP_ARCH=${LP_ARCH} ./tools/build-fsck.sh' 로 만드세요."
            echo "        없으면 기기에서 /data 를 고칠 수 없습니다."
            continue
        fi
        m="$(elf_machine_of "$SRC")"
        want=""
        case "$LP_ARCH" in
            arm64) want=0xb7 ;;
            amd64) want=0x3e ;;
            armv6) want=0x28 ;;
        esac
        if [[ -n "$m" && -n "$want" && "$m" != "$want" ]]; then
            die "${SRC} 는 ${LP_ARCH} 용이 아닙니다 (e_machine ${m}).
       이 파일은 부팅 중 root 로 실행됩니다. 아키텍처가 다르면
       실패하는 시점이 '/data 가 이미 깨진 뒤'입니다."
        fi
        mcopy -o -i "$PART_IMG" "$SRC" "::${tool}"
        log "복사: $(printf '%-36s' "$tool")($(stat -c%s "$SRC") bytes)"
    done

    log "복사: README.txt                          (설정 안내)"
    mcopy -i "$PART_IMG" "$README" ::README.txt
    rm -f "$README"
fi

# ── 3. MBR 을 붙여 완성 ─────────────────────────────────────────
log "MBR 작성 + 파티션 결합"

truncate -s "${IMAGE_MB}M" "$IMAGE"
dd if="$PART_IMG" of="$IMAGE" bs="$SECTOR_SIZE" seek="$BOOT_START_SECTOR" \
   conv=notrunc status=none

# ── 데이터 파티션 (ext4) ────────────────────────────────────────
# root 권한이나 loop 마운트 없이 파일 안에 직접 만든다.
TOTAL_SECTORS=$(( IMAGE_MB * 1024 * 1024 / SECTOR_SIZE ))
DATA_SECTORS=$(( TOTAL_SECTORS - DATA_START_SECTOR ))
DATA_IMG="${OUT_DIR}/.data-part.img"

log "데이터 파티션: ${DATA_SECTORS} 섹터 ($(( DATA_SECTORS * SECTOR_SIZE / 1024 / 1024 ))MiB, ext4)"
rm -f "$DATA_IMG"
truncate -s "$(( DATA_SECTORS * SECTOR_SIZE ))" "$DATA_IMG"
mkfs.ext4 -q -F -L "$DATA_LABEL" -m 0 "$DATA_IMG"

# ── 데이터 파티션 채우기 ────────────────────────────────────────
# debugfs 로 마운트 없이 ext4 안에 직접 써 넣는다. root 도 loop 장치도
# 필요 없어서 컨테이너/CI 안에서 그대로 돈다.
#
# 파일 하나마다 debugfs 를 새로 띄우면 CPython 처럼 수천 개짜리 트리에서
# 몇 분이 걸린다. 명령을 파일 하나에 모아 한 번에 실행한다.
if command -v debugfs >/dev/null 2>&1; then
    DBG="${OUT_DIR}/.debugfs-cmds"
    : > "$DBG"
    d_mkdir() { printf 'mkdir %s\n' "$1" >> "$DBG"; }
    d_link()  { printf 'symlink %s %s\n' "$1" "$2" >> "$DBG"; }
    # debugfs 의 write 는 항상 0100644 로 만든다. 실행 파일은 모드를 따로 세운다.
    d_put() {   # d_put <원본> <파티션안경로> [8진수모드]
        printf 'write %s %s\n' "$1" "$2" >> "$DBG"
        [[ -n "${3:-}" ]] && printf 'sif %s mode %s\n' "$2" "$3" >> "$DBG"
        return 0
    }

    d_mkdir /bin
    d_mkdir /root
    d_mkdir /root/.ssh

    # 첫 부팅에 쓸 WiFi 설정. SD 를 다시 굽지 않고 여기만 고쳐서
    # 공유기 설정을 바꿀 수 있다.
    WPA_SRC="${REPO_ROOT}/boot/rootfs-overlay/etc/wpa_supplicant.conf"
    # 0100644 을 명시한다. debugfs 의 write 가 원본 모드를 따라가는
    # 버전이 있어서, 실행 권한이 붙은 채로 들어가면 defend 가 매 부팅
    # "/data/wpa_supplicant.conf is executable, and that is a place for
    # data" 를 보고한다. 설정 파일 하나 때문에 보안 도구가 늑대를
    # 외치게 두면, 진짜 보고를 사람이 안 읽게 된다.
    [[ -f "$WPA_SRC" ]] && d_put "$WPA_SRC" wpa_supplicant.conf 0100644 \
        && log "데이터: wpa_supplicant.conf"

    # 파이썬을 시스템(initramfs)이 아니라 여기에 두는 이유는 크기다.
    # initramfs 는 커널에 박혀 있어서 늘리면 부팅 이미지가 그대로 커지지만,
    # /data 는 첫 부팅에 카드 전체로 늘어나므로 크기가 문제되지 않는다.
    # Per architecture, like everything else that ships a binary.
    MPY_BUILD="build-lpzero"
    [[ "$LP_ARCH" == "amd64" ]] && MPY_BUILD="build-lpzero-amd64"
    MPY="${MICROPYTHON_BIN:-${WORK}/micropython/ports/unix/${MPY_BUILD}/micropython}"
    HAVE_MPY=0
    if $WITH_MPY && [[ -f "$MPY" ]]; then
        d_put "$MPY" bin/micropython 0100755
        HAVE_MPY=1
        log "데이터: bin/micropython  ($(stat -c%s "$MPY") bytes)"
    fi

    # CPython 을 빌드해두었으면 트리째 넣는다 (tools/build-python.sh).
    PY_ROOT="$PYSTAGE_ROOT"
    [[ "$LP_ARCH" == "amd64" ]] && PY_ROOT="${PYSTAGE_ROOT}-amd64"

    # Check what goes on the data partition, not only what goes in the
    # initramfs. The first amd64 image had a clean initramfs and 43MB of
    # aarch64 on /data - glibc, CPython and micropython - so it booted
    # and then had no working interpreter at all. Two separate places
    # ship binaries; both are checked now.
    check_tree_arch "$PY_ROOT" "$LP_ARCH" "/data staging (${PY_ROOT})"
    if $WITH_MPY && [[ -f "$MPY" ]]; then
        m="$(elf_machine_of "$MPY")"
        want=0xb7; [[ "$LP_ARCH" == "amd64" ]] && want=0x3e
        [[ "$m" == "$want" ]] || die "micropython 이 ${LP_ARCH} 가 아닙니다 (e_machine ${m}): $MPY"
    fi

    PYSTAGE="${PYSTAGE:-${PY_ROOT}/data/python}"
    if [[ -d "$PYSTAGE" ]]; then
        log "데이터: CPython 트리 준비 중..."
        # 디렉터리가 먼저, 그 다음 파일. find 는 부모를 먼저 내므로 순서가 맞다.
        # 원본 경로는 절대경로여야 한다. debugfs 는 여기서 cd 한 곳이 아니라
        # 스크립트를 부른 디렉터리에서 돌기 때문이다.
        PYPARENT="$(cd "$(dirname "$PYSTAGE")" && pwd)"
        ( cd "$PYPARENT"
          find python -type d  -printf 'mkdir /%p\n'
          find python -type f  -printf "write ${PYPARENT}/%p %p\n"
          find python -type f -perm -u+x -printf 'sif %p mode 0100755\n'
          find python -type l  -printf 'symlink /%p %l\n' ) >> "$DBG"
        log "데이터: python/  ($(du -sh "$PYSTAGE" | cut -f1))"
    fi

    # glibc, next to Python and only for Python.
    #
    # The operating system does not use it: init, the shell and every
    # command run on the libc in userland/libc with nothing else linked
    # in. This is here so that CPython can be dynamically linked, and it
    # has to be, because every wheel on PyPI carrying a compiled
    # extension is built against glibc. Static Python could not load one
    # at all - "pip install numpy" got as far as downloading it.
    #
    # It lives on the data partition, so the system image is exactly the
    # size it was.
    GLIBCSTAGE="${GLIBCSTAGE:-${PY_ROOT}/data/glibc}"
    if [[ -d "$GLIBCSTAGE" ]]; then
        GPARENT="$(cd "$(dirname "$GLIBCSTAGE")" && pwd)"
        ( cd "$GPARENT"
          find glibc -type d -printf 'mkdir /%p\n'
          find glibc -type f -printf "write ${GPARENT}/%p %p\n"
          find glibc -type f -printf 'sif %p mode 0100755\n' ) >> "$DBG"
        log "데이터: glibc/  ($(du -sh "$GLIBCSTAGE" | cut -f1), 파이썬 확장 모듈용)"
    fi

    # 루트 인증서. OpenSSL 을 --openssldir=/data/ssl 로 지었으므로
    # 파이썬과 다른 프로그램이 여기를 본다. 없으면 HTTPS 접속이
    # CERTIFICATE_VERIFY_FAILED 로 죽는다.
    CA_SRC="${CA_BUNDLE:-${WORK}/ca/cert.pem}"
    if [[ -f "$CA_SRC" ]]; then
        d_mkdir /ssl
        d_put "$CA_SRC" ssl/cert.pem
        log "데이터: ssl/cert.pem  (루트 인증서 $(grep -c 'BEGIN CERTIFICATE' "$CA_SRC")개)"
    fi

    # terminfo. readline 이 터미널의 능력(화면 크기, 키 코드)을 여기서
    # 읽는다. 없어도 방향키는 되지만 화면 폭 계산이 틀어진다.
    # 바이너리 형식이 아키텍처와 무관해서 호스트 것을 그대로 쓴다.
    #
    # find 에 없는 디렉터리를 넘기면 find 가 1 로 끝나고, pipefail 때문에
    # 대입이 실패하면서 set -e 가 스크립트를 끊는다. 있는 것만 넘긴다.
    TI_DIRS=()
    for d in /usr/share/terminfo /lib/terminfo /etc/terminfo; do
        [[ -d "$d" ]] && TI_DIRS+=("$d")
    done
    TI_N=0
    for t in xterm xterm-256color linux vt100 vt102 vt220 screen screen-256color ansi dumb; do
        [[ ${#TI_DIRS[@]} -gt 0 ]] || break
        ti_src=$(find "${TI_DIRS[@]}" -name "$t" -type f 2>/dev/null | head -1 || true)
        [[ -n "$ti_src" ]] || continue
        [[ "$TI_N" == "0" ]] && d_mkdir /terminfo
        d_mkdir "/terminfo/${t:0:1}"
        d_put "$ti_src" "terminfo/${t:0:1}/${t}"
        TI_N=$((TI_N + 1))
    done
    [[ "$TI_N" -gt 0 ]] && log "데이터: terminfo/  (${TI_N}개 터미널)"

    # 사용자가 python 이라고 쳤을 때 쓸 것을 정한다.
    # CPython 이 있으면 그것이, 없으면 MicroPython 이 python 이다.
    if [[ -d "$PYSTAGE" ]]; then
        d_link /bin/python3 /data/python/bin/python3.12
        d_link /bin/python  /data/python/bin/python3.12
        # pip lives beside the interpreter, and /data/python/bin is not
        # on PATH - only /data/bin is. Without these, "pip install"
        # answers "command not found" on a system that has pip.
        if [[ -f "${PYSTAGE}/bin/pip" ]]; then
            d_link /bin/pip  /data/python/bin/pip
            d_link /bin/pip3 /data/python/bin/pip
        fi
    elif [[ "$HAVE_MPY" == "1" ]]; then
        d_link /bin/python3 /data/bin/micropython
        d_link /bin/python  /data/bin/micropython
    fi

    debugfs -w -f "$DBG" "$DATA_IMG" > "${OUT_DIR}/.debugfs.log" 2>&1 || true
    # debugfs 는 실패해도 종료코드가 0 이다. 결과를 직접 확인한다.
    if ! debugfs -R "stat bin/micropython" "$DATA_IMG" 2>/dev/null | grep -q 'Mode:  0755'; then
        [[ "$HAVE_MPY" == "1" ]] && die "데이터 파티션에 micropython 을 넣지 못했습니다 (${OUT_DIR}/.debugfs.log 확인)"
    fi
    rm -f "$DBG"
else
    log "debugfs 없음 - 데이터 파티션을 비워 둡니다 (apt install e2fsprogs)"
fi

dd if="$DATA_IMG" of="$IMAGE" bs="$SECTOR_SIZE" seek="$DATA_START_SECTOR" \
   conv=notrunc status=none
rm -f "$DATA_IMG"

python3 "${REPO_ROOT}/tools/write-mbr.py" "$IMAGE" \
    "$BOOT_START_SECTOR" "$PART_SECTORS" "$DATA_START_SECTOR" "$DATA_SECTORS"

rm -f "$PART_IMG"

# ── 4. 결과 확인 ────────────────────────────────────────────────
echo ""
echo "완성: ${IMAGE}  ($(du -h "$IMAGE" | cut -f1))"
echo ""
echo "부트 파티션 내용:"
mdir -i "${IMAGE}@@$(( BOOT_START_SECTOR * SECTOR_SIZE ))" :: 2>/dev/null | sed 's/^/  /'

cat <<EOF

SD카드에 굽기:
  sudo dd if=${IMAGE} of=/dev/sdX bs=4M conv=fsync status=progress

시리얼 콘솔 연결 (USB-TTL 어댑터):
  Pi 헤더 8번 (GPIO14 TX) -> 어댑터 RX
  Pi 헤더 10번(GPIO15 RX) -> 어댑터 TX
  Pi 헤더 6번 (GND)       -> 어댑터 GND
  screen /dev/ttyUSB0 115200      (또는 minicom / picocom)
EOF
