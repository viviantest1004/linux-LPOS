#!/usr/bin/env bash
#
# mkrootfs.sh - 루트 파일시스템을 조립하고 initramfs(cpio)를 만든다.
#
# 리눅스는 부팅 시 initramfs 를 램디스크로 풀고 그 안의 /init 을 PID 1 로
# 실행한다. 형식은 newc cpio 아카이브다.
#
# 우리 프로그램은 전부 정적 링크라 라이브러리도 동적 링커도 필요 없다.
# 외부에서 가져오는 것은 셋뿐이다:
#   dropbear         SSH 서버 (직접 만든 암호 구현은 위험하다)
#   wpa_supplicant   WPA2 인증 (같은 이유)
#   brcm 펌웨어      무선칩 내부로 올라가는 블롭
#
# 사용법:
#   ./mkrootfs.sh              rootfs/ 조립
#   ./mkrootfs.sh --cpio       initramfs.cpio.gz 까지
#   ./mkrootfs.sh --test       chroot 로 실행 (root 필요)

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"


BIN_DIR="${HERE}/${LP_BINDIR:-bin}"
ROOT_DIR="${HERE}/${LP_ROOTFS_DIR:-rootfs}"
CPIO_OUT="${HERE}/${LP_CPIO_NAME:-initramfs.cpio.gz}"
OVERLAY="${REPO_ROOT}/boot/rootfs-overlay"
FW_DIR="${REPO_ROOT}/blobs/brcm"
THIRD="${THIRDPARTY_DIR:-${LPZERO_WORK}/thirdparty}"

# 프로그램 목록은 Makefile 의 PROGS 에서 읽는다.
# 두 곳에 나눠 적으면 새 도구를 추가할 때 한쪽을 빠뜨린다
# (실제로 zram/guard/expandfs/mkdir 를 빌드해놓고 rootfs 에 넣지 않아
#  부팅 후 "명령을 찾을 수 없습니다" 가 났었다).
read -r -a PROGRAMS <<< "$(sed -n 's/^PROGS[[:space:]]*:*=[[:space:]]*//p' "${HERE}/Makefile" | tail -1)"
[[ ${#PROGRAMS[@]} -gt 0 ]] || die "Makefile 에서 PROGS 를 읽지 못했습니다"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
log() { printf '  %s\n' "$*"; }

# ── 소스보다 오래된 바이너리를 이미지에 넣지 않는다 ──
#
# 실제로 겪은 사고: persist.c 를 고치고 arm64 만 빌드한 뒤 amd64 이미지를
# 만들었더니, 이미지 안의 persist 는 고치기 전 것이었다. 셀프테스트가
# "usage: persist" 를 뱉었고, 원인을 찾는 데 한참 걸렸다 - 코드는 맞는데
# 실행되는 물건이 달랐기 때문이다.
#
# 빌드가 조용히 건너뛴 파일 하나가 이미지 전체를 거짓말로 만든다. 그래서
# 여기서 멈춘다. check_tree_arch 가 아키텍처를 대조하는 것과 같은 이유다.
stale=0
for p in "${PROGRAMS[@]}"; do
    src="${HERE}/${p}/${p}.c"
    bin="${BIN_DIR}/${p}"
    [[ -f "$src" && -f "$bin" ]] || continue
    if [[ "$src" -nt "$bin" ]]; then
        echo "  ** ${bin} 이 ${p}/${p}.c 보다 오래되었습니다" >&2
        stale=$((stale + 1))
    fi
done
[[ $stale -eq 0 ]] || die "${stale}개의 바이너리가 소스보다 오래되었습니다. 'make' 를 다시 실행하세요."

for p in "${PROGRAMS[@]}"; do
    [[ -f "${BIN_DIR}/${p}" ]] || die "bin/${p} 가 없습니다. 'make' 를 먼저 실행하세요."
done

echo "rootfs 조립 중..."
# 외부 프로그램은 지우기 전에 챙겨 둔다.
#
# dropbear 와 wpa_supplicant 는 .build/thirdparty 에서 오는데, 그
# 디렉터리는 빌드 산출물이라 없을 수 있다 (컨테이너를 새로 받았거나,
# 자리를 비우려고 지웠거나). 그때 이 스크립트는 경고 한 줄을 찍고
# 계속 진행했고, 나온 이미지에는 SSH 도 WiFi 도 없었다 - 부팅은
# 멀쩡히 되므로 켜 보기 전에는 알 수가 없다.
#
# 그래서 지우기 전에 이미 있던 것을 옮겨 두고, 새로 복사할 것이
# 없으면 그것을 도로 넣는다. 저장소에 커밋되어 있는 바이너리가
# 그대로 쓰이는 셈이고, 빌드 디렉터리가 있으면 그쪽이 이긴다.
KEEP="$(mktemp -d)"
for f in dropbear dropbearkey wpa_supplicant wpa_cli ; do
    [[ -f "${ROOT_DIR}/bin/$f" ]] && cp -a "${ROOT_DIR}/bin/$f" "$KEEP/$f"
done

rm -rf "$ROOT_DIR"
# /media 는 automount 가 꽂힌 드라이브를 붙이는 자리다. 미리 만들어
# 두는 이유는, 없으면 automount 가 첫 드라이브에서 디렉터리를 만들어야
# 하는데 그 시점의 실패는 "드라이브를 못 읽는다" 처럼 보이기 때문이다.
mkdir -p "$ROOT_DIR"/{bin,sbin,opt,srv,dev/pts,proc,sys,tmp,etc,root/.ssh,data,media,mnt,var/run,lib/firmware/brcm,usr/local/bin,usr/bin,usr/sbin,usr/lib}

# The key that says an update is genuine.
#
# Public half only: the board checks signatures and cannot make one.
# It goes in the initramfs rather than on the card so that it is part of
# the running image - a key on a filesystem anyone can edit is not a key,
# it is a suggestion.
#
# An image built without keys/update-key.pub has no key at all, and
# `update` then refuses to install anything unless the person running it
# passes the hash themselves. That is the right default for somebody
# else's build: it cannot be signed by us, so it should not claim to be.
if [ -f "${REPO_ROOT}/keys/update-key.pub" ]; then
    cp "${REPO_ROOT}/keys/update-key.pub" "$ROOT_DIR/etc/update-key.pub"
    echo "  update key: $(wc -c < "$ROOT_DIR/etc/update-key.pub") bytes"
else
    echo "  update key: none - this image will refuse unsigned updates"
fi

# ── 우리 프로그램 ────────────────────────────────────────────────
for p in "${PROGRAMS[@]}"; do
    # rm first: if an earlier build left bin/<p> as an alias symlink (sudo
    # used to be a link to su, less to more), cp would follow it and write
    # the new program over the link's TARGET - replacing su with sudo.
    rm -f "${ROOT_DIR}/bin/${p}"
    cp "${BIN_DIR}/${p}" "${ROOT_DIR}/bin/${p}"
done

# An alias only where there is no real program by that name. sudo and
# less were aliases (of su and more) until they became programs of their
# own; the unconditional ln -sf that made them would now replace the real
# ones with links every time this script ran.
alias_bin() {
    [[ -e "${ROOT_DIR}/bin/$2" && ! -L "${ROOT_DIR}/bin/$2" ]] && return 0
    ln -sf "$1" "${ROOT_DIR}/bin/$2"
}

# Our shell under the name that is always ours. On these RAM-root images
# /bin/sh is our shell too; on the desktop /bin/sh belongs to Debian's
# dash, and init, /etc/rc and the login shell use /bin/lpsh. A hard link,
# so it is one file under two names and cannot drift.
ln -f "${ROOT_DIR}/bin/sh" "${ROOT_DIR}/bin/lpsh"
# mount 는 argv[0] 로 동작을 가른다. 같은 파일을 umount 이름으로도 둔다.
alias_bin mount "umount"
# chattr 도 argv[0] 로 갈린다. lsattr 은 같은 파일이다.
alias_bin chattr "lsattr"
# 나머지 argv[0] 쌍들. 하는 일이 같고 기본값만 다른 것들이라
# 파일을 두 개 만들 이유가 없다.
alias_bin chown "chgrp"
alias_bin id "groups"
alias_bin useradd "userdel"
alias_bin su "sudo"
# `less` is what people type. `more` here already scrolls both ways and
# quits on q, so the name is the only thing that was missing.
alias_bin more "less"
# md5sum and its siblings are the same program: one digest front end that
# picks the algorithm from the name it was called by.
alias_bin sha256sum "md5sum"
alias_bin sha256sum "sha1sum"
alias_bin sha256sum "sha512sum"
# base32 is base64 with a different alphabet, and nothing else.
alias_bin base64 "base32"
# `[` is test. The shell has both built in; the files have to exist too,
# because `find -exec [ ... ]` and a crontab line look for a file.
alias_bin test "["
# pkill is pgrep that sends a signal instead of printing a pid.
alias_bin pgrep "pkill"
# gunzip is `gzip -d` and zcat is `gzip -dc`. One decompressor, three
# names, because those are the three names people type.
alias_bin gzip "gunzip"
alias_bin gzip "zcat"
log "bin/: ${PROGRAMS[*]} umount lsattr chgrp groups userdel sudo less md5sum sha1sum sha512sum base32 [ pkill gunzip zcat"

# 커널은 initramfs 의 /init 을 PID 1 로 실행한다
ln -sf bin/init "${ROOT_DIR}/init"

# ── 외부 프로그램 ────────────────────────────────────────────────
copy_third() {
    local src="$1" name="$2"
    if [[ -f "$src" ]]; then
        cp "$src" "${ROOT_DIR}/bin/${name}"
        printf "  %-16s %8s bytes\n" "$name" "$(stat -c%s "$src")"
        return 0
    fi
    # 빌드해 둔 것이 없으면 지우기 전에 챙겨 둔 것을 쓴다.
    if [[ -f "$KEEP/$name" ]]; then
        cp -a "$KEEP/$name" "${ROOT_DIR}/bin/${name}"
        printf "  %-16s %8s bytes  (저장소에 있던 것)\n" \
            "$name" "$(stat -c%s "$KEEP/$name")"
        return 0
    fi
    printf "  경고: %s 가 없습니다 (%s)\n" "$name" "$src"
    MISSING_THIRD="${MISSING_THIRD}${MISSING_THIRD:+, }$name"
    return 1
}
MISSING_THIRD=""

echo ""
echo "외부 프로그램:"
# dropbear, built for the machine this image is for. A binary for the
# wrong architecture does not fail to copy and does not fail to install:
# it fails at exec, with 127, once every five seconds for ever, because
# dropbear is on init's critical list and init never gives up on those.
# On the first amd64 image that filled the screen and nothing else was
# readable.
DROPBEAR_SRC="${THIRD}/dropbear-2024.86"
# The path build-thirdparty.sh actually writes to, which is
# .build/thirdparty/<arch>/<package> for everything except arm64 - that
# one builds where the sources are unpacked, as it always has.
#
# The amd64 entries here used to name dropbear-amd64 and wpa-amd64,
# which no version of build-thirdparty.sh has ever produced. copy_third
# ends in "|| true", so the miss was silent and the amd64 image simply
# kept whatever binary was already committed in the rootfs directory -
# for as long as that took to notice, which was months.
case "${LP_ARCH:-arm64}" in
    amd64) DROPBEAR_SRC="${THIRD}/amd64/dropbear-2024.86" ;;
    armv6) DROPBEAR_SRC="${THIRD}/armv6/dropbear-2024.86" ;;
esac
copy_third "${DROPBEAR_SRC}/dropbear"    dropbear    || true
copy_third "${DROPBEAR_SRC}/dropbearkey" dropbearkey || true
# wpa_supplicant, for the machine this image is for. Same reason as
# dropbear above: an aarch64 binary on an amd64 image does not fail to
# copy, it fails at exec, and WiFi comes up as "Exec format error" on a
# board that otherwise looks fine.
WPA_SRC="${THIRD}/wpa_supplicant-2.11/wpa_supplicant"
case "${LP_ARCH:-arm64}" in
    amd64) WPA_SRC="${THIRD}/amd64/wpa_supplicant-2.11/wpa_supplicant" ;;
    armv6) WPA_SRC="${THIRD}/armv6/wpa_supplicant-2.11/wpa_supplicant" ;;
esac
copy_third "${WPA_SRC}/wpa_supplicant" wpa_supplicant || true
copy_third "${WPA_SRC}/wpa_cli"        wpa_cli        || true

# ── 무선칩 펌웨어 ────────────────────────────────────────────────
# brcmfmac 드라이버가 /lib/firmware/brcm/ 에서 찾는다.
if [[ -d "$FW_DIR" ]] && compgen -G "${FW_DIR}/*" >/dev/null; then
    cp "${FW_DIR}"/* "${ROOT_DIR}/lib/firmware/brcm/"

    # 규제 데이터베이스는 brcm/ 이 아니라 한 단계 위다.
    #
    # 커널은 이걸 칩 펌웨어가 아니라 "regulatory.db" 라는 이름 그대로
    # 요청하고, 펌웨어 로더는 /lib/firmware/regulatory.db 를 본다.
    # brcm/ 안에 두면 정확히 지금까지처럼 못 찾는다:
    #
    #   platform regulatory.0: Direct firmware load for regulatory.db
    #                          failed with error -2
    #
    # 서명(.p7s)과 짝이라 둘 다 있어야 한다.
    for rdb in regulatory.db regulatory.db.p7s; do
        if [[ -f "${FW_DIR}/${rdb}" ]]; then
            mv "${ROOT_DIR}/lib/firmware/brcm/${rdb}" \
               "${ROOT_DIR}/lib/firmware/${rdb}"
        fi
    done

    echo ""
    if [[ -f "${ROOT_DIR}/lib/firmware/regulatory.db" ]]; then
        log "lib/firmware/regulatory.db  ($(stat -c%s "${ROOT_DIR}/lib/firmware/regulatory.db") bytes, 서명 포함)"
    else
        log "경고: regulatory.db 가 없습니다. 무선이 world 도메인으로 떨어집니다."
    fi
    log "lib/firmware/brcm/: $(ls -1 "${ROOT_DIR}/lib/firmware/brcm" | wc -l)개 파일"
else
    echo ""
    log "경고: blobs/brcm 이 비어 있습니다. './tools/fetch-wifi-fw.sh' 를 실행하세요."
    log "      (없으면 wlan0 인터페이스가 생기지 않습니다)"
fi

# ── 설정 파일 ────────────────────────────────────────────────────
if [[ -d "$OVERLAY" ]]; then
    cp -r "${OVERLAY}/." "${ROOT_DIR}/"

    # The hostname is in /etc/rc, and /etc/rc is shared between the two
    # builds, so it is substituted here rather than kept in two copies
    # that would drift. LP_HOSTNAME comes from the Makefile: lpzero on
    # arm64, linux-lp on amd64.
    if [[ -n "${LP_HOSTNAME:-}" && -f "${ROOT_DIR}/etc/rc" ]]; then
        sed -i "s/^echo lpzero > /echo ${LP_HOSTNAME} > /" "${ROOT_DIR}/etc/rc"
        echo "  hostname:   ${LP_HOSTNAME}"
    fi
    printf '%s\n' "${LP_OS_NAME:-LP-zero}" > "${ROOT_DIR}/etc/osname"
fi

# dropbear 는 사용자의 홈 디렉터리를 /etc/passwd 에서 찾는다.
# 이 파일이 없으면 로그인이 되지 않는다.
cat > "${ROOT_DIR}/etc/passwd" <<'PASSWD'
root:x:0:0:root:/root:/bin/sh
# 로그인용 계정이 아니다. dropprivs 가 권한을 낮춰 프로그램을 돌릴 때
# 쓰는 상대이며, 여기 있는 이유는 ls -l 이 숫자 대신 이름을 찍게
# 하기 위해서다. 셸이 nologin 인 것은 이 계정으로 들어올 수 없다는 뜻.
#
#   dropprivs 1000 python3 /data/app.py
#
# 네트워크에서 오는 것을 다루는 스크립트는 root 로 돌리지 않는 편이
# 낫다. 그 스크립트의 실수가 곧 기기 전체를 잃는 일이 되지 않는다.
user:x:1000:1000:unprivileged:/data/user:/bin/false
PASSWD
cat > "${ROOT_DIR}/etc/group" <<'GROUP'
root:x:0:
user:x:1000:
GROUP
printf 'lpzero\n' > "${ROOT_DIR}/etc/hostname"

# ── 이미지에 공개키 굽기 ────────────────────────────────────────
#
#   AUTHORIZED_KEYS=~/.ssh/id_ed25519.pub ./mkrootfs.sh
#
# 여기 넣은 키는 커널 이미지 안에 들어가 매 부팅 램에 펼쳐진다. 어떤
# 파일시스템에서도 닿을 수 없으므로, /data 가 랜섬웨어로 암호화되고
# /boot 까지 지워져도 재부팅 한 번이면 다시 SSH 로 들어올 수 있다.
# 카드를 새로 굽는 것 외에는 지울 방법이 없는 유일한 사본이다.
#
# 넣지 않으면 안내문만 들어가고, 부팅 때 authkey 가 "아무도 못 들어옴"
# 이라고 경고한다.
if [[ -n "${AUTHORIZED_KEYS:-}" ]]; then
    [[ -f "$AUTHORIZED_KEYS" ]] \
        || { echo "error: 공개키 파일이 없습니다: $AUTHORIZED_KEYS" >&2; exit 1; }
    # 개인키를 실수로 넣는 사고를 막는다. 공개키는 한 줄이고 ssh- 로 시작한다.
    if grep -q "PRIVATE KEY" "$AUTHORIZED_KEYS"; then
        echo "error: $AUTHORIZED_KEYS 는 개인키입니다. .pub 파일을 주세요." >&2
        exit 1
    fi
    cat "$AUTHORIZED_KEYS" >> "${ROOT_DIR}/etc/authorized_keys"
    log "etc/authorized_keys: 이미지에 공개키 $(grep -c "^ssh-" "$AUTHORIZED_KEYS")개 구움"
fi

# 공개키는 ~/.ssh/authorized_keys 에 있어야 dropbear 가 읽는다.
if [[ -f "${ROOT_DIR}/etc/authorized_keys" ]]; then
    cp "${ROOT_DIR}/etc/authorized_keys" "${ROOT_DIR}/root/.ssh/authorized_keys"
    chmod 600 "${ROOT_DIR}/root/.ssh/authorized_keys"
fi
chmod 700 "${ROOT_DIR}/root/.ssh" "${ROOT_DIR}/root"

cat > "${ROOT_DIR}/etc/motd" <<MOTD
${LP_OS_NAME:-LP-zero} OS

  bare-metal firmware -> Linux kernel -> own libc -> own init -> own shell

Where files live

  /data     the SD card. Survives a reboot.
  /root     home. Bound to /data, so it survives too.
  anywhere else   RAM. Gone on reboot, including /tmp and /etc.

  To keep a file, put it under /data or in your home directory.

  help        list the built-in commands
  edit FILE   text editor        touch FILE   create an empty file
  calc 2+3*4  decimal, hex and binary at once
  date        the time. This board has no clock battery, and a wrong
              clock makes every HTTPS certificate check fail.
              'ntp' sets it from the network, 'date -z list' picks a zone.
  python      on /data
  sysinfo     how the machine is doing

  Commands in /data/rc.local run at every boot.
MOTD

# ── /boot 에서 실행할 바이너리의 해시 ────────────────────────────
#
# fsck 는 /boot/e2fsck 를, datadisk --format 은 /boot/mke2fs 를 root 로 실행한다. /boot 는 FAT 파티션이고,
# FAT 파티션은 카드를 뽑아 아무 PC 에나 꽂으면 쓸 수 있다. 그러니
# "부트 파티션에 있는 e2fsck" 는 그 자체로는 신뢰할 근거가 없다.
#
# 기대되는 해시를 시스템 이미지 안에 넣는다. 이 파일은 initramfs 안,
# 즉 커널 이미지 안에 있고 부팅 때 램으로 풀린다 - 어떤 파일시스템에서도
# 손댈 수 없다. 해시가 다르면 fsck 는 실행하지 않는다.
# 형식은 sha256sum 이 쓰는 것 그대로: "<해시>  <이름>".
: > "${ROOT_DIR}/etc/boot-tools.sha256"
# One directory per machine, matching what mksdcard.sh puts on the card.
# Hashing the arm64 e2fsck into an armv6 image would not be a wrong
# number in a file, it would be a board that refuses to repair itself:
# the hash never matches, so fsck never runs.
PREBUILT="${HERE}/prebuilt"
[[ "${LP_ARCH:-arm64}" == "arm64" ]] || PREBUILT="${PREBUILT}/${LP_ARCH}"
for tool in e2fsck mke2fs resize2fs; do
    src="${PREBUILT}/${tool}"
    if [[ -f "$src" ]]; then
        printf '%s  %s\n' \
            "$(sha256sum "$src" | cut -d' ' -f1)" "$tool" \
            >> "${ROOT_DIR}/etc/boot-tools.sha256"
        log "etc/boot-tools.sha256  ${tool} $(sha256sum "$src" | cut -c1-16)..."
    else
        # 없으면 줄도 쓰지 않는다. 기대 해시가 없으면 실행을 거부하는
        # 쪽을 택한다 - 자동 복구나 --format 을 잃을 뿐이다.
        log "경고: ${PREBUILT#"${HERE}/"}/${tool} 이 없습니다"
    fi
done

# When this image was built.
#
# A board with no battery-backed clock starts at 1970 on its first boot
# and stays there until the network gives it a time. If the network is
# the broken thing, that is for ever, and every log line carries the
# same timestamp. `ntp -r` uses this as a floor: not the right time, but
# a true lower bound, and enough to put the log in order.
date +%s > "${ROOT_DIR}/etc/build-epoch"
log "etc/build-epoch  $(cat "${ROOT_DIR}/etc/build-epoch") ($(date -u -d @"$(cat "${ROOT_DIR}/etc/build-epoch")" '+%Y-%m-%d %H:%M:%S') UTC)"

echo ""
log "etc/: rc, wpa_supplicant.conf, authorized_keys, passwd, group, motd, boot-tools.sha256, build-epoch"

# ── 장치 노드 ────────────────────────────────────────────────────
# 커널은 init 실행 전 /dev/console 을 열어 fd 0,1,2 로 준다.
# 없으면 init 이 파일 디스크립터 하나 없이 시작해 오류조차 못 낸다.
if [[ "$(id -u)" == "0" ]]; then
    mknod -m 600 "${ROOT_DIR}/dev/console" c 5 1
    mknod -m 666 "${ROOT_DIR}/dev/null"    c 1 3
    mknod -m 666 "${ROOT_DIR}/dev/zero"    c 1 5
    mknod -m 444 "${ROOT_DIR}/dev/urandom" c 1 9
    log "dev/: console, null, zero, urandom"
else
    log "경고: root 가 아니라 장치 노드를 만들지 못했습니다"
fi

# Everything in here has to be for the machine this image is for. See
# check_tree_arch in tools/common.sh for the morning that produced this.
check_tree_arch "$ROOT_DIR" "${LP_ARCH:-arm64}" "initramfs"

TOTAL=$(du -sb "$ROOT_DIR" | cut -f1)
echo ""
printf "rootfs: %s  (%.2f MB)\n" "$ROOT_DIR" "$(echo "scale=2; $TOTAL/1048576" | bc)"

# 빠진 것이 있으면 크게 말한다. 경고 한 줄은 스무 줄의 정상 출력
# 사이에서 눈에 띄지 않고, 그 결과가 SSH 없는 이미지다.
if [[ -n "$MISSING_THIRD" ]]; then
    echo ""
    echo "  ⚠ 이 rootfs 에는 다음이 없습니다: $MISSING_THIRD"
    echo "    ./tools/build-thirdparty.sh 로 빌드하거나,"
    echo "    저장소에 커밋된 바이너리를 git checkout 으로 되살리십시오."
fi
rm -rf "$KEEP"

# ── initramfs cpio ──────────────────────────────────────────────
if [[ "${1:-}" == "--cpio" ]]; then
    command -v cpio >/dev/null 2>&1 || die "cpio 가 필요합니다 (apt install cpio)"

    # newc 가 리눅스 initramfs 의 표준 형식이다.
    # --reproducible 로 타임스탬프/inode 를 고정해 빌드를 재현 가능하게 한다.
    ( cd "$ROOT_DIR" && find . -print0 \
        | LC_ALL=C sort -z \
        | cpio --null --create --format=newc --reproducible --quiet ) \
        | gzip -9 -n > "$CPIO_OUT"

    printf "initramfs: %s  (%.2f MB)\n" "$CPIO_OUT" \
        "$(echo "scale=2; $(stat -c%s "$CPIO_OUT")/1048576" | bc)"
fi

if [[ "${1:-}" == "--test" ]]; then
    [[ "$(id -u)" == "0" ]] || die "chroot 테스트는 root 권한이 필요합니다"
    echo ""
    chroot "$ROOT_DIR" /bin/init
fi
