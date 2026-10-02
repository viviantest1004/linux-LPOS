#!/usr/bin/env bash
#
# mkdeb.sh - package this OS's own userland as lp-base_<version>_<arch>.deb
#
# The desktop is a Debian base with our init, shell and two hundred
# commands on top. Copying those over the base worked until the first
# `apt upgrade`: dpkg believes it owns /bin/ls, /bin/cp, /bin/sh and a
# hundred other paths, and it writes GNU's files back over ours without
# asking - or, where the copy had replaced /bin wholesale, it finds
# /bin/bash missing and every `#!/bin/bash` script on the machine fails.
#
# A package is the only arrangement dpkg respects. lp-base:
#
#   - ships /bin/<ours> and /sbin/init, and for every one that another
#     package also ships, adds a dpkg diversion first, so Debian's copy
#     goes to /usr/lib/lp-base/debian/<path> (upgrades land there) and
#     /usr/bin/<name> points at it - GNU ls stays at /usr/bin/ls;
#   - puts back the Debian files an earlier copy displaced (fixup.sh),
#     so /bin/bash exists again and `dpkg --verify` is clean;
#   - ships our shell as /bin/lpsh and leaves /bin/sh to dash, which is
#     what every maintainer script and system() call in Debian expects;
#   - puts init's service list at /etc/lp/services, because /etc/services
#     is netbase's port table that glibc reads;
#   - tells apt to run dpkg with /usr/bin first (DPkg::Path), so Debian's
#     maintainer scripts get the tools they were written against;
#   - is Protected: apt will not remove it without being told twice,
#     because removing it removes the machine's init.
#
#   ./tools/mkdeb.sh [amd64]      -> dist/debs/lp-base_<ver>_amd64.deb
#
# Needs: userland built and userland/rootfs-<arch> assembled
# (make -C userland ARCH=amd64 && userland/mkrootfs.sh amd64), dpkg-deb.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
source "${REPO_ROOT}/tools/common.sh"

ARCH="${1:-amd64}"
case "$ARCH" in amd64|arm64|armhf) ;; *) echo "error: arch amd64, arm64 or armhf" >&2; exit 1 ;; esac
SRC_ARCH="$ARCH"; [[ "$ARCH" == armhf ]] && SRC_ARCH=armv6
SRC="${LP_DEB_SRC:-${REPO_ROOT}/userland/rootfs-${SRC_ARCH}}"
OUT_DIR="${REPO_ROOT}/dist/debs"
STAGE="${LPZERO_WORK}/debs/lp-base-${ARCH}"
FIXUP="${REPO_ROOT}/tools/lp-base/fixup.sh"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
[[ -x "$SRC/bin/init" && -x "$SRC/bin/sh" ]] ||
    die "no userland in $SRC - run: make -C userland ARCH=${SRC_ARCH} && userland/mkrootfs.sh ${SRC_ARCH}"
command -v dpkg-deb >/dev/null || die "dpkg-deb is missing (apt install dpkg)"
[[ -f "$FIXUP" ]] || die "missing $FIXUP"

# Version: 1.<commits>, so every commit sorts after the one before and
# `apt upgrade` sees it. A dirty tree gets +dirty.<time> so two local
# builds of the same commit are still told apart.
COUNT=$(git -C "$REPO_ROOT" rev-list --count HEAD 2>/dev/null || echo 0)
VER="1.${COUNT}"
if [[ -n "$(git -C "$REPO_ROOT" status --porcelain -- userland tools/lp-base 2>/dev/null)" ]]; then
    VER="${VER}+dirty.$(date -u +%Y%m%d%H%M)"
fi
VER="${LP_VERSION:-$VER}"

rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" "$STAGE/bin" "$STAGE/sbin" "$STAGE/etc/lp" \
         "$STAGE/etc/apt/apt.conf.d" "$STAGE/usr/lib/lp-base" \
         "$STAGE/usr/share/doc/lp-base" "$STAGE/data"

# ── /bin ─────────────────────────────────────────────────────────────
cp -a "$SRC/bin/." "$STAGE/bin/"
# Two commands are the boards' and would be wrong here, so they stay out:
#   apt     runs Debian inside a directory on /data, for a system too
#           small to carry one. This one IS Debian, and /bin/apt would
#           hide the real apt in /usr/bin behind a chroot of a chroot.
#   update  replaces the one-file RAM system image. The desktop is
#           updated by apt, lp-base included; writing a board image
#           over this machine's ESP would be the worst thing it could do.
for p in apt update; do rm -f "$STAGE/bin/$p"; done
# Our shell is /bin/lpsh here; /bin/sh stays dash's. mkrootfs makes
# lpsh a hard link of sh - if an older tree lacks it, make it.
[[ -e "$STAGE/bin/lpsh" ]] || cp -a "$STAGE/bin/sh" "$STAGE/bin/lpsh"
rm -f "$STAGE/bin/sh"
# An alias pointing at "sh" would now point at dash. None do today;
# if one ever does, it should mean our shell.
while IFS= read -r l; do
    ln -sfn lpsh "$l"
done < <(find "$STAGE/bin" -maxdepth 1 -type l -lname sh)
# Absolute symlinks would point into the build host.
if find "$STAGE/bin" -maxdepth 1 -type l -lname '/*' | grep -q .; then
    die "absolute symlinks in $SRC/bin: $(find "$STAGE/bin" -maxdepth 1 -type l -lname '/*' | xargs -n1 basename | tr '\n' ' ')"
fi

# ── /sbin/init ───────────────────────────────────────────────────────
# preinit looks for /sbin/init first, and in the Debian base that is
# systemd-sysv's. It is diverted like everything else in /bin.
cp -a "$SRC/bin/init" "$STAGE/sbin/init"

# ── /etc ─────────────────────────────────────────────────────────────
# Only files that are this OS's configuration. passwd, group, shadow,
# hostname, hosts, profile and motd are Debian's or the installer's -
# a package that shipped them would fight base-passwd and the person.
CONFFILES=()
etc() {  # etc <source name> <installed path>
    [[ -f "$SRC/etc/$1" ]] || return 0
    mkdir -p "$STAGE$(dirname "$2")"
    cp -a "$SRC/etc/$1" "$STAGE$2"
    CONFFILES+=("$2")
}
etc services       /etc/lp/services
etc rc             /etc/rc
etc wpa-start      /etc/wpa-start
etc firewall.conf  /etc/firewall.conf
etc beacon.conf    /etc/beacon.conf
etc osname         /etc/osname
etc update-key.pub /etc/update-key.pub
# Not a setting: the clock floor ntp uses before the network is up.
# Replaced by every version, so not a conffile.
[[ -f "$SRC/etc/build-epoch" ]] && cp -a "$SRC/etc/build-epoch" "$STAGE/etc/build-epoch"

cat > "$STAGE/etc/apt/apt.conf.d/00lp-base" <<'EOF'
// lp-base: Debian's maintainer scripts run with /usr/bin first, so they
// get the GNU tools they were written against (Debian's copies of what
// LP replaces in /bin are at /usr/bin/<name>). A person at the prompt
// has /bin first and gets LP's.
DPkg::Path "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
// After every dpkg run, put back anything in /bin that dpkg's record and
// the disk disagree about. Cheap (one pass over dpkg's lists) and quiet.
DPkg::Post-Invoke { "if [ -x /usr/lib/lp-base/fixup ]; then /usr/lib/lp-base/fixup --quiet || true; fi"; };
EOF
CONFFILES+=("/etc/apt/apt.conf.d/00lp-base")

# ── the maintainer scripts ───────────────────────────────────────────
# Every path we ship that another package might own: all of /bin, and
# /sbin/init. preinst diverts whichever of them dpkg says are owned.
SHIPS=$( (cd "$STAGE" && find bin -maxdepth 1 \( -type f -o -type l \) | sort | sed 's|^|/|'; echo /sbin/init) | tr '\n' ' ')

# dash, by absolute path: this code is what makes /bin/sh dash again,
# so it cannot rely on /bin/sh. /usr/bin/dash exists on a merged base,
# on the half-merged desktop root, and (as a compat link) after fixup.
script() {  # script <name> <body>
    {
        echo '#!/usr/bin/dash'
        echo "# lp-base $1 - generated by tools/mkdeb.sh from tools/lp-base/fixup.sh"
        echo 'set -e'
        cat "$FIXUP"
        echo
        echo "LPB_SHIPS='$SHIPS'"
        echo
        printf '%s\n' "$2"
    } > "$STAGE/DEBIAN/$1"
    chmod 0755 "$STAGE/DEBIAN/$1"
}

script preinst '
case "$1" in
install|upgrade)
    lpb_index
    trap lpb_done EXIT
    lpb_repair_bin
    for p in $LPB_SHIPS; do lpb_divert "$p"; done
    ;;
esac
exit 0'

script postinst '
case "$1" in
configure)
    lpb_index
    trap lpb_done EXIT
    lpb_repair_bin
    # Diversions for paths this version no longer ships: Debian gets
    # its file back.
    for p in $(lpb_our_diversions); do
        case " $LPB_SHIPS " in *" $p "*) ;; *) lpb_undivert "$p" ;; esac
    done
    lpb_compat_links
    if command -v add-shell >/dev/null 2>&1; then add-shell /bin/lpsh; fi
    ;;
esac
exit 0'

script postrm '
case "$1" in
remove|abort-install)
    lpb_index
    trap lpb_done EXIT
    for p in $(lpb_our_diversions); do lpb_undivert "$p"; done
    if command -v remove-shell >/dev/null 2>&1; then remove-shell /bin/lpsh; fi
    ;;
purge)
    rm -rf /usr/lib/lp-base /var/lib/lp-base
    ;;
esac
exit 0'

# The same repair, runnable by hand and from apt's Post-Invoke.
{
    echo '#!/usr/bin/dash'
    echo '# /usr/lib/lp-base/fixup - put /bin back the way dpkg recorded it.'
    echo '# Generated by tools/mkdeb.sh from tools/lp-base/fixup.sh.'
    echo 'set -e'
    echo 'case "${1:-}" in --quiet) LPB_QUIET=1 ;; esac'
    cat "$FIXUP"
    echo
    echo '[ "$(id -u)" = 0 ] || { echo "lp-base fixup: run as root" >&2; exit 1; }'
    echo 'lpb_index'
    echo 'trap lpb_done EXIT'
    echo 'lpb_repair_bin'
    echo 'lpb_compat_links'
} > "$STAGE/usr/lib/lp-base/fixup"
chmod 0755 "$STAGE/usr/lib/lp-base/fixup"

cat > "$STAGE/usr/share/doc/lp-base/copyright" <<EOF
lp-base: the init, shell and commands of LP, written from scratch.
See LICENSE in the source repository.
EOF

printf '%s\n' "${CONFFILES[@]}" > "$STAGE/DEBIAN/conffiles"

# sudo, su and passwd are setuid root in the package itself, so that
# dpkg sets the mode on every install and upgrade: a mode given only
# by the image build would be reset to 0755 by the first
# `apt install --reinstall lp-base`, and nobody could use sudo again.
# (--root-owner-group makes them root:root.)
for p in sudo su passwd; do
    [ -f "$STAGE/bin/$p" ] && [ ! -L "$STAGE/bin/$p" ] && chmod 4755 "$STAGE/bin/$p"
done

SIZE_KB=$(du -sk --exclude=DEBIAN "$STAGE" | cut -f1)
cat > "$STAGE/DEBIAN/control" <<EOF
Package: lp-base
Version: ${VER}
Architecture: ${ARCH}
Maintainer: LP <lp@localhost>
Installed-Size: ${SIZE_KB}
Pre-Depends: dpkg (>= 1.20.0), dash, mawk | awk
Depends: netbase, debianutils
Section: admin
Priority: required
Protected: yes
Description: LP's own init, shell and commands
 The userland of LP, written from scratch: init and its service
 supervisor, the lpsh shell, and about two hundred commands. Debian's
 versions of the commands LP replaces stay installed, diverted to
 /usr/lib/lp-base/debian and reachable at /usr/bin/<name>; /bin/sh
 remains dash.
EOF

(cd "$STAGE" && find . -path ./DEBIAN -prune -o -type f -print0 | sort -z |
    xargs -0 md5sum | sed 's|  \./|  |') > "$STAGE/DEBIAN/md5sums"

mkdir -p "$OUT_DIR"
OUT="${OUT_DIR}/lp-base_${VER}_${ARCH}.deb"
rm -f "${OUT_DIR}"/lp-base_*_"${ARCH}".deb
dpkg-deb --root-owner-group -Zxz --build "$STAGE" "$OUT" >/dev/null
printf 'lp-base %s: %s (%s KiB installed, %d paths to divert-check, %d conffiles)\n' \
    "$VER" "$OUT" "$SIZE_KB" "$(wc -w <<<"$SHIPS")" "${#CONFFILES[@]}"
