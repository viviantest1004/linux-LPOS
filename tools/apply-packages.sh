#!/usr/bin/env bash
#
# apply-packages.sh - install tools/desktop-packages.list into the Debian base.
#
# The base under the amd64 desktop (tools/mkdesktop.sh) is a Debian
# bookworm tree that every desktop program is built against. This is the
# one way packages get into it, so that what the image carries is what
# the list in the repository says and nothing an afternoon at a chroot
# prompt left behind.
#
#   sudo ./tools/apply-packages.sh              install whatever is missing
#   LP_DEB=/elsewhere ./tools/apply-packages.sh a base somewhere else
#
# ── Four things the chroot needs that it does not have ──
#
# A POSIX /bin/sh. The base's /bin is our userland - that is the design,
# mkdesktop.sh explains it - and our shell is not what dpkg's maintainer
# scripts or apt-key were written for. apt-key under it printed
# "Unknown option: --readonly" into a file called "&2" at the top of the
# base, and signature checking failed with it.
#
# A /bin that Debian's packages can write to. Bookworm packages still
# ship files under /bin (iputils-ping ships /bin/ping) and expect /bin to
# be the usrmerge symlink into /usr/bin. Here /bin is a real directory
# holding our commands, so an install would overwrite our ping with
# Debian's. For the length of the run, /usr/bin is bind-mounted over
# /bin: dpkg sees an ordinary merged-/usr system, its /bin files land in
# /usr/bin where they would on any Debian machine, our /bin is not
# touched, and /bin/sh is Debian's dash by construction - which is the
# first problem solved by the same mount.
#
# The build proxy. Outbound https from the build host goes through a
# proxy that re-signs certificates. apt is told about it on its own
# command line for each call - never in a file under /etc/apt, because
# a file there ships with the image, and a machine that tries a proxy on
# 127.0.0.1 for every download is broken in a way nobody can see. The
# proxy's CA is copied into the base's /tmp for the run and removed on
# the way out, whatever the way out is.
#
# No daemons. Maintainer scripts start their services through
# invoke-rc.d, and without a policy-rc.d saying no, installing `at`
# starts an atd on the build host. The policy file exists only for the
# run.
#
# All of it happens in a private mount namespace, so none of these bind
# mounts is visible to anything else using the base at the same time.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
source "${REPO_ROOT}/tools/common.sh"

DEB="${LP_DEB:-${LPZERO_WORK}/deb}"
LIST="${LP_PACKAGE_LIST:-${REPO_ROOT}/tools/desktop-packages.list}"
CA_SRC="${LP_APT_CA:-/root/.ccr/ca-bundle.crt}"

log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ $EUID -eq 0 ]]         || die "root 로 실행해야 합니다 (chroot, mount)"
[[ -x "$DEB/usr/bin/apt-get" ]] || die "데비안 베이스가 없습니다: $DEB"
[[ -f "$LIST" ]]          || die "목록이 없습니다: $LIST"

# The list, without comments or blank lines.
mapfile -t WANT < <(sed -e 's/#.*//' -e 's/[[:space:]]//g' "$LIST" | grep -v '^$')
(( ${#WANT[@]} > 0 )) || die "목록이 비어 있습니다"

# ── outside the namespace: measure, then go in ───────────────────────
if [[ "${1:-}" != "--inside" ]]; then
    step "베이스: $DEB"
    size_of() { du -sxm "$DEB" 2>/dev/null | cut -f1; }
    dpkg_mb() {
        chroot "$DEB" /usr/bin/dpkg-query -W -f='${Installed-Size}\n' \
            | awk '{ s += $1 } END { printf "%d", s / 1024 }'
    }
    BEFORE_DU=$(size_of); BEFORE_DPKG=$(dpkg_mb)
    BEFORE_N=$(chroot "$DEB" /usr/bin/dpkg-query -W -f='${Status}\n' | grep -c '^install ok installed')
    log "${BEFORE_DU}MB 디스크, 패키지 ${BEFORE_N}개 (dpkg 기준 ${BEFORE_DPKG}MB)"

    unshare -m --propagation private "$0" --inside

    step "크기 변화"
    AFTER_DU=$(size_of); AFTER_DPKG=$(dpkg_mb)
    AFTER_N=$(chroot "$DEB" /usr/bin/dpkg-query -W -f='${Status}\n' | grep -c '^install ok installed')
    log "디스크   ${BEFORE_DU}MB -> ${AFTER_DU}MB  ($(( AFTER_DU - BEFORE_DU ))MB)"
    log "dpkg     ${BEFORE_DPKG}MB -> ${AFTER_DPKG}MB  ($(( AFTER_DPKG - BEFORE_DPKG ))MB)"
    log "패키지   ${BEFORE_N} -> ${AFTER_N}  (+$(( AFTER_N - BEFORE_N )))"
    exit 0
fi

# ── inside the private namespace ─────────────────────────────────────
POLICY="$DEB/usr/sbin/policy-rc.d"
CA_DST="$DEB/tmp/build-ca.crt"
cleanup() {
    rm -f "$POLICY" "$CA_DST"
    # The namespace dies with this process and takes the mounts with it;
    # unmounting anyway keeps the order right if anything is still busy.
    for m in bin dev/pts dev sys proc; do umount -l "$DEB/$m" 2>/dev/null || true; done
}
trap cleanup EXIT

for m in proc sys dev dev/pts; do
    mkdir -p "$DEB/$m"
    mount --bind "/$m" "$DEB/$m"
done

# Debian's own files that are physically in our /bin.
#
# Packages installed before this script existed ran with the real /bin,
# so what they ship under /bin went there: udevadm, systemd-hwdb, fuser,
# fusermount. Once /usr/bin is mounted over /bin they are invisible, and
# the first run found out the hard way - udev's trigger called
# systemd-hwdb, got "not found", and the hardware database was not
# rebuilt. A copy in /usr/bin is where a stock merged-/usr system keeps
# them anyway; our own commands are never copied, only files dpkg says
# a package owns.
for f in "$DEB"/bin/*; do
    n=${f##*/}
    [[ -f "$f" && ! -L "$f" && ! -e "$DEB/usr/bin/$n" ]] || continue
    chroot "$DEB" /usr/bin/dpkg-query -S "/bin/$n" >/dev/null 2>&1 || continue
    cp -a "$f" "$DEB/usr/bin/$n"
    log "/bin/$n -> /usr/bin/$n (데비안 파일)"
done

mount --bind "$DEB/usr/bin" "$DEB/bin"
[[ "$(readlink -f "$DEB/usr/bin/sh")" == */dash ]] || die "베이스의 /usr/bin/sh 가 dash 가 아닙니다"

# Our files at paths a package on the list ships.
#
# /etc/services was init's list of supervised programs until it moved to
# /etc/lp/services; netbase ships the real /etc/services (the port table
# glibc reads). A file already at a conffile's path when its package is
# first installed is kept by --force-confold, so without this netbase
# would install around our list and getservbyname("ssh") would keep
# failing on the finished image. It is moved, not deleted: it is the
# base's copy of init's list, and /etc/lp/services is where init reads
# it now.
if [[ -f "$DEB/etc/services" ]] && \
   ! chroot "$DEB" /usr/bin/dpkg-query -S /etc/services >/dev/null 2>&1 && \
   head -1 "$DEB/etc/services" | grep -q 'programs init supervises'; then
    mkdir -p "$DEB/etc/lp"
    mv -f "$DEB/etc/services" "$DEB/etc/lp/services"
    log "/etc/services (init 의 목록) -> /etc/lp/services"
fi

printf '#!/bin/sh\n# apply-packages.sh: no daemons during a chroot install.\nexit 101\n' > "$POLICY"
chmod 755 "$POLICY"

APT_OPTS=(-o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold)
if [[ -n "${HTTPS_PROXY:-}" ]]; then
    APT_OPTS+=(-o "Acquire::http::Proxy=${HTTPS_PROXY}"
               -o "Acquire::https::Proxy=${HTTPS_PROXY}")
    if [[ -f "$CA_SRC" ]]; then
        cp "$CA_SRC" "$CA_DST"
        APT_OPTS+=(-o "Acquire::https::CAInfo=/tmp/build-ca.crt")
    fi
    log "프록시: ${HTTPS_PROXY} (이번 실행에만, 파일에는 남기지 않음)"
fi

in_base() {
    chroot "$DEB" /usr/bin/env -i \
        PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LANG=C.UTF-8 \
        DEBIAN_FRONTEND=noninteractive "$@"
}

# Firmware and the graphics drivers from bookworm-backports, and nothing
# else from there.
#
# bookworm's own firmware-nonfree is from February 2023: no Wi-Fi 7
# (Intel BE200), no MediaTek MT7925, no Meteor Lake or Lunar Lake sound
# and graphics blobs, none for the newest Radeons. Backports carries a
# current linux-firmware in the same packages.
#
# bookworm's Mesa is 22.3, which knows nothing of the xe kernel driver:
# on the Intel graphics xe drives (Lunar Lake, Arc B-series) there was no
# OpenGL at all, and the desktop fell back to drawing on the CPU. Its
# RADV and radeonsi also predate the newest Radeons. Backports has Mesa
# 25 and the libdrm it needs; the two source packages are pinned, and
# their binaries (libgl1-mesa-dri, libgbm1, mesa-vulkan-drivers, ...)
# come from there.
#
# The pin keeps every other package on bookworm: backports is priority
# 100 already (never chosen over bookworm's version), and what is named
# here is raised to bookworm's 500, where the newer version wins. The
# image keeps both files, so the Software app's updates keep firmware
# and Mesa current too. Rewritten on every run, so a base made before a
# line was added here gets it.
BPO_LIST="$DEB/etc/apt/sources.list.d/lp-firmware-backports.list"
BPO_PIN="$DEB/etc/apt/preferences.d/lp-firmware-backports"
printf '%s\n' \
    "# LP: firmware and Mesa from bookworm-backports (tools/apply-packages.sh)." \
    "deb https://deb.debian.org/debian bookworm-backports main non-free-firmware" \
    > "$BPO_LIST"
printf '%s\n' \
    "# LP: only firmware-* and Mesa come from backports (tools/apply-packages.sh)." \
    "Package: *" "Pin: release n=bookworm-backports" "Pin-Priority: 100" "" \
    "Package: firmware-*" "Pin: release n=bookworm-backports" "Pin-Priority: 500" "" \
    "Package: src:mesa src:libdrm" "Pin: release n=bookworm-backports" "Pin-Priority: 500" \
    > "$BPO_PIN"
log "bookworm-backports (firmware-*, Mesa, libdrm)"

step "apt-get update"
in_base apt-get "${APT_OPTS[@]}" -q update 2>&1 | tail -4

# What the archive does not have is reported, not guessed at. A name
# that vanished from bookworm has to be replaced by a person deciding
# what replaces it.
step "목록 확인 (${#WANT[@]}개)"
HAVE=() MISSING=() NEW=()
for p in "${WANT[@]}"; do
    cand=$(in_base apt-cache policy "$p" 2>/dev/null | awk '/Candidate:/ { print $2 }')
    if [[ -z "$cand" || "$cand" == "(none)" ]]; then
        MISSING+=("$p")
        continue
    fi
    HAVE+=("$p")
    st=$(in_base dpkg-query -W -f='${Status}' "$p" 2>/dev/null || true)
    [[ "$st" == "install ok installed" ]] || NEW+=("$p")
done
log "이미 있음 $(( ${#HAVE[@]} - ${#NEW[@]} ))개, 새로 ${#NEW[@]}개, 저장소에 없음 ${#MISSING[@]}개"
(( ${#NEW[@]} ))     && log "새로: ${NEW[*]}"
(( ${#MISSING[@]} )) && log "없음: ${MISSING[*]}"

if (( ${#NEW[@]} )); then
    step "설치"
    # The whole transcript goes to a log beside the other build output;
    # the screen gets the end of it, which is where apt says what went
    # wrong when something did.
    APT_LOG="${LPZERO_WORK}/apply-packages.log"
    rc=0
    in_base apt-get "${APT_OPTS[@]}" -y -q --no-install-recommends \
        install "${HAVE[@]}" > "$APT_LOG" 2>&1 || rc=$?
    grep -Ev '^(Get:|Selecting|Preparing|Unpacking|Setting up|Processing|\(Reading)' \
        "$APT_LOG" | tail -25 | sed 's/^/    /' || true
    log "전체 기록: $APT_LOG (apt-get 종료 코드 $rc)"
fi

# What is installed already and now has a newer pinned version in
# backports (Mesa and libdrm the first time, firmware as it moves on):
# installing only what is missing would leave the old ones in place.
step "backports 로 올릴 것"
UP=()
while read -r name _; do
    [[ -n "$name" ]] && UP+=("${name%%/*}")
done < <(in_base apt list --upgradable 2>/dev/null | grep -- '-backports' || true)
if (( ${#UP[@]} )); then
    log "올림: ${UP[*]}"
    rc=0
    in_base apt-get "${APT_OPTS[@]}" -y -q --no-install-recommends \
        install "${UP[@]}" > "${LPZERO_WORK}/apply-packages-bpo.log" 2>&1 || rc=$?
    tail -3 "${LPZERO_WORK}/apply-packages-bpo.log" | sed 's/^/    /'
    (( rc == 0 )) || die "apt-get install (backports) 실패: ${LPZERO_WORK}/apply-packages-bpo.log"
else
    log "없음"
fi

# Everything on the list is marked manual, so a later autoremove cannot
# take out something the list asked for because nothing else needs it.
in_base apt-mark manual "${HAVE[@]}" >/dev/null

step "확인"
FAILED=()
for p in "${HAVE[@]}"; do
    st=$(in_base dpkg-query -W -f='${Status}' "$p" 2>/dev/null || true)
    [[ "$st" == "install ok installed" ]] || FAILED+=("$p")
done
if (( ${#FAILED[@]} )); then
    log "설치되지 않음: ${FAILED[*]}"
else
    log "목록의 ${#HAVE[@]}개 전부 설치됨"
fi
BROKEN=$(in_base dpkg --audit 2>/dev/null | head -5 || true)
[[ -z "$BROKEN" ]] || { log "dpkg --audit:"; printf '%s\n' "$BROKEN" | sed 's/^/    /'; }

# The downloaded .debs and the package lists are this build's, not the
# machine's: the lists are stale the day the image ships and a device
# runs `apt update` before it installs anything anyway.
in_base apt-get clean
rm -rf "$DEB"/var/lib/apt/lists/*
mkdir -p "$DEB/var/lib/apt/lists/partial"
log "apt 캐시와 목록 비움"

(( ${#FAILED[@]} == 0 )) || exit 1
