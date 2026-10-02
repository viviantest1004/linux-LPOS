#!/usr/bin/env bash
#
# mkrecovery.sh - build the tree of the LP-RECOVERY partition.
#
#   tools/mkrecovery.sh --root ROOTFS --out TREE --kernel BZIMAGE [--cmdline LINE]
#                       [--no-payload]
#
# --no-payload leaves /reinstall empty: the recovery system and its shell,
# without the 1.2GB copy of the root to reinstall from. That is the 8GB
# stick's image (mkdesktop.sh, LP_EDITION=8g), which has no room for it;
# the recovery menu then says the partition carries no installation
# files (LPS_R_NO_PAYLOAD) instead of offering a reinstall.
#
# tools/mkdisk.sh calls this with the root it has just packed and turns
# TREE into the ext4 of p2. lp-install copies that partition to the disk
# it installs to, so every installed LP carries the same recovery system
# as the stick it came from.
#
# ── What is in it ──
#
#   /sbin/init, /bin/*        our static userland: init sees
#                             lp.mode=recovery and runs the menu on tty1
#   /recovery/bin/lp-recovery the menu (recovery/): recovery shell after an
#                             administrator's password, reinstall, disk
#                             checks, restart - all usable by touch
#   /usr/bin/tar, zstd        Debian's, for the reinstall payload, and
#   /usr/sbin/e2fsck, mkfs.ext4, fsck.vfat
#                             the disk tools, with exactly the shared
#                             libraries they load (found from their ELF
#                             headers, not guessed) and an ld.so.cache
#   /reinstall/               the payload: root.tar.zst (this very system,
#                             numeric owners, xattrs, hard links),
#                             esp/{vmlinuz.efi,lpboot.efi,cmdline.txt} and
#                             a manifest with the sha256 and size of each -
#                             reinstall.c refuses to touch LP-ROOT unless
#                             every one of them checks out
#
# The root is booted by preinit with root=PARTUUID=<this partition> (the
# boot menu reads it from \EFI\LP\recovery.ok, which mkdisk and lp-install
# write only when this tree has a system in it) and nothing else of the
# desktop runs: no rc, no services, no desktop.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"

log()  { printf '  %s\n' "$*"; }
die()  { printf 'mkrecovery: %s\n' "$*" >&2; exit 1; }

ROOTFS= OUT= KERNEL= CMDLINE= PAYLOAD=1
while (($#)); do
    case "$1" in
        --no-payload) PAYLOAD=0; shift ;;
        --root)    ROOTFS=$2; shift 2 ;;
        --out)     OUT=$2; shift 2 ;;
        --kernel)  KERNEL=$2; shift 2 ;;
        --cmdline) CMDLINE=$2; shift 2 ;;
        *) die "unknown argument: $1" ;;
    esac
done
[[ -d "$ROOTFS" && -n "$OUT" && -f "$KERNEL" ]] ||
    die "usage: $0 --root ROOTFS --out TREE --kernel BZIMAGE [--cmdline LINE] [--no-payload]"

OURS="${REPO_ROOT}/userland/rootfs-amd64"
MENU="${REPO_ROOT}/recovery/bin-amd64/lp-recovery"
LPBOOT="${LP_BOOT_EFI:-${REPO_ROOT}/boot/efi/lpboot.efi}"
[[ -x "$OURS/bin/init" ]] || die "no userland at $OURS (userland/mkrootfs.sh amd64)"
[[ -x "$MENU" ]]          || die "no $MENU (make -C recovery)"
[[ -f "$LPBOOT" ]]        || die "no $LPBOOT (make -C boot/efi)"

mkdir -p "$OUT"
cd "$OUT"

# ── the skeleton ─────────────────────────────────────────────────────
mkdir -p bin sbin dev proc sys run root etc/lp mnt/lp mnt/esp \
         var/lib/lp-recovery var/log recovery/bin reinstall/esp \
         usr/bin usr/sbin usr/lib/x86_64-linux-gnu lib64
mkdir -p -m 1777 tmp
chmod 700 root var/lib/lp-recovery
ln -sfn usr/lib lib

# ── our userland ─────────────────────────────────────────────────────
cp -a "$OURS/bin/." bin/
cp -a "$OURS/bin/init" sbin/init
ln -sfn sbin/init init
install -m 755 "$MENU" recovery/bin/lp-recovery
log "userland: $(ls bin | wc -l) programs, init, lp-recovery"

# The accounts of the recovery system itself: root only, no password -
# nobody logs in here. The recovery shell is opened by the menu after it
# has checked an administrator's password against the INSTALLED system's
# /etc/shadow (or, when that cannot be read, /etc/lp/recovery-password,
# which lp-install writes onto this partition).
printf 'root:x:0:0:root:/root:/bin/lpsh\n' > etc/passwd
printf 'root:*:0:\n' > etc/group
printf 'root:*:19000:0:99999:7:::\n' > etc/shadow
chmod 600 etc/shadow
printf 'lp-recovery\n' > etc/hostname
[[ -f "$OURS/etc/profile" ]] && cp -a "$OURS/etc/profile" etc/profile
printf '/usr/lib/x86_64-linux-gnu\n' > etc/ld.so.conf

# ── Debian's disk tools, with exactly the libraries they load ────────
#
# The ELF headers say what each one needs (DT_NEEDED) and which loader
# runs it (PT_INTERP); both are looked up inside ROOTFS, following its
# symlinks as ROOTFS sees them, not as this build machine would.
python3 - "$ROOTFS" "$OUT" <<'PY'
import os, shutil, subprocess, sys
root, out = sys.argv[1], sys.argv[2]
TOOLS = ["usr/bin/tar", "usr/bin/zstd", "usr/sbin/e2fsck", "usr/sbin/mke2fs",
         "usr/sbin/fsck.fat"]
LINKS = {"usr/sbin/mkfs.ext4": "mke2fs", "usr/sbin/fsck.vfat": "fsck.fat"}
DIRS = ["usr/lib/x86_64-linux-gnu", "lib/x86_64-linux-gnu", "usr/lib", "lib",
        "usr/lib64", "lib64"]

def inside(path):
    """path (absolute, as ROOTFS sees it) resolved inside ROOTFS."""
    parts = [p for p in path.split("/") if p]
    cur = ""
    for depth in range(64):
        if not parts:
            return os.path.join(root, cur)
        p = parts.pop(0)
        cand = os.path.join(cur, p) if cur else p
        full = os.path.join(root, cand)
        if os.path.islink(full):
            t = os.readlink(full)
            base = "" if t.startswith("/") else cur
            parts = [x for x in os.path.join(base, t).split("/") if x] + parts
            cur = ""
            # normalise . and ..
            norm = []
            for x in parts:
                if x == "..":
                    if norm: norm.pop()
                elif x != ".":
                    norm.append(x)
            parts = norm
        else:
            cur = cand
    raise SystemExit("symlink loop at " + path)

def elf(path, what):
    r = subprocess.run(["readelf", what, "-W", path], capture_output=True, text=True)
    return r.stdout

def needed(path):
    return [l.split("[", 1)[1].split("]", 1)[0] for l in elf(path, "-d").splitlines()
            if "(NEEDED)" in l]

def interp(path):
    for l in elf(path, "-l").splitlines():
        if "Requesting program interpreter:" in l:
            return l.split(":", 1)[1].strip().rstrip("]").strip()
    return None

def find_lib(name):
    for d in DIRS:
        p = inside("/" + d + "/" + name)
        if os.path.isfile(p):
            return p
    raise SystemExit("mkrecovery: %s not found in the root" % name)

libdir = os.path.join(out, "usr/lib/x86_64-linux-gnu")
done, todo, loader = set(), [], None
for t in TOOLS:
    src = inside("/" + t)
    if not os.path.isfile(src):
        raise SystemExit("mkrecovery: /%s is not in the root" % t)
    shutil.copy2(src, os.path.join(out, t))
    todo.append(src)
    loader = loader or interp(src)
for link, target in LINKS.items():
    dst = os.path.join(out, link)
    if os.path.lexists(dst):
        os.unlink(dst)
    os.symlink(target, dst)
while todo:
    for n in needed(todo.pop()):
        if n in done:
            continue
        done.add(n)
        src = find_lib(n)
        shutil.copy2(src, os.path.join(libdir, n))
        todo.append(src)
if not loader:
    raise SystemExit("mkrecovery: no program interpreter in the tools")
ld = inside(loader)
shutil.copy2(ld, os.path.join(out, loader.lstrip("/")))
shutil.copy2(ld, os.path.join(libdir, os.path.basename(loader)))
print("  tools: %d programs, %d libraries, loader %s" % (len(TOOLS), len(done), loader))
PY
ldconfig -r "$OUT" 2>/dev/null || die "ldconfig -r failed"

# Every tool has to start in the tree as it stands - a missing library
# would otherwise be found by the first person who needs a reinstall.
if [[ $(id -u) = 0 ]]; then
    for t in /usr/bin/tar /usr/bin/zstd /usr/sbin/e2fsck /usr/sbin/mkfs.ext4 /usr/sbin/fsck.vfat; do
        chroot "$OUT" "$t" --help >/dev/null 2>&1 ||
            chroot "$OUT" "$t" -V >/dev/null 2>&1 ||
            chroot "$OUT" "$t" --version >/dev/null 2>&1 ||
            die "$t does not run inside the recovery tree"
    done
    log "every tool runs inside the tree"
fi

if [[ $PAYLOAD = 0 ]]; then
    rmdir reinstall/esp reinstall
    printf 'LP recovery partition: the recovery system, without a reinstall payload.\n' > README.txt
    log "no payload (--no-payload)"
    log "tree: $(du -sh "$OUT" | cut -f1)"
    exit 0
fi

# ── the reinstall payload ────────────────────────────────────────────
#
# The whole root, as it will be on a freshly installed disk: numeric
# owners (the recovery system's passwd knows only root), extended
# attributes (file capabilities: ping, the GPU helpers) and hard links.
# --one-file-system keeps out whatever the build has mounted on it.
#
# zstd is the root's own, run through its own loader: the build machine
# need not have zstd, and the payload is then made by the same zstd
# that will unpack it.
LD="$(readelf -l -W "$ROOTFS/usr/bin/zstd" | sed -n 's/.*interpreter: \(.*\)]/\1/p')"
zstd_root() {
    "$OUT${LD}" --library-path "$OUT/usr/lib/x86_64-linux-gnu" "$OUT/usr/bin/zstd" "$@"
}
tar --create --numeric-owner --xattrs --xattrs-include='*' --acls \
    --one-file-system --sparse \
    --exclude='./proc/*' --exclude='./sys/*' --exclude='./dev/*' --exclude='./run/*' \
    --exclude='./tmp/*' --exclude='./mnt/lp-src' \
    -C "$ROOTFS" -f - . |
    zstd_root -q -T0 -10 -o reinstall/root.tar.zst
log "payload: root.tar.zst $(du -h reinstall/root.tar.zst | cut -f1)"

cp "$KERNEL" reinstall/esp/vmlinuz.efi
cp "$LPBOOT" reinstall/esp/lpboot.efi
# The normal boot's options without a root= (reinstall writes this
# disk's own in front of them).
line=""
for w in $CMDLINE; do
    case "$w" in root=*) ;; *) line="$line $w" ;; esac
done
printf '%s\n' "${line# }" > reinstall/esp/cmdline.txt

VERSION="LP"
[[ -s "$ROOTFS/etc/osname" ]] && VERSION="$(head -1 "$ROOTFS/etc/osname")"
# The desktop says what it is in os-release (desktop/branding); the
# recovery menu shows this, and "LP-zero" was the Pi image's old name.
if [[ -s "$ROOTFS/usr/lib/os-release" ]]; then
    v="$(. "$ROOTFS/usr/lib/os-release" && printf '%s' "${PRETTY_NAME:-}")"
    [[ -n "$v" ]] && VERSION="$v"
fi
VERSION="$VERSION $(date -u -r "$KERNEL" +%Y-%m-%d)"
{
    printf 'version %s\n' "$VERSION"
    printf 'lang en_US.UTF-8\n'
    for f in root.tar.zst esp/vmlinuz.efi esp/lpboot.efi esp/cmdline.txt; do
        printf 'sha256 %s %s %s\n' "$(sha256sum "reinstall/$f" | cut -d' ' -f1)" \
            "$f" "$(stat -c %s "reinstall/$f")"
    done
} > reinstall/manifest
log "manifest: $VERSION"

printf 'LP recovery partition: the recovery system and the reinstall payload.\n' > README.txt
log "tree: $(du -sh "$OUT" | cut -f1)"
