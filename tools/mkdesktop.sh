#!/usr/bin/env bash
#
# mkdesktop.sh - the amd64 desktop: our userland on a Debian base, as a
# disk image that installs itself.
#
# The desktop needs two things that cannot both be built the same way.
#
# A Wayland compositor is a hundred thousand lines against glibc, mesa,
# libinput and libxkbcommon. It is not going to be rewritten here and it
# is not going to link against a hand-written libc. So the root carries
# a Debian bookworm base underneath: glibc, GTK, sway, fonts.
#
# But the machine is still this one. Its init is ours, its shell is
# ours, its two hundred commands are ours, and its /etc/rc is what brings
# the system up. That is what the project is; a Debian system with our
# kernel on it would be a Debian system.
#
# What comes out is dist-ready: sdcard/linux-LP_desktop.img, a GPT disk
# (tools/mkdisk.sh) that boots on UEFI from a USB stick into the
# installer, with a persistent read-write root on the stick itself. The
# installed system on the laptop's NVMe is a copy of this root made by
# lp-install - there is no RAM-live system anywhere.
#
#   sudo ./tools/mkdesktop.sh            the image (calls mkdisk.sh)
#   LP_CHECK_ONLY=1 ./tools/mkdesktop.sh assemble and check, no image
#   LP_DEB=/path ./tools/mkdesktop.sh    a Debian base somewhere else
#   LP_EDITION=8g ./tools/mkdesktop.sh   the same installer, for an 8GB
#                                        stick: sdcard/linux-LP_desktop-8g.img
#
# ── The root is assembled in an overlay, not a copy ──
#
# The base is 3GB. Copying it to build on cost 3GB of disk twice over (the
# copy, then the image), and building in it directly - the old
# LP_INPLACE - changed the base every other build uses: the /bin swap,
# the account files, a stale /sbin/init. So the base is the lower layer
# of an overlayfs, read-only; every change this script makes lands in an
# upper directory of a few hundred MB, and mkfs.ext4 -d reads the merged
# view straight into the image. All of it happens in a private mount
# namespace, so nobody else using the base sees the overlay or the bind
# mounts, and the whole thing vanishes when the script ends.
#
# ── Who owns /bin ──
#
# lp-base (tools/mkdeb.sh, dist/debs/lp-base_<ver>_amd64.deb) when it has
# been built: a Debian package, so dpkg knows our commands are ours and
# diverts its own copies instead of writing GNU's over them on the next
# upgrade. It puts our shell at /bin/lpsh and leaves /bin/sh to dash, and
# init's list of services at /etc/lp/services. Without the package, the
# old way - our /bin copied over the base's - with a warning, because
# the first `apt upgrade` of coreutils on such a machine undoes it.
#
# ── /bin/sh ──
#
# Debian's maintainer scripts, apt-key and every system() call run
# /bin/sh, and they were written for a POSIX shell. Ours is being made
# one (userland/sh, tests/sh-posix); until its conformance suite says so,
# /bin/sh is dash, and our shell is every account's login shell
# (/bin/lpsh) - what a person types into is ours either way. When
# tests/sh-posix/gate.status says "pass", /bin/sh becomes lpsh through a
# dpkg diversion, the way Debian itself lets a system choose its /bin/sh.
# LP_BINSH=dash|lpsh overrides the gate.
#
# ── What the image must not carry ──
#
# The build host's proxy and CA, its resolver, apt's package lists, a
# [trusted=yes], a machine-id, SSH host keys, shell histories, logs, the
# base's build scripts. scrub() removes them and leaks() fails the build
# if any is still there - checked, not assumed, because each of these
# once shipped.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
source "${REPO_ROOT}/tools/common.sh"

DEB="${LP_DEB:-${LPZERO_WORK}/deb}"
OURS="${REPO_ROOT}/userland/rootfs-amd64"
WORKDIR="${LP_DESKTOP_WORK:-${LPZERO_WORK}/desktop}"
UPPER="${WORKDIR}/upper"
OVWORK="${WORKDIR}/work"
ROOT="${WORKDIR}/root"
PC_FW="${PC_FW_DIR:-${REPO_ROOT}/blobs/pc-fw}"

log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
warn() { printf '  warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
case "${LP_EDITION:-}" in
    ""|8g) ;;
    *) die "LP_EDITION=${LP_EDITION}: the only edition is 8g" ;;
esac

# ── outside the namespace: preconditions, then go in ─────────────────
if [[ "${1:-}" != "--inside" ]]; then
    [[ $EUID -eq 0 ]] || die "run as root (overlay and chroot)"
    [[ -x "$DEB/usr/bin/dpkg" ]] || die "no Debian base at $DEB (tools/apply-packages.sh)"
    [[ -d "$OURS/bin" ]] || die "no userland at $OURS - make -C userland ARCH=amd64 && userland/mkrootfs.sh"
    grep -qw overlay /proc/filesystems || modprobe overlay 2>/dev/null ||
        die "this kernel has no overlayfs"
    # The image is sparse, but the root inside it is not: the base, the
    # upper layer and the finished image all land on this disk.
    need_mb=$(( $(du -sxm "$DEB" | cut -f1) + 1500 ))
    # With sdcard/ on another filesystem (a tmpfs, a second disk) the image
    # does not land here: what stays is the upper layer and the recovery
    # tree.
    if [[ -d "${REPO_ROOT}/sdcard" && "$(stat -c %d "${REPO_ROOT}/sdcard")" != "$(stat -c %d "$(dirname "$WORKDIR")")" ]]; then
        need_mb=2500
    fi
    have_mb=$(df -Pm "$(dirname "$WORKDIR")" | awk 'NR == 2 { print $4 }')
    (( have_mb > need_mb )) || die "needs about ${need_mb}MB free, there is ${have_mb}MB"
    mkdir -p "$WORKDIR"
    exec unshare -m --propagation private "$0" --inside
fi

# ── inside the private namespace ─────────────────────────────────────
POLICY="$ROOT/usr/sbin/policy-rc.d"
cleanup() {
    for m in mnt/lp-src tmp dev/pts dev sys proc; do
        umount -l "$ROOT/$m" 2>/dev/null || true
    done
    umount -l "$ROOT" 2>/dev/null || true
    # The upper layer is this build's only trace; kept with LP_KEEP_UPPER=1
    # for somebody who wants to see what the build changed.
    [[ "${LP_KEEP_UPPER:-0}" == 1 ]] || rm -rf "$UPPER" "$OVWORK"
}
trap cleanup EXIT

in_root() {
    chroot "$ROOT" /usr/bin/env -i \
        PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        HOME=/root LANG=C.UTF-8 DEBIAN_FRONTEND=noninteractive "$@"
}

step "the root: ${DEB} + an overlay"
rm -rf "$UPPER" "$OVWORK"
mkdir -p "$UPPER" "$OVWORK" "$ROOT"
mount -t overlay overlay \
    -o "lowerdir=${DEB},upperdir=${UPPER},workdir=${OVWORK}" "$ROOT"
for m in proc sys dev dev/pts; do
    mkdir -p "$ROOT/$m"
    mount --bind "/$m" "$ROOT/$m"
done
# Scratch space for the build that cannot end up in the image.
mount -t tmpfs -o mode=1777 tmpfs "$ROOT/tmp"
printf '#!/bin/sh\n# mkdesktop.sh: no daemons during the build.\nexit 101\n' > "$POLICY"
chmod 755 "$POLICY"
log "$(du -sh "$DEB" 2>/dev/null | cut -f1) base, changes go to $UPPER"

# What the base's own history left at its top: the scripts that built it
# (outside the repository - tools/desktop-packages.list replaced them),
# a file apt-key once wrote called "&2", and the home of an account
# ("lp") that was renamed to "user" long ago.
rm -rf "$ROOT/setup.sh" "$ROOT/apps.sh" "$ROOT/&2" "$ROOT/home/lp"

# ── 1. our userland ──────────────────────────────────────────────────
step "our userland"
LPBASE="$(ls -t "${REPO_ROOT}"/dist/debs/lp-base_*_amd64.deb 2>/dev/null | head -1 || true)"
USE_DEB=0
if [[ -n "$LPBASE" ]]; then
    USE_DEB=1
    # A package older than the userland it was made from would put last
    # week's commands on the image and say nothing.
    #
    # /etc counts as much as /bin (the package ships /etc/rc), and so do
    # the freshly built programs in userland/bin-amd64 that mkrootfs.sh has
    # not yet copied into $OURS. Both were missed once: an image built at
    # 17:33 carried lp-base from 12:44, so /etc/rc still ran /data/rc.local
    # through dash ("sh: 0: Illegal option -q") and init still left the
    # root un-remounted at shutdown (a journal replay after every clean
    # reboot) - fixes that were committed, and absent. A warning scrolled
    # past, so this stops the build; LP_ALLOW_STALE_DEB=1 builds anyway.
    stale="$(find "$OURS/bin" "$OURS/etc" "${REPO_ROOT}/userland/bin-amd64" \
                  -newer "$LPBASE" -type f -print -quit 2>/dev/null || true)"
    if [[ -n "$stale" ]]; then
        msg="$(basename "$LPBASE") is older than ${stale#"${REPO_ROOT}"/} - run userland/mkrootfs.sh amd64 && tools/mkdeb.sh amd64"
        [[ "${LP_ALLOW_STALE_DEB:-0}" == 1 ]] && warn "$msg" || die "$msg"
    fi
    # dpkg runs its maintainer scripts through /bin/sh in places; in the
    # base /bin/sh is still our old shell, so dash first, then the package
    # (whose own scripts name /usr/bin/dash).
    ln -sfn /usr/bin/dash "$ROOT/bin/sh"
    cp "$LPBASE" "$ROOT/tmp/lp-base.deb"
    # --force-confnew: the base carries older copies of the package's
    # conffiles (/etc/rc, /etc/lp/services) from before the package
    # existed; the package's are the current ones. On a machine that
    # upgrades lp-base, dpkg asks as usual.
    if in_root dpkg --force-confnew -i /tmp/lp-base.deb > "$ROOT/tmp/dpkg.log" 2>&1 </dev/null; then
        log "lp-base: $(basename "$LPBASE")"
    else
        tail -15 "$ROOT/tmp/dpkg.log" | sed 's/^/    /'
        die "dpkg -i lp-base failed"
    fi
else
    warn "no dist/debs/lp-base_*_amd64.deb - copying our /bin over the base's."
    warn "The first 'apt upgrade' of coreutils will write GNU's files over ours; build the package (tools/mkdeb.sh amd64)."
    [[ -L "$ROOT/bin" ]] && rm -f "$ROOT/bin"
    mkdir -p "$ROOT/bin"
    # --remove-destination: /bin/sh may be a symlink to /usr/bin/dash,
    # and cp follows symlinks - without it, the copy lands on whatever
    # the link points at (it once pointed at the build host's own dash).
    cp -a --remove-destination "$OURS/bin/." "$ROOT/bin/"
    [[ -e "$ROOT/bin/lpsh" ]] || cp -a "$ROOT/bin/sh" "$ROOT/bin/lpsh"
    ln -sfn /usr/bin/dash "$ROOT/bin/sh"
    rm -f "$ROOT/sbin/init"
    cp -a "$OURS/bin/init" "$ROOT/sbin/init"
    mkdir -p "$ROOT/etc/lp"
    for f in rc osname motd boot-tools.sha256 firewall.conf beacon.conf wpa-start; do
        [[ -f "$OURS/etc/$f" ]] && cp -a "$OURS/etc/$f" "$ROOT/etc/$f"
    done
    # init's list of services lives at /etc/lp/services: /etc/services
    # is netbase's port table, which glibc's getservbyname() reads.
    cp -a "$OURS/etc/services" "$ROOT/etc/lp/services"
    grep -qx /bin/lpsh "$ROOT/etc/shells" 2>/dev/null || echo /bin/lpsh >> "$ROOT/etc/shells"
fi
# preinit looks for /sbin/init first; the base's /init was a link to ours.
ln -sfn sbin/init "$ROOT/init"
log "$(ls "$ROOT/bin" | wc -l) entries in /bin"

# ── 2. /bin/sh ───────────────────────────────────────────────────────
step "/bin/sh"
BINSH="${LP_BINSH:-}"
GATE="${REPO_ROOT}/tests/sh-posix/gate.status"
if [[ -z "$BINSH" ]]; then
    if [[ -f "$GATE" && "$(head -1 "$GATE" | tr -d '[:space:]')" == pass ]]; then
        BINSH=lpsh
    else
        BINSH=dash
    fi
fi
if [[ "$BINSH" == lpsh ]]; then
    in_root dpkg-divert --quiet --local --divert /bin/sh.distrib --add /bin/sh
    ln -sfn lpsh "$ROOT/bin/sh"
    log "lpsh (tests/sh-posix passed the gate)"
else
    # lp-base leaves /bin/sh as dash's own link; only a /bin/sh that is
    # something else is replaced.
    [[ "$(readlink -f "$ROOT/bin/sh")" == */dash ]] || ln -sfn /usr/bin/dash "$ROOT/bin/sh"
    log "dash; our shell is /bin/lpsh, every account's login shell (sh-posix gate: $(head -1 "$GATE" 2>/dev/null || echo 'not run'))"
fi

# ── 3. accounts ──────────────────────────────────────────────────────
#
# The account files are Debian's state, not our configuration: packages
# added their system accounts to them as they were installed (dbus,
# polkit, geoclue, uuidd...), and replacing them with anything else once
# stopped dpkg dead ("unknown system group 'messagebus' in statoverride
# file"). So they are edited, not written.
#
# Three accounts matter here:
#   root   locked ("!"): no password logs in as root. Administration is
#          sudo, with the person's own password (COMMON.md).
#   user   uid 1000, the stick's live account: the installer and "Try LP"
#          run as it. Its password is locked too - nobody signs in to the
#          stick; lp-install turns this account into the person's, with
#          their name, their password and group sudo, on the installed
#          copy.
#   every other account: a system account, "!" or "*", never empty.
step "accounts"
LOGIN_SHELL=/bin/lpsh
[[ -x "$ROOT/bin/lpsh" ]] || LOGIN_SHELL=/bin/sh
sed -i "s|^root:\([^:]*\):0:0:\([^:]*\):/root:.*$|root:\1:0:0:\2:/root:${LOGIN_SHELL}|" "$ROOT/etc/passwd"
if grep -q '^[^:]*:[^:]*:1000:' "$ROOT/etc/passwd"; then
    sed -i "s|^\([^:]*\):\([^:]*\):1000:1000:[^:]*:/home/\([^:]*\):.*$|user:x:1000:1000:LP:/home/user:${LOGIN_SHELL}|" "$ROOT/etc/passwd"
else
    printf 'user:x:1000:1000:LP:/home/user:%s\n' "$LOGIN_SHELL" >> "$ROOT/etc/passwd"
fi
grep -q '^user:' "$ROOT/etc/group" || printf 'user:x:1000:\n' >> "$ROOT/etc/group"
# The live account opens the devices a desktop needs (there is no logind
# to hand them out). Not sudo: that is the installed owner's.
# shadow: one line per account, root and user locked, nothing empty.
python3 - "$ROOT" <<'PY'
import os, sys, time
root = sys.argv[1]
# The device groups udev hands the nodes to (50-udev-default.rules:
# input, render) must exist, or udev leaves the nodes root's alone and
# the desktop account cannot open its own keyboard - which is what the
# session used to answer by opening them to everyone (0666).
have = [l.split(":") for l in open(root + "/etc/group") if l.strip()]
names_g = {f[0] for f in have}
used = {int(f[2]) for f in have if len(f) > 2 and f[2].isdigit()}
add = []
for g in ("input", "render", "kvm"):
    if g not in names_g:
        gid = next(i for i in range(100, 1000) if i not in used)
        used.add(gid)
        add.append("%s:x:%d:" % (g, gid))
if add:
    with open(root + "/etc/group", "a") as f:
        f.write("\n".join(add) + "\n")
    gs = root + "/etc/gshadow"
    if os.path.exists(gs):
        with open(gs, "a") as f:
            f.write("".join("%s:!::\n" % a.split(":")[0] for a in add))
# group: user in the device groups, and in no admin group.
lines = []
for l in open(root + "/etc/group"):
    f = l.rstrip("\n").split(":")
    if len(f) >= 4:
        mem = [m for m in f[3].split(",") if m]
        if f[0] in ("audio", "video", "input", "render", "plugdev", "netdev",
                    "users", "bluetooth") and "user" not in mem:
            mem.append("user")
        if f[0] in ("sudo", "adm") and "user" in mem:
            mem.remove("user")
        f[3] = ",".join(mem)
    lines.append(":".join(f))
open(root + "/etc/group.new", "w").write("\n".join(lines) + "\n")
os.chmod(root + "/etc/group.new", 0o644)
os.rename(root + "/etc/group.new", root + "/etc/group")
names = [l.split(":")[0] for l in open(root + "/etc/passwd") if l.strip()]
path = root + "/etc/shadow"
rows = {}
order = []
for l in open(path):
    f = l.rstrip("\n").split(":")
    if len(f) < 2 or not f[0]:
        continue
    f += [""] * (9 - len(f))
    rows[f[0]] = f
    order.append(f[0])
days = str(int(time.time() // 86400))
for n in names:
    if n not in rows:
        rows[n] = [n, "*", days, "0", "99999", "7", "", "", ""]
        order.append(n)
for n in ("root", "user"):
    if n in rows and not rows[n][1].startswith("!"):
        rows[n][1] = "!" + rows[n][1] if rows[n][1] not in ("", "*") else "!"
for n in order:
    if rows[n][1] == "":
        rows[n][1] = "!"
with open(path + ".new", "w") as f:
    for n in order:
        if n in names:
            f.write(":".join(rows[n][:9]) + "\n")
os.chmod(path + ".new", 0o640)
os.rename(path + ".new", path)
PY
in_root chgrp shadow /etc/shadow
in_root pwck -r -q >/dev/null 2>&1 || in_root pwck -r 2>&1 | sed 's/^/    pwck: /' | head -5
# SHA-512 crypt for everything Debian's tools write: pam_unix (passwd),
# and login.defs (chpasswd, newusers). libc's crypt6 and the recovery
# shell check $6$; bookworm's default is yescrypt ($y$), which they do not.
sed -i 's/^\(password.*pam_unix\.so.*\)\byescrypt\b/\1sha512/' "$ROOT/etc/pam.d/common-password"
grep -q 'pam_unix\.so.*sha512' "$ROOT/etc/pam.d/common-password" ||
    die "pam_unix in /etc/pam.d/common-password does not say sha512"
if grep -q '^ENCRYPT_METHOD' "$ROOT/etc/login.defs"; then
    sed -i 's/^ENCRYPT_METHOD.*/ENCRYPT_METHOD SHA512/' "$ROOT/etc/login.defs"
else
    echo 'ENCRYPT_METHOD SHA512' >> "$ROOT/etc/login.defs"
fi
log "root locked, user (uid 1000) locked until the installer, SHA-512 hashes"

# ── 4. language, time, name ──────────────────────────────────────────
step "locales, time zone, name"
# English is the default, Korean is carried in full (COMMON.md, Language).
for l in "en_US.UTF-8 UTF-8" "ko_KR.UTF-8 UTF-8"; do
    grep -qx "$l" "$ROOT/etc/locale.gen" || echo "$l" >> "$ROOT/etc/locale.gen"
done
in_root locale-gen > /dev/null
printf 'LANG=en_US.UTF-8\n' > "$ROOT/etc/default/locale"
ln -sfn /usr/share/zoneinfo/Etc/UTC "$ROOT/etc/localtime"
echo Etc/UTC > "$ROOT/etc/timezone"
echo linux-lp > "$ROOT/etc/hostname"
printf '127.0.0.1\tlocalhost\n127.0.1.1\tlinux-lp\n::1\t\tlocalhost ip6-localhost ip6-loopback\n' \
    > "$ROOT/etc/hosts"
log "$(in_root locale -a | grep -ci utf) UTF-8 locales, UTC, linux-lp"

# ── 5. the desktop ───────────────────────────────────────────────────
step "the desktop"
D="${REPO_ROOT}/desktop"
H="$ROOT/home/user"
mkdir -p "$ROOT/usr/local/bin" "$ROOT/usr/local/share/applications" "$ROOT/usr/lib/lp"

if [[ -d "$D/theme" ]]; then
    mkdir -p "$ROOT/usr/share/themes/LP/gtk-4.0" "$ROOT/usr/share/themes/LP/gtk-3.0"
    # GTK_THEME=LP (session-run) makes each file the WHOLE theme, so GTK's
    # own dark theme goes in front of it: without it every widget the file
    # does not mention - menus, menu bars, toolbars, entries - came out in
    # GTK's unstyled fallback (LibreOffice's "FileEditViewInsert...").
    { printf '@import url("resource:///org/gtk/libgtk/theme/Default/Default-dark.css");\n\n'
      cat "$D/theme/gtk-4.0/gtk.css"; } > "$ROOT/usr/share/themes/LP/gtk-4.0/gtk.css"
    { printf '@import url("resource:///org/gtk/libgtk/theme/Adwaita/gtk-contained-dark.css");\n\n'
      cat "$D/theme/gtk-3.0/gtk.css"; } > "$ROOT/usr/share/themes/LP/gtk-3.0/gtk.css"
    # The shell's own style (top bar, dock, quick settings, app grid, OSK)
    # and the springs as CSS transitions. lp-shell.c reads them from
    # /usr/local/share/lp; without them every shell surface is drawn in
    # plain Adwaita and the dock sits under the top bar.
    mkdir -p "$ROOT/usr/local/share/lp"
    for f in shell.css shell-light.css motion.css wallpaper.png; do
        [[ -f "$D/theme/$f" ]] && cp -a "$D/theme/$f" "$ROOT/usr/local/share/lp/$f"
    done
fi

# The session. rc starts /bin/start-desktop; that name is the setup gate
# now (desktop/installer/lp-setup-gate), which runs the installer or the
# first-boot setup when one is due and otherwise execs the session's own
# start-desktop, kept here.
if [[ -d "$D/session" ]]; then
    cp -a "$D/session/start-desktop" "$ROOT/usr/lib/lp/start-desktop.session"
    cp -a "$D/session/session-run" "$ROOT/bin/session-run"
    # LP's sway: maximise and minimise work, and a window never opens
    # under the top bar (desktop/compositor/sway-1.7-lp.patch).
    if [[ -x "$D/compositor/sway" ]]; then
        install -m 755 "$D/compositor/sway" "$ROOT/usr/bin/sway"
    fi
    # And LP's wlroots under both compositors: the pointer on the GPU's
    # cursor plane (a virtual machine's too) instead of a whole-screen
    # redraw for every movement (desktop/compositor/wlroots-0.15.1-lp.patch).
    if [[ -f "$D/compositor/libwlroots.so.10" ]]; then
        install -m 644 "$D/compositor/libwlroots.so.10" \
            "$ROOT/usr/lib/x86_64-linux-gnu/libwlroots.so.10"
    fi
    # Every GTK 3 window draws its own title bar (desktop/csd/lp-csd.c).
    if [[ -f "$D/csd/liblp-csd.so" ]]; then
        install -D -m 755 "$D/csd/liblp-csd.so" "$ROOT/usr/local/lib/liblp-csd.so"
    fi
    for s in lp-audio-start lp-idle lp-autoscale lp-shell-start lp-lock lp-logout lp-admin-run lp-input-lang lp-screenshot; do
        [[ -f "$D/session/$s" ]] && cp -a "$D/session/$s" "$ROOT/usr/local/bin/$s"
    done
    if [[ -d "$D/session/fcitx5" ]]; then
        mkdir -p "$H/.config/fcitx5"
        cp -a "$D/session/fcitx5/profile" "$D/session/fcitx5/config" "$H/.config/fcitx5/"
    fi
    [[ -f "$D/session/wayfire.ini" ]] && mkdir -p "$H/.config" &&
        cp -a "$D/session/wayfire.ini" "$H/.config/wayfire.ini"
    if [[ -f "$D/session/sway.config" ]]; then
        mkdir -p "$H/.config/sway"
        cp -a "$D/session/sway.config" "$H/.config/sway/config"
        # The shortcuts, written from wayfire.ini (lp-settings --sway-keys);
        # Settings keeps it in step with every change after this.
        [[ -f "$D/session/sway-keys.conf" ]] &&
            cp -a "$D/session/sway-keys.conf" "$H/.config/sway/lp-keys.conf"
        # Settings -> Keyboard writes the layout here; sway nags about an
        # include that does not exist, so it starts out empty.
        [[ -f "$H/.config/sway/input.conf" ]] ||
            printf '# Settings -> Keyboard writes the layout here.\n' > "$H/.config/sway/input.conf"
    fi
    chmod 755 "$ROOT/usr/lib/lp/start-desktop.session" "$ROOT/bin/session-run"
fi
if [[ -d "$D/bar" ]]; then
    mkdir -p "$H/.config/waybar" "$H/.config/fuzzel"
    cp -a "$D/bar/config.jsonc" "$H/.config/waybar/config" 2>/dev/null || true
    cp -a "$D/bar/style.css" "$H/.config/waybar/style.css" 2>/dev/null || true
    cp -a "$D/bar/fuzzel.ini" "$H/.config/fuzzel/fuzzel.ini" 2>/dev/null || true
fi
if [[ -f "$D/theme/gtk-4.0/gtk.css" ]]; then
    mkdir -p "$H/.config/gtk-4.0" "$H/.config/gtk-3.0"
    # And again as the person's own stylesheet, without the import: GTK
    # reads it above every application's CSS (libadwaita's, libhandy's),
    # so the title bar and its three buttons are the same in all of them.
    cp -a "$D/theme/gtk-4.0/gtk.css" "$H/.config/gtk-4.0/gtk.css"
    cp -a "$D/theme/gtk-3.0/gtk.css" "$H/.config/gtk-3.0/gtk.css"
    if [[ -f "$D/theme/settings.ini" ]]; then
        cp -a "$D/theme/settings.ini" "$H/.config/gtk-4.0/settings.ini"
        cp -a "$D/theme/settings.ini" "$H/.config/gtk-3.0/settings.ini"
    fi
fi
[[ -f "$D/terminal/foot.ini" ]] && mkdir -p "$ROOT/etc/xdg/foot" &&
    cp -a "$D/terminal/foot.ini" "$ROOT/etc/xdg/foot/foot.ini"
[[ -f "$D/terminal/foot.desktop" ]] &&
    cp -a "$D/terminal/foot.desktop" "$ROOT/usr/local/share/applications/foot.desktop"
[[ -f "$D/terminal/org.gnome.Console.desktop" ]] &&
    cp -a "$D/terminal/org.gnome.Console.desktop" "$ROOT/usr/local/share/applications/org.gnome.Console.desktop"

# Every program the desktop tracks have built: desktop/<dir>/lp-* that
# is executable, and its .desktop entry. The installer's own are below.
n=0
for f in "$D"/*/lp-*; do
    [[ -f "$f" && -x "$f" ]] || continue
    case "$f" in "$D"/installer/*|"$D"/firstboot/*|"$D"/session/*) continue ;; esac
    cp -a "$f" "$ROOT/usr/local/bin/"; n=$((n + 1))
done
# The session's loading cover is one level deeper than the rest.
[[ -x "$D/branding/lp-splash-fade/lp-splash-fade" ]] &&
    cp -a "$D/branding/lp-splash-fade/lp-splash-fade" "$ROOT/usr/local/bin/"
# The lock screen checks passwords through PAM, as its own service.
[[ -f "$D/lockscreen/pam.lp-lockscreen" ]] &&
    install -D -m 644 "$D/lockscreen/pam.lp-lockscreen" "$ROOT/etc/pam.d/lp-lockscreen"
# Sound: WirePlumber without logind (the file says why it must be).
[[ -f "$D/session/wireplumber-no-logind.lua" ]] &&
    install -D -m 644 "$D/session/wireplumber-no-logind.lua" \
        "$ROOT/etc/wireplumber/bluetooth.lua.d/80-lp-no-logind.lua"
for f in "$D"/*/lp-*.desktop; do
    [[ -f "$f" ]] && cp -a "$f" "$ROOT/usr/local/share/applications/"
done
log "$n desktop programs"

# The command-line tools (desktop/cli): lp-time, lp-info, lp-hostname.
# Debian's timedatectl and hostnamectl ask systemd over D-Bus, and there
# is no systemd here - Settings' Date & Time and machine name went
# through them and failed every time. Ours take their place, diverted so
# that an apt upgrade of systemd does not put Debian's back.
if [[ -d "$D/cli" ]]; then
    for f in "$D"/cli/*; do
        [[ -f "$f" ]] && install -m 755 "$f" "$ROOT/usr/local/bin/$(basename "$f")"
    done
    for pair in timedatectl:lp-time hostnamectl:lp-hostname; do
        name=${pair%%:*} tool=${pair##*:}
        if [[ -e "$ROOT/usr/bin/$name" && ! -L "$ROOT/usr/bin/$name" ]]; then
            in_root dpkg-divert --local --rename --divert "/usr/bin/$name.debian" \
                --add "/usr/bin/$name" >/dev/null
        fi
        ln -sfn "/usr/local/bin/$tool" "$ROOT/usr/bin/$name"
        ln -sfn "$tool" "$ROOT/usr/local/bin/$name"
    done
    log "cli: $(ls "$D/cli" | tr '\n' ' ')(timedatectl, hostnamectl)"
fi

# What this system is (desktop/branding/os-release): LP, like Debian for
# apt's sake. base-files ships Debian's; it is diverted, not overwritten.
if [[ -f "$D/branding/os-release" ]]; then
    if [[ ! -e "$ROOT/usr/lib/os-release.debian" ]]; then
        in_root dpkg-divert --local --rename --divert /usr/lib/os-release.debian \
            --add /usr/lib/os-release >/dev/null
    fi
    install -m 644 "$D/branding/os-release" "$ROOT/usr/lib/os-release"
    ln -sfn ../usr/lib/os-release "$ROOT/etc/os-release"
fi

# The wallpapers (branding track), at the resolutions they were drawn
# for; sway.config points at the 3840x2160 dark one.
if compgen -G "$D/branding/wallpaper/*.png" >/dev/null; then
    mkdir -p "$ROOT/usr/share/backgrounds/lp"
    cp -a "$D"/branding/wallpaper/*.png "$ROOT/usr/share/backgrounds/lp/"
    log "wallpapers: $(ls "$D"/branding/wallpaper/*.png | wc -l)"
fi

# The icon theme (branding track), if it has been made.
if [[ -f "$D/icons/LP/index.theme" ]]; then
    rm -rf "$ROOT/usr/share/icons/LP"
    cp -a "$D/icons/LP" "$ROOT/usr/share/icons/LP"
    in_root gtk-update-icon-cache -q -f /usr/share/icons/LP 2>/dev/null || true
    log "icons: LP"
fi

# Graphics (fonts-and-graphics track): whatever it puts under
# desktop/graphics/rootfs/ goes over the root as it is - environment,
# GSettings overrides, drirc.
if [[ -d "$D/graphics/rootfs" ]]; then
    cp -a "$D/graphics/rootfs/." "$ROOT/"
    log "graphics: $(find "$D/graphics/rootfs" -type f | wc -l) files"
fi

# Type. Pretendard and D2Coding are not packaged by Debian, so they are
# fetched (and cached); local.conf is what makes GTK's "sans-serif" mean
# Pretendard rather than DejaVu.
if [[ -x "${REPO_ROOT}/tools/fetch-fonts.sh" ]]; then
    LP_FONT_CACHE="${LPZERO_WORK}/fontcache" \
        "${REPO_ROOT}/tools/fetch-fonts.sh" "$ROOT/usr/share/fonts/truetype" >/dev/null ||
        warn "Pretendard and D2Coding were not fetched - the desktop falls back to Noto"
fi
if [[ -f "$D/fonts/local.conf" ]]; then
    mkdir -p "$ROOT/etc/fonts"
    cp -a "$D/fonts/local.conf" "$ROOT/etc/fonts/local.conf"
fi
[[ -d "$ROOT/usr/share/glib-2.0/schemas" ]] &&
    in_root glib-compile-schemas /usr/share/glib-2.0/schemas 2>/dev/null
# The font cache, made now: otherwise the first application started on
# the machine spends seconds building it, on a 4K screen, visibly.
in_root fc-cache -s >/dev/null 2>&1 || warn "fc-cache failed"

# Firmware (kernel track, tools/fetch-pc-fw.sh): the kernel's initramfs
# carries what it needs to boot; /lib/firmware gets the whole set, so a
# driver loaded later finds its blob too.
if [[ -d "$PC_FW" ]]; then
    mkdir -p "$ROOT/usr/lib/firmware"
    cp -a "$PC_FW/." "$ROOT/usr/lib/firmware/"
    log "firmware: $(find "$PC_FW" -type f | wc -l) files from $(basename "$PC_FW")"
else
    warn "no $PC_FW (tools/fetch-pc-fw.sh) - WiFi and Bluetooth firmware missing"
fi

# ── 6. the installer and the first-boot setup ────────────────────────
#
# Built here, in the root being assembled, because they link its GTK
# 4.8; `make` is incremental and costs nothing when they are current.
step "installer and first-boot setup"
mkdir -p "$ROOT/mnt/lp-src"
mount --bind "$D" "$ROOT/mnt/lp-src"
for dir in installer firstboot; do
    in_root make -s -C "/mnt/lp-src/$dir" SHELL=/usr/bin/dash >/dev/null ||
        die "desktop/$dir did not build"
done
in_root make -s -C /mnt/lp-src/installer install SHELL=/usr/bin/dash
in_root make -s -C /mnt/lp-src/firstboot install SHELL=/usr/bin/dash
umount "$ROOT/mnt/lp-src"
rmdir "$ROOT/mnt/lp-src"
install -m 755 "$D/installer/lp-setup-gate" "$ROOT/bin/start-desktop"
# This root is the installer medium. lp-install removes the file from the
# copy it makes; the gate reads it at every boot of the stick.
mkdir -p "$ROOT/etc/lp"
printf '# This root is an LP installer medium: lp-setup-gate starts the installer.\n' \
    > "$ROOT/etc/lp/installer-medium"
log "lp-installer, lp-firstboot, lp-install, the setup gate"

# ── 7. the live account's home ───────────────────────────────────────
#
# Disk names are English; what a person sees is the file manager's
# translation. XDG's user-dirs.dirs is what GLib's special-directory
# lookup reads, so every application agrees on where Downloads is.
mkdir -p "$H"/{Desktop,Documents,Downloads,Music,Pictures,Videos} \
         "$H/.local/share/Trash/files" "$H/.local/share/Trash/info" \
         "$H/.cache" "$H/.config/lp"
cat > "$H/.config/user-dirs.dirs" <<'DIRS'
# XDG standard folders. The names on disk are English; applications
# show them in the session's language.
XDG_DESKTOP_DIR="$HOME/Desktop"
XDG_DOCUMENTS_DIR="$HOME/Documents"
XDG_DOWNLOAD_DIR="$HOME/Downloads"
XDG_MUSIC_DIR="$HOME/Music"
XDG_PICTURES_DIR="$HOME/Pictures"
XDG_VIDEOS_DIR="$HOME/Videos"
XDG_TEMPLATES_DIR="$HOME"
XDG_PUBLICSHARE_DIR="$HOME"
DIRS
printf 'en_US.UTF-8\n' > "$H/.config/lp/locale"
chown -R 1000:1000 "$H"
chmod 750 "$H"
mkdir -p "$ROOT/data" "$ROOT/boot"

# The setuid programs.
#   lp-power  one word in, nothing exec'd - how an account that is not
#             root turns the machine off without logind
#   sudo      asks the caller's own password (sudo group), then runs
#   su        asks the target account's password
#   passwd    changes the caller's own password, after the current one
# Each checks its real uid before it does anything. Copied into the root
# like every other program they arrived 0755, and sudo refused everyone
# with "must be owned by uid 0 and have the setuid bit set" - on a
# machine whose administrator is meant to be an ordinary account with a
# password, nobody could administer it.
for p in lp-power sudo su passwd; do
    [[ -f "$ROOT/bin/$p" && ! -L "$ROOT/bin/$p" ]] || continue
    chown 0:0 "$ROOT/bin/$p"
    chmod 4755 "$ROOT/bin/$p"
done

# passwd at the prompt is Debian's here. Ours keeps the hashes in
# /data/shadow - right on a RAM root, where /etc is unpacked afresh at
# every boot - but on this disk root the login screen, the lock screen
# and Settings all read /etc/shadow: a password changed in a terminal
# changed for sudo and for nothing else, and ours, checking the real
# uid, could not write /data for an ordinary account anyway.
if [[ -f "$ROOT/usr/bin/passwd" && ! -L "$ROOT/usr/bin/passwd" && -u "$ROOT/usr/bin/passwd" ]]; then
    ln -sfn /usr/bin/passwd "$ROOT/bin/passwd"
fi

# ── 8. what the image must not carry ─────────────────────────────────
step "scrub"
scrub() {
    rm -f "$ROOT"/etc/apt/apt.conf.d/*proxy* "$POLICY"
    rm -f "$ROOT"/tmp/build-ca.crt "$ROOT"/usr/local/share/ca-certificates/*build*
    sed -i 's/\[trusted=yes\] //; s/ \[trusted=yes\]//' \
        "$ROOT"/etc/apt/sources.list "$ROOT"/etc/apt/sources.list.d/*.list 2>/dev/null || true
    rm -rf "$ROOT"/var/lib/apt/lists/* "$ROOT"/var/cache/apt/*.bin \
           "$ROOT"/var/cache/apt/archives/*.deb
    mkdir -p "$ROOT/var/lib/apt/lists/partial"
    # A machine without an address from DHCP yet still resolves; dhcp
    # overwrites this with the network's own servers.
    printf '# Replaced by dhcp with the network'"'"'s servers once a link is up.\nnameserver 1.1.1.1\nnameserver 8.8.8.8\n' \
        > "$ROOT/etc/resolv.conf"
    : > "$ROOT/etc/machine-id"
    rm -f "$ROOT/var/lib/dbus/machine-id" "$ROOT"/etc/ssh/ssh_host_* \
          "$ROOT"/etc/dropbear/*_host_key "$ROOT"/data/dropbear_*_host_key \
          "$ROOT/var/lib/systemd/random-seed"
    rm -f "$ROOT"/root/.*_history "$ROOT"/home/*/.*_history "$ROOT"/root/.lesshst \
          "$ROOT"/root/.viminfo "$ROOT"/root/.wget-hsts "$ROOT"/home/*/.lesshst
    rm -rf "$ROOT"/root/.cache "$ROOT"/var/tmp/* "$ROOT"/var/log/journal
    find "$ROOT/var/log" -type f -delete 2>/dev/null || true
    rm -f "$ROOT"/var/cache/debconf/*-old "$ROOT"/var/lib/dpkg/*-old
}
leaks() {
    local bad=()
    grep -rlsI 'Acquire::.*Proxy' "$ROOT/etc/apt" >/dev/null && bad+=("apt proxy setting")
    grep -rlsI 'trusted=yes' "$ROOT/etc/apt" >/dev/null && bad+=("[trusted=yes]")
    [[ -e "$ROOT/tmp/build-ca.crt" || -e "$DEB/tmp/build-ca.crt" ]] && bad+=("build CA")
    [[ -f "$POLICY" ]] && bad+=("policy-rc.d")
    [[ -s "$ROOT/etc/machine-id" ]] && bad+=("machine-id")
    ls "$ROOT"/etc/ssh/ssh_host_* "$ROOT"/etc/dropbear/*_host_key >/dev/null 2>&1 && bad+=("SSH host keys")
    [[ -n "$(ls -A "$ROOT/var/lib/apt/lists" | grep -v '^partial$\|^lock$' || true)" ]] && bad+=("apt lists")
    ls "$ROOT"/root/.*_history "$ROOT"/home/*/.*_history >/dev/null 2>&1 && bad+=("shell history")
    [[ -n "$(find "$ROOT/var/log" -type f -print -quit)" ]] && bad+=("logs")
    local me; me="$(hostname)"
    case "$me" in localhost|linux-lp|"") ;; *)
        grep -qsw -- "$me" "$ROOT/etc/hostname" "$ROOT/etc/hosts" && bad+=("the build host's name") ;;
    esac
    cmp -s /etc/resolv.conf "$ROOT/etc/resolv.conf" && bad+=("the build host's resolv.conf")
    [[ -e "$ROOT/setup.sh" || -e "$ROOT/apps.sh" || -e "$ROOT/&2" ]] && bad+=("base build scripts")
    if (( ${#bad[@]} )); then
        printf '  leak: %s\n' "${bad[@]}" >&2
        return 1
    fi
}
# ── slim ──
# The machine speaks English and Korean; the other ninety languages'
# translations, and the manuals, change logs and help pages nobody opens
# from a desktop, are about 300MB of a root that has to fit, with room to
# spare, on a 16GB disk. dpkg is told to leave them out from now on too,
# so an apt install later does not bring them back. (Copyright files
# stay: they are the licence, not documentation.)
slim() {
    local before after
    before=$(du -smx "$ROOT/usr/share" | cut -f1)
    mkdir -p "$ROOT/etc/dpkg/dpkg.cfg.d"
    cat > "$ROOT/etc/dpkg/dpkg.cfg.d/50-lp-slim" <<'SLIM'
# LP: only English and Korean translations, no manuals or change logs
# (the copyright files stay). tools/mkdesktop.sh says why.
path-exclude=/usr/share/locale/*
path-include=/usr/share/locale/locale.alias
path-include=/usr/share/locale/en*
path-include=/usr/share/locale/ko*
path-exclude=/usr/share/doc/*
path-include=/usr/share/doc/*/copyright
path-exclude=/usr/share/help/*
path-include=/usr/share/help/C/*
path-include=/usr/share/help/ko/*
path-exclude=/usr/share/gtk-doc/*
path-exclude=/usr/share/info/*
SLIM
    find "$ROOT/usr/share/locale" -mindepth 1 -maxdepth 1 -type d \
        ! -name 'en*' ! -name 'ko*' -exec rm -rf {} + 2>/dev/null || true
    find "$ROOT/usr/share/doc" -mindepth 2 ! -name copyright -type f -delete 2>/dev/null || true
    find "$ROOT/usr/share/help" -mindepth 1 -maxdepth 1 -type d \
        ! -name C ! -name ko -exec rm -rf {} + 2>/dev/null || true
    rm -rf "$ROOT/usr/share/gtk-doc" "$ROOT"/usr/share/info/* 2>/dev/null || true
    # LibreOffice's help and the other languages' UI in its own tree.
    find "$ROOT/usr/lib/libreoffice/share/extensions" -maxdepth 1 -name 'dict-*' \
        ! -name 'dict-en*' ! -name 'dict-ko*' -exec rm -rf {} + 2>/dev/null || true
    after=$(du -smx "$ROOT/usr/share" | cut -f1)
    log "slim: /usr/share ${before}MB -> ${after}MB"
}
# Launchers that open no window of their own, out of the app grid: a
# server and its client (footclient without the server fails), the
# input method's daemon (already running) and its one-time migrator, a
# developer's layout viewer, and mpv beside Celluloid, which is the same
# player with a window. Each shown one did nothing when pressed. An entry
# of the same name in /usr/local/share comes first in XDG_DATA_DIRS, so
# the packages' own files stay untouched.
mkdir -p "$ROOT/usr/local/share/applications"
for id in foot-server footclient org.fcitx.Fcitx5 org.fcitx.fcitx5-migrator \
          kbd-layout-viewer5 mpv; do
    printf '[Desktop Entry]\nType=Application\nName=%s\nExec=true\nNoDisplay=true\n' "$id" \
        > "$ROOT/usr/local/share/applications/$id.desktop"
done
log "hidden from the app grid: launchers with no window of their own"
# GParted runs as root, through pkexec, which needs a logind session for
# its password window - LP has none, so pressing it did nothing. Its
# launcher asks through lp-admin-run instead (desktop/session).
if [[ -f "$ROOT/usr/share/applications/gparted.desktop" ]]; then
    sed -e 's|^Exec=.*|Exec=/usr/local/bin/lp-admin-run /usr/sbin/gparted %f|' \
        -e '/^X-KDE-SubstituteUID/d' -e '/^X-KDE-Username/d' \
        "$ROOT/usr/share/applications/gparted.desktop" \
        > "$ROOT/usr/local/share/applications/gparted.desktop"
    log "GParted: asks for the password through lp-admin-run"
fi

slim
# EC2's metadata daemon has nothing to do on a desktop: it would only
# keep asking 169.254.169.254 on every network the laptop joins.
if [[ -f "$ROOT/etc/lp/services" ]]; then
    sed -i 's|^?/bin/ec2 ec2 -d$|# ?/bin/ec2 ec2 -d   (not on the desktop: tools/mkdesktop.sh)|' "$ROOT/etc/lp/services"
fi
# Bluetooth: bluez's bus activation file hands the job to systemd
# (Exec=/bin/false, SystemdService=...), so without a service line here
# bluetoothd never ran and Settings > Bluetooth had no adapter to show.
if [[ -f "$ROOT/etc/lp/services" && -x "$ROOT/usr/libexec/bluetooth/bluetoothd" ]] &&
   ! grep -q bluetoothd "$ROOT/etc/lp/services"; then
    printf '\n# Bluetooth (bluez). -n: stay in the foreground, init watches it.\n?/usr/libexec/bluetooth/bluetoothd /usr/libexec/bluetooth/bluetoothd -n\n' \
        >> "$ROOT/etc/lp/services"
fi
# The seat's devices belong to the desktop account (uid 1000) as well as
# to their groups - what logind's uaccess does for the person at the
# screen, here for the one account the session runs as. A keyboard, a
# mouse or a USB sound card plugged in later is theirs the same way;
# before, the session opened every node to every user (0666), which
# let any process on the machine read the keyboard.
mkdir -p "$ROOT/etc/udev/rules.d"
cat > "$ROOT/etc/udev/rules.d/71-lp-seat.rules" <<'RULES'
# LP: the desktop account owns the seat's devices (tools/mkdesktop.sh).
SUBSYSTEM=="input", KERNEL=="event*", OWNER="1000", MODE="0660"
SUBSYSTEM=="drm", KERNEL=="card[0-9]*|renderD*", OWNER="1000", MODE="0660"
SUBSYSTEM=="sound", OWNER="1000", MODE="0660"
RULES
# Undoing the last update: apt keeps what each run replaces
# (desktop/system/lp-rollback-save), `sudo lp-rollback` puts it back -
# on the running system, or from the recovery shell through lp-chroot.
install -D -m 755 "$D/system/lp-rollback-save" "$ROOT/usr/lib/lp/lp-rollback-save"
install -D -m 755 "$D/system/lp-rollback" "$ROOT/usr/sbin/lp-rollback"
install -D -m 644 "$D/system/80lp-rollback" "$ROOT/etc/apt/apt.conf.d/80lp-rollback"
install -D -m 755 "$D/system/lp-safe-mode" "$ROOT/usr/sbin/lp-safe-mode"
install -D -m 755 "$D/system/lp-bugreport" "$ROOT/usr/bin/lp-bugreport"
# The login shells' profile. The one the base carried set HOME, TERM,
# LANG and XDG_RUNTIME_DIR to root's console values in every terminal
# (desktop/session/profile says what that broke).
install -m 644 "$D/session/profile" "$ROOT/etc/profile"
# thermald, on real Intel hardware only. In a virtual machine there is
# nothing for it to hold down (no RAPL, no DPTF), it exits, and init
# would start it again and again; there the line exits 78
# (LP_EXIT_NO_HARDWARE), which init takes as "nothing to do on this
# machine" and does not start again. (It used to wait on
# `sleep 2147483647`, which our sleep refuses as too long - the same
# restart loop.)
if [[ -f "$ROOT/etc/lp/services" && -x "$ROOT/usr/sbin/thermald" ]]; then
    mkdir -p "$ROOT/usr/lib/lp"
    cat > "$ROOT/usr/lib/lp/thermald-start" <<'SH'
#!/bin/sh
# init's thermald line (tools/mkdesktop.sh).
if /usr/bin/grep -q GenuineIntel /proc/cpuinfo && ! /usr/bin/grep -qw hypervisor /proc/cpuinfo &&
   test -d /sys/class/powercap/intel-rapl ; then
    exec /usr/sbin/thermald --no-daemon --adaptive
fi
exit 78
SH
    chmod 755 "$ROOT/usr/lib/lp/thermald-start"
    grep -q thermald "$ROOT/etc/lp/services" ||
        printf '\n# Intel thermal daemon (real hardware only, see thermald-start).\n?/usr/lib/lp/thermald-start =thermald /usr/lib/lp/thermald-start\n' \
            >> "$ROOT/etc/lp/services"
fi
# The screen's console asks for a login here (userland/init/init.c):
# a laptop's Ctrl+Alt+F1 was otherwise a root shell with no password.
mkdir -p "$ROOT/etc/lp"
printf '%s\n' 'Present: init runs /bin/login on the virtual terminals, not a root shell.' \
    > "$ROOT/etc/lp/console-login"
# Which application opens what. Without a list GIO takes the first
# application that claims a type in name order: folders opened in Disk
# Usage Analyzer, text in Geany, photos in Drawing, PDFs in LibreOffice.
mkdir -p "$ROOT/etc/xdg"
{
    echo '[Default Applications]'
    echo 'inode/directory=lp-files.desktop'
    for t in application/pdf application/x-pdf application/postscript image/tiff; do echo "$t=org.gnome.Evince.desktop"; done
    for t in text/plain text/markdown text/x-log text/csv application/x-shellscript text/x-csrc text/x-chdr text/x-python application/json application/xml; do echo "$t=org.gnome.gedit.desktop"; done
    # Photos open in LP Photos (desktop/photos), which also edits them;
    # the formats it does not read stay with Eye of GNOME.
    for t in image/png image/jpeg image/gif image/webp image/bmp; do echo "$t=lp-photos.desktop"; done
    for t in image/svg+xml image/x-icon image/heif image/avif; do echo "$t=org.gnome.eog.desktop"; done
    # Videos open in LP Videos (desktop/video), which also trims and
    # converts them; music stays in Celluloid, which keeps a playlist.
    for t in video/mp4 video/x-matroska video/webm video/quicktime video/x-msvideo video/mpeg video/ogg video/3gpp; do echo "$t=lp-video.desktop"; done
    for t in audio/mpeg audio/ogg audio/flac audio/x-wav audio/wav audio/mp4 audio/aac audio/x-vorbis+ogg; do echo "$t=io.github.celluloid_player.Celluloid.desktop"; done
    for t in application/zip application/x-tar application/gzip application/x-compressed-tar application/x-xz-compressed-tar application/x-bzip2-compressed-tar application/x-7z-compressed application/vnd.rar application/x-rar application/zstd; do echo "$t=org.gnome.FileRoller.desktop"; done
    for t in text/html application/xhtml+xml x-scheme-handler/http x-scheme-handler/https; do echo "$t=firefox-esr.desktop"; done
    for t in application/vnd.oasis.opendocument.text application/msword application/vnd.openxmlformats-officedocument.wordprocessingml.document application/rtf; do echo "$t=libreoffice-writer.desktop"; done
    for t in application/vnd.oasis.opendocument.spreadsheet application/vnd.ms-excel application/vnd.openxmlformats-officedocument.spreadsheetml.sheet; do echo "$t=libreoffice-calc.desktop"; done
    for t in application/vnd.oasis.opendocument.presentation application/vnd.ms-powerpoint application/vnd.openxmlformats-officedocument.presentationml.presentation; do echo "$t=libreoffice-impress.desktop"; done
} > "$ROOT/etc/xdg/mimeapps.list"
log "default applications: $(grep -c = "$ROOT/etc/xdg/mimeapps.list") types; bluetoothd in init's services"
scrub
leaks || die "the image would carry what the build host left in it"
log "no proxy, CA, apt lists, machine-id, host keys, histories or logs"

# ── 9. does it hold together ─────────────────────────────────────────
#
# The ways this root has broken before, each checked by running it.
step "checks"
fail=()
if (( USE_DEB )); then
    # Documentation was stripped from the base long before this script
    # (there is no path-exclude for it, the files are simply gone), so
    # dpkg reports it missing; what has to be intact is everything else.
    v="$(in_root dpkg --verify coreutils bash dash util-linux 2>&1 || true)"
    docs=$(grep -c ' /usr/share/\(doc\|man\|info\|locale\|lintian\)/' <<<"$v" || true)
    v="$(grep -v ' /usr/share/\(doc\|man\|info\|locale\|lintian\)/' <<<"$v" || true)"
    [[ -z "$v" ]] || fail+=("dpkg --verify: $(echo "$v" | head -3 | tr '\n' ' ')")
    (( docs == 0 )) || log "dpkg --verify: ${docs} documentation files missing from the base (not a fault)"
fi
printf '#!/bin/bash\necho ok\n' > "$ROOT/tmp/t.sh"; chmod +x "$ROOT/tmp/t.sh"
[[ "$(in_root /tmp/t.sh 2>&1)" == ok ]] || fail+=("#!/bin/bash scripts do not run")
case "$(readlink -f "$ROOT/bin/sh")" in
    */dash) [[ "$BINSH" == dash ]] || fail+=("/bin/sh is dash, wanted $BINSH") ;;
    */lpsh|*/sh) [[ "$BINSH" == lpsh ]] || fail+=("/bin/sh is not dash") ;;
    *) fail+=("/bin/sh is $(readlink -f "$ROOT/bin/sh")") ;;
esac
cmp -s "$ROOT/bin/ls" "$OURS/bin/ls" || fail+=("/bin/ls is not ours")
cmp -s "$ROOT/sbin/init" "$OURS/bin/init" || fail+=("/sbin/init is not ours")
[[ "$(in_root getent services ssh | awk '{ print $2 }')" == 22/tcp ]] ||
    fail+=("/etc/services is not the port table (getent services ssh)")
[[ -f "$ROOT/etc/lp/services" ]] || fail+=("no /etc/lp/services for init")
for l in en_US.utf8 ko_KR.utf8; do
    in_root locale -a | grep -qx "$l" || fail+=("locale $l missing")
done
[[ -n "$(in_root fc-list :lang=ko family 2>/dev/null | head -1)" ]] || fail+=("no Korean font (fc-list :lang=ko)")
awk -F: '$2 == "" { exit 1 }' "$ROOT/etc/shadow" || fail+=("an empty password in /etc/shadow")
grep -q '^root:!' "$ROOT/etc/shadow" || fail+=("root is not locked")
in_root python3 -c 'import warnings; warnings.simplefilter("ignore"); import crypt' ||
    fail+=("python3 has no crypt (lp-install hashes passwords with it)")
for b in /usr/local/bin/lp-installer /usr/local/bin/lp-firstboot; do
    in_root ldd "$b" 2>&1 | grep -q 'not found' && fail+=("$b: missing libraries")
done
in_root /usr/local/bin/lp-install list --json >/dev/null || fail+=("lp-install does not run")
if (( ${#fail[@]} )); then
    printf '  FAIL: %s\n' "${fail[@]}" >&2
    die "the root does not hold together"
fi
log "dpkg clean, bash runs, /bin/sh $(readlink "$ROOT/bin/sh"), ls and init ours,"
log "ports from netbase, en_US + ko_KR, Korean fonts, no empty passwords, root locked"

rm -f "$ROOT/tmp/t.sh"
# Not -x: when the overlay's upper layer is on another filesystem than the
# base (the work directory in /dev/shm, the base on disk), overlayfs gives
# each file the st_dev of the layer it lives on, and -x skipped every one
# of them - the root partition came out a fraction of its size and mkfs
# failed with "Could not allocate block". The mounts are excluded by name.
SIZE_MB=$(du -sm "$ROOT" --exclude=proc --exclude=sys --exclude=dev --exclude=tmp 2>/dev/null | cut -f1)
log "root: ${SIZE_MB}MB (upper layer $(du -sxm "$UPPER" | cut -f1)MB)"

if [[ "${LP_CHECK_ONLY:-0}" == 1 ]]; then
    log "LP_CHECK_ONLY=1: no image"
    exit 0
fi

# ── 10. the image ────────────────────────────────────────────────────
#
# mkfs.ext4 -d reads the directory it is given, and a mount point under
# it is read too - so the kernel's filesystems and the scratch tmpfs come
# off first, and the image sees exactly the root.
for m in tmp dev/pts dev sys proc; do umount "$ROOT/$m"; done
chmod 1777 "$ROOT/tmp"
# The root partition: what is there, a quarter again for ext4 and for
# the first updates, and 2GB for the person's files on the stick. (An
# installed system gets the whole disk; this is only the stick.)
#
# LP_EDITION=8g is the same system and the same installer for a stick of
# 8GB, which the image above (about 9GB) does not fit on: half a GB for
# files on the stick instead of 2GB, and a recovery partition without the
# reinstall payload - the recovery menu and its shell are there, on the
# stick and on every disk installed from it, but "Reinstall LP" says the
# partition carries no installation files. About 6GB in all.
EDITION_ENV=()
if [[ "${LP_EDITION:-}" == 8g ]]; then
    ROOT_MB=$(( SIZE_MB * 5 / 4 + 512 ))
    EDITION_ENV=(LP_NO_PAYLOAD=1 LP_IMAGE_NAME=linux-LP_desktop-8g.img
                 LP_MAX_BYTES=7000000000)
else
    ROOT_MB=$(( SIZE_MB * 5 / 4 + 2048 ))
fi
step "the disk image"
env "${EDITION_ENV[@]}" LP_ROOTFS_OVERRIDE="$ROOT" LP_ROOT_MB="$ROOT_MB" LP_DESKTOP=1 \
    "${REPO_ROOT}/tools/mkdisk.sh"
