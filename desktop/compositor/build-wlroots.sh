#!/bin/bash
# build-wlroots.sh - Debian's wlroots 0.15.1-6 + wlroots-0.15.1-lp.patch,
# built as a drop-in libwlroots.so.10 (see README-wlroots.md).
#
#   desktop/compositor/build-wlroots.sh        -> desktop/compositor/libwlroots.so.10
#
# The build runs in a throwaway overlay of the Debian base tree ($DEB, the
# image's own root: same compilers, same libdrm/pixman/wayland as the machine
# runs) inside a private mount namespace, so nothing is ever written to the
# base and every mount disappears with the namespace when the build ends.
# The build dependencies are apt-installed into the overlay's upper layer,
# which is kept in $WORK/ovl as a cache (CLEAN=1 starts over).
#
# Needs root (mount/chroot), and HTTPS to deb.debian.org (directly or
# through https_proxy; a TLS-intercepting proxy's CA can be given in
# CA_BUNDLE, it is used for apt only - signatures are still checked).
#
# Environment:
#   DEB        Debian base tree            (/home/user/kernel-work/deb)
#   WORK       scratch dir, big + fast     (/dev/shm/wlr-build)
#   CA_BUNDLE  extra CA for apt over HTTPS (/root/.ccr/ca-bundle.crt if present)
#   CLEAN=1    drop the cached overlay and sources first
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DEB=${DEB:-/home/user/kernel-work/deb}
WORK=${WORK:-/dev/shm/wlr-build}
CA_BUNDLE=${CA_BUNDLE:-/root/.ccr/ca-bundle.crt}
POOL=https://deb.debian.org/debian/pool/main/w/wlroots
VER=0.15.1
DEBREV=6
PATCH=$HERE/wlroots-$VER-lp.patch
OUT=$HERE/libwlroots.so.10

# From wlroots_0.15.1-6.dsc (Checksums-Sha256).
declare -A SHA256=(
  [wlroots_$VER.orig.tar.xz]=e55b07264339dc258afbf950b36da932d510caf6e9721d500f9a94d27ce2f14d
  [wlroots_$VER-$DEBREV.debian.tar.xz]=797afc194ec8c2777b975ef54fe141a248fe44a1de650e724428b68ec2603d43
)

# The build dependencies from debian/control that the library needs
# (libav*, libpng, libcap and libsystemd are for the examples, not built).
BUILD_DEPS="meson ninja-build pkg-config dpkg-dev libdrm-dev libgbm-dev
  libegl1-mesa-dev libgles2-mesa-dev libinput-dev libpixman-1-dev libseat-dev
  libudev-dev libwayland-dev wayland-protocols libxkbcommon-dev xwayland
  libxcb1-dev libx11-xcb-dev libxcb-composite0-dev libxcb-dri3-dev
  libxcb-icccm4-dev libxcb-image0-dev libxcb-present-dev libxcb-render0-dev
  libxcb-render-util0-dev libxcb-res0-dev libxcb-shm0-dev libxcb-xfixes0-dev
  libxcb-xinput-dev"

[ "$(id -u)" = 0 ] || { echo "build-wlroots.sh: needs root (mount, chroot)" >&2; exit 1; }
[ -x "$DEB/usr/bin/gcc" ] || { echo "build-wlroots.sh: no Debian base at $DEB" >&2; exit 1; }
[ -f "$PATCH" ] || { echo "build-wlroots.sh: missing $PATCH" >&2; exit 1; }

if [ "${CLEAN:-0}" = 1 ]; then
    rm -rf "$WORK/ovl" "$WORK/build"
fi
mkdir -p "$WORK/dl" "$WORK/ovl/up" "$WORK/ovl/wk" "$WORK/ovl/root"

# ── sources: Debian's, checked against the .dsc ──
for f in "${!SHA256[@]}"; do
    if ! echo "${SHA256[$f]}  $WORK/dl/$f" | sha256sum -c --quiet 2>/dev/null; then
        curl -fsSL ${CA_BUNDLE:+--cacert "$CA_BUNDLE"} -o "$WORK/dl/$f" "$POOL/$f"
        echo "${SHA256[$f]}  $WORK/dl/$f" | sha256sum -c --quiet
    fi
done

SRC=$WORK/build/wlroots-$VER
rm -rf "$WORK/build"
mkdir -p "$WORK/build"
tar -C "$WORK/build" -xf "$WORK/dl/wlroots_$VER.orig.tar.xz"
tar -C "$SRC" -xf "$WORK/dl/wlroots_$VER-$DEBREV.debian.tar.xz"
# Debian's patches in series order (what dpkg-source -x does), then ours.
while read -r p _; do
    case $p in ''|'#'*) continue ;; esac
    patch -d "$SRC" -p1 -s --no-backup-if-mismatch < "$SRC/debian/patches/$p"
done < "$SRC/debian/patches/series"
patch -d "$SRC" -p1 -s --no-backup-if-mismatch < "$PATCH"

# ── the build, in the overlay ──
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
cd /build/wlroots-0.15.1
# debian/rules: DEB_BUILD_MAINT_OPTIONS=hardening=+all, dh_auto_configure
# (meson, buildtype plain, multiarch libdir) -- -Dbackends=drm,libinput,x11.
# Pinned here to what Debian's build ends up with: gles2 only (no vulkan in
# its build-deps; the base has libvulkan-dev), Xwayland on, no xcb-errors.
eval "$(DEB_BUILD_MAINT_OPTIONS=hardening=+all dpkg-buildflags --export=sh)"
meson setup obj --wrap-mode=nodownload --buildtype=plain --prefix=/usr \
    --sysconfdir=/etc --localstatedir=/var --libdir=lib/x86_64-linux-gnu \
    -Dbackends=drm,libinput,x11 -Drenderers=gles2 -Dxwayland=enabled \
    -Dxcb-errors=disabled -Dexamples=false
ninja -C obj
cp obj/libwlroots.so.10 /build/libwlroots.so.10
# dh_strip
strip --remove-section=.comment --remove-section=.note --strip-unneeded /build/libwlroots.so.10
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
# The base's /bin/sh is LP's own shell (no set -u, and not what Debian's
# maintainer scripts are written for): dash stands in for it here.
mount --bind $R/usr/bin/dash $R/bin/sh
cp /etc/resolv.conf $R/etc/resolv.conf
mkdir -p $R/etc/ssl
if [ -f '$CA_BUNDLE' ]; then cp '$CA_BUNDLE' $R/etc/ssl/lp-build-ca.crt; fi
chroot $R /usr/bin/env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin HOME=/root \
    https_proxy='${https_proxy:-${HTTPS_PROXY:-}}' HTTPS_PROXY='${HTTPS_PROXY:-${https_proxy:-}}' \
    BUILD_DEPS='$(echo $BUILD_DEPS)' /usr/bin/dash /build/inside.sh
"

# ── same ABI as Debian's library? ──
ORIG=$DEB/usr/lib/x86_64-linux-gnu/libwlroots.so.10
nm -D --defined-only "$ORIG" | awk '{print $2, $3}' | sort > "$WORK/build/syms.orig"
nm -D --defined-only "$WORK/build/libwlroots.so.10" | awk '{print $2, $3}' | sort > "$WORK/build/syms.new"
if ! diff -u "$WORK/build/syms.orig" "$WORK/build/syms.new"; then
    echo "build-wlroots.sh: exported symbols differ from $ORIG" >&2
    exit 1
fi
objdump -p "$ORIG" | awk '/NEEDED|SONAME/ {print $1, $2}' > "$WORK/build/needed.orig"
objdump -p "$WORK/build/libwlroots.so.10" | awk '/NEEDED|SONAME/ {print $1, $2}' > "$WORK/build/needed.new"
if ! diff -u "$WORK/build/needed.orig" "$WORK/build/needed.new"; then
    echo "build-wlroots.sh: NEEDED/SONAME differ from $ORIG" >&2
    exit 1
fi

install -m 644 "$WORK/build/libwlroots.so.10" "$OUT"
echo "build-wlroots.sh: $OUT ($(wc -l < "$WORK/build/syms.new") exported symbols, same as Debian's)"
sha256sum "$OUT"
