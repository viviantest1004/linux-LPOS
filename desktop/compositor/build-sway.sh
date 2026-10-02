#!/bin/bash
# build-sway.sh - Debian's sway 1.7-6 + sway-1.7-lp.patch (README.md).
#
#   desktop/compositor/build-sway.sh        -> desktop/compositor/sway
#
# Same arrangement as build-wlroots.sh: the build runs in a throwaway
# overlay of the Debian base tree ($DEB, the image's own root) inside a
# private mount namespace, the build dependencies are apt-installed into
# the overlay's upper layer (kept in $WORK/ovl as a cache, CLEAN=1 starts
# over), and nothing is written to the base.
#
# Environment:
#   DEB        Debian base tree            (/home/user/kernel-work/deb)
#   WORK       scratch dir, big + fast     (/dev/shm/sway-build)
#   CA_BUNDLE  extra CA for apt over HTTPS (/root/.ccr/ca-bundle.crt if present)
#   CLEAN=1    drop the cached overlay and sources first
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DEB=${DEB:-/home/user/kernel-work/deb}
WORK=${WORK:-/dev/shm/sway-build}
CA_BUNDLE=${CA_BUNDLE:-/root/.ccr/ca-bundle.crt}
POOL=https://deb.debian.org/debian/pool/main/s/sway
VER=1.7
DEBREV=6
PATCH=$HERE/sway-$VER-lp.patch
OUT=$HERE/sway

BUILD_DEPS="meson ninja-build pkg-config dpkg-dev libwlroots-dev libjson-c-dev
  libpcre2-dev libpango1.0-dev libcairo2-dev libgdk-pixbuf-2.0-dev libinput-dev
  libxkbcommon-dev libwayland-dev wayland-protocols libevdev-dev libdrm-dev
  libpixman-1-dev libsystemd-dev libxcb1-dev libxcb-icccm4-dev"

[ "$(id -u)" = 0 ] || { echo "build-sway.sh: needs root (mount, chroot)" >&2; exit 1; }
[ -x "$DEB/usr/bin/gcc" ] || [ -d "$DEB/usr" ] || { echo "build-sway.sh: no Debian base at $DEB" >&2; exit 1; }
[ -f "$PATCH" ] || { echo "build-sway.sh: missing $PATCH" >&2; exit 1; }

if [ "${CLEAN:-0}" = 1 ]; then
    rm -rf "$WORK/ovl" "$WORK/build"
fi
mkdir -p "$WORK/dl" "$WORK/ovl/up" "$WORK/ovl/wk" "$WORK/ovl/root"

# ── sources: Debian's, checked against the .dsc's own sums ──
for f in sway_$VER-$DEBREV.dsc sway_$VER.orig.tar.gz sway_$VER-$DEBREV.debian.tar.xz; do
    [ -s "$WORK/dl/$f" ] || curl -fsSL ${CA_BUNDLE:+--cacert "$CA_BUNDLE"} -o "$WORK/dl/$f" "$POOL/$f"
done
(cd "$WORK/dl" && awk '/^Checksums-Sha256:/ {s=1; next} /^[A-Z]/ {s=0} s && NF==3 {print $1 "  " $3}' \
    sway_$VER-$DEBREV.dsc | sha256sum -c --quiet)

SRC=$WORK/build/sway-$VER
rm -rf "$WORK/build"
mkdir -p "$WORK/build"
tar -C "$WORK/build" -xf "$WORK/dl/sway_$VER.orig.tar.gz"
tar -C "$SRC" -xf "$WORK/dl/sway_$VER-$DEBREV.debian.tar.xz"
while read -r p _; do
    case $p in ''|'#'*) continue ;; esac
    patch -d "$SRC" -p1 -s --no-backup-if-mismatch < "$SRC/debian/patches/$p"
done < "$SRC/debian/patches/series"
patch -d "$SRC" -p1 -s --no-backup-if-mismatch < "$PATCH"

cat > "$WORK/build/inside.sh" <<'INSIDE'
#!/bin/sh
set -eu
export DEBIAN_FRONTEND=noninteractive LC_ALL=C.UTF-8
APT_OPTS="-o Acquire::Retries=3"
[ -f /etc/ssl/lp-build-ca.crt ] && APT_OPTS="$APT_OPTS -o Acquire::https::CAInfo=/etc/ssl/lp-build-ca.crt"
missing=
for p in $BUILD_DEPS; do
    dpkg-query -W -f='${Status}\n' "$p" 2>/dev/null | grep -q '^install ok installed' || missing="$missing $p"
done
if [ -n "$missing" ]; then
    apt-get $APT_OPTS update -qq
    apt-get $APT_OPTS install -y -qq --no-install-recommends $missing
fi
cd /build
eval "$(DEB_BUILD_MAINT_OPTIONS=hardening=+all dpkg-buildflags --export=sh)"
meson setup b sway-1.7 --wrap-mode=nodownload --buildtype=plain --prefix=/usr \
    --sysconfdir=/etc --localstatedir=/var --libdir=lib/x86_64-linux-gnu \
    -Dman-pages=disabled -Dtray=disabled
ninja -C b sway/sway
cp b/sway/sway /build/sway.out
strip --remove-section=.comment --remove-section=.note /build/sway.out
INSIDE
chmod +x "$WORK/build/inside.sh"

R=$WORK/ovl/root
unshare -m bash -euc "
mount --make-rprivate /
mount -t overlay overlay -o lowerdir=$DEB,upperdir=$WORK/ovl/up,workdir=$WORK/ovl/wk $R
mount -t proc proc $R/proc
mount --rbind /dev $R/dev
mkdir -p $R/build
mount --bind $WORK/build $R/build
mount --bind $R/usr/bin/dash $R/bin/sh
cp /etc/resolv.conf $R/etc/resolv.conf
mkdir -p $R/etc/ssl
if [ -f '$CA_BUNDLE' ]; then cp '$CA_BUNDLE' $R/etc/ssl/lp-build-ca.crt; fi
chroot $R /usr/bin/env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin HOME=/root \
    https_proxy='${https_proxy:-${HTTPS_PROXY:-}}' HTTPS_PROXY='${HTTPS_PROXY:-${https_proxy:-}}' \
    BUILD_DEPS='$(echo $BUILD_DEPS)' /usr/bin/dash /build/inside.sh
"

# Every library it needs has to be in the base as it ships (runtime
# packages only - the -dev packages above are in the overlay, not there).
missing=$(objdump -p "$WORK/build/sway.out" | awk '/NEEDED/ {print $2}' | while read -r l; do
    [ -e "$DEB/usr/lib/x86_64-linux-gnu/$l" ] || [ -e "$DEB/lib/x86_64-linux-gnu/$l" ] || echo "$l"
done)
if [ -n "$missing" ]; then
    echo "build-sway.sh: not in the base: $missing" >&2
    exit 1
fi
install -m 755 "$WORK/build/sway.out" "$OUT"
echo "build-sway.sh: $OUT"
sha256sum "$OUT"
