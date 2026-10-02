#!/usr/bin/env bash
#
# mkrepo.sh - a signed apt repository of LP's own packages.
#
# lp-base (and whatever lp-* packages follow it) is how the desktop gets
# new versions of this OS: the machine already runs apt for Debian, so
# the same `apt update && apt upgrade` - or the Software app's "Update
# all" - should bring LP's init, shell and commands up to date too. That
# needs a repository apt will trust, which means a Release file signed
# with a key the machine already has.
#
#   ./tools/mkrepo.sh              dist/debs/*.deb -> dist/apt/
#   LP_APT_URL=https://… ./tools/mkrepo.sh   also writes the sources file
#                                             for that address
#
# Layout (flat enough to serve from any static web host):
#
#   dist/apt/pool/main/<first letter>/<package>/<file>.deb
#   dist/apt/dists/lp/{InRelease,Release,Release.gpg}
#   dist/apt/dists/lp/main/binary-<arch>/Packages{,.gz,.xz}
#   dist/apt/lp-archive-keyring.gpg     the public key, binary
#
# ── The key ──
#
# The signing key lives in keys/apt-gnupg/ (keys/ is gitignored, like
# the update key beside it) and is made on first use. Only its PUBLIC
# half leaves that directory: desktop/apt/lp-archive-keyring.gpg is
# committed and installed as /usr/share/keyrings/lp-archive-keyring.gpg,
# and desktop/apt/lp.sources names it with Signed-By - so this key can
# vouch for LP's repository and nothing else, not for Debian's.
#
# Lose keys/apt-gnupg/ and every installed machine stops trusting new
# releases until it gets the new public key by hand. Back it up.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
DEBS="${REPO_ROOT}/dist/debs"
OUT="${REPO_ROOT}/dist/apt"
PUB_DIR="${REPO_ROOT}/desktop/apt"
export GNUPGHOME="${REPO_ROOT}/keys/apt-gnupg"
SUITE=lp
COMPONENT=main
ARCHES=(amd64 arm64 armhf)
KEY_UID="LP Archive Signing Key <lp-archive@localhost>"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
command -v dpkg-scanpackages >/dev/null || die "dpkg-scanpackages missing (apt install dpkg-dev)"
command -v gpg >/dev/null || die "gpg missing"
compgen -G "${DEBS}/*.deb" >/dev/null || die "no packages in ${DEBS} - run tools/mkdeb.sh first"

# ── the key ──────────────────────────────────────────────────────────
mkdir -p "$GNUPGHOME"
chmod 700 "$GNUPGHOME"
if ! gpg --batch --list-secret-keys "$KEY_UID" >/dev/null 2>&1; then
    echo "  making the archive signing key (keys/apt-gnupg, not committed)"
    gpg --batch --quiet --passphrase '' \
        --quick-generate-key "$KEY_UID" ed25519 sign never
fi
FPR=$(gpg --batch --with-colons --list-secret-keys "$KEY_UID" |
      awk -F: '$1 == "fpr" { print $10; exit }')
[[ -n "$FPR" ]] || die "could not read the signing key's fingerprint"

# ── the pool ─────────────────────────────────────────────────────────
rm -rf "$OUT"
mkdir -p "$OUT/pool/$COMPONENT"
for deb in "$DEBS"/*.deb; do
    pkg=$(dpkg-deb -f "$deb" Package)
    dir="$OUT/pool/$COMPONENT/${pkg:0:1}/$pkg"
    mkdir -p "$dir"
    cp -a "$deb" "$dir/"
done

# ── the indexes ──────────────────────────────────────────────────────
cd "$OUT"
for arch in "${ARCHES[@]}"; do
    d="dists/$SUITE/$COMPONENT/binary-$arch"
    mkdir -p "$d"
    # -a limits the index to this architecture (plus "all").
    dpkg-scanpackages -m -a "$arch" pool /dev/null 2>/dev/null > "$d/Packages"
    gzip -9nkf "$d/Packages"
    xz -9kf "$d/Packages"
done

# Release: what the index files are and their hashes. apt refuses a
# repository whose Release does not list, with the right size and
# SHA256, every index it fetches.
{
    echo "Origin: LP"
    echo "Label: LP"
    echo "Suite: $SUITE"
    echo "Codename: $SUITE"
    echo "Architectures: ${ARCHES[*]}"
    echo "Components: $COMPONENT"
    echo "Description: LP's own packages - init, shell, commands and desktop"
    echo "Date: $(LC_ALL=C date -Ru)"
    echo "Acquire-By-Hash: no"
    for algo in MD5Sum:md5sum SHA256:sha256sum; do
        echo "${algo%%:*}:"
        (cd "dists/$SUITE" && find "$COMPONENT" -type f | sort | while read -r f; do
            printf ' %s %16d %s\n' "$(${algo#*:} "$f" | cut -d' ' -f1)" "$(stat -c %s "$f")" "$f"
        done)
    done
} > "dists/$SUITE/Release"

gpg --batch --yes --quiet --local-user "$FPR" --clearsign \
    -o "dists/$SUITE/InRelease" "dists/$SUITE/Release"
gpg --batch --yes --quiet --local-user "$FPR" --armor --detach-sign \
    -o "dists/$SUITE/Release.gpg" "dists/$SUITE/Release"

# ── the public half ──────────────────────────────────────────────────
mkdir -p "$PUB_DIR"
gpg --batch --export "$FPR" > "$OUT/lp-archive-keyring.gpg"
cp -a "$OUT/lp-archive-keyring.gpg" "$PUB_DIR/lp-archive-keyring.gpg"
if [[ -n "${LP_APT_URL:-}" ]]; then
    cat > "$PUB_DIR/lp.sources" <<EOF
# LP's own packages. Installed as /etc/apt/sources.list.d/lp.sources.
# Signed-By limits the LP key to this repository.
Types: deb
URIs: ${LP_APT_URL%/}/
Suites: $SUITE
Components: $COMPONENT
Signed-By: /usr/share/keyrings/lp-archive-keyring.gpg
EOF
fi

# Prove it before anyone else has to: the signature must verify with
# nothing but the public key we are about to ship.
gpgv --keyring "$OUT/lp-archive-keyring.gpg" "dists/$SUITE/InRelease" 2>/dev/null ||
    die "InRelease does not verify with the exported public key"
n=$(find pool -name '*.deb' | wc -l)
printf 'apt repository: %s (%d packages, suite %s, key %s)\n' "$OUT" "$n" "$SUITE" "${FPR: -16}"
