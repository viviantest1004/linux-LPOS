#!/usr/bin/env bash
#
# mkweb.sh - build the download page from the files it offers.
#
# The page carries three sizes and three sha256 sums. Written by hand
# they go stale the first time somebody rebuilds an image and forgets,
# and a checksum that does not match is worse than no checksum: the
# person downloading cannot tell a corrupt transfer from a stale page.
# That has already happened once here, to SHA256SUMS.txt.
#
# So the numbers are read out of dist/ every time. web/template.html is
# the part people edit; index.html at the repository root is the result
# and is overwritten without warning.
#
# ── Why the root, and why it is committed ──
# GitHub Pages serves this repository as it is. The page has to sit at
# the root because that is where Pages looks, and it has to be committed
# because Pages publishes what is in the branch - there is no build step
# to run it. So this is the rare generated file that belongs in git.
# mkdist.sh runs it, so rebuilding the images updates the page in the
# same breath and the two cannot drift apart.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && cd .. && pwd)"
DIST="${REPO_ROOT}/dist"
OUT="${REPO_ROOT}"
TEMPLATE="${REPO_ROOT}/web/template.html"

# The repository this page links to: REPO_URL, or this checkout's origin.
REPO_URL="${REPO_URL:-$( (git -C "$REPO_ROOT" remote get-url origin 2>/dev/null || true) |
    sed -e 's#^git@github.com:#https://github.com/#' -e 's#//[^/@]*@#//#' -e 's#\.git$##')}"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
[ -n "$REPO_URL" ] || die "set REPO_URL to the repository's https:// address"

PI="linux-LP_arm64_Zero2W.img.xz"
ZEROW="linux-LP_armv6_ZeroW.img.xz"
AMD="linux-LP_amd64.img.xz"
UTM="test_a_123_LPzero2W_linux-utm.zip"

for f in "$PI" "$ZEROW" "$AMD" "$UTM"; do
    [[ -f "${DIST}/${f}" ]] || die "dist/${f} 가 없습니다. ./tools/mkdist.sh 를 먼저 실행하세요."
done

# "31 MB", "25 MB" - one decimal is more precision than anybody wants
# when deciding whether to click.
human() {
    local bytes="$1"
    awk -v b="$bytes" 'BEGIN {
        if (b >= 1073741824) printf "%.1f GB", b/1073741824;
        else if (b >= 1048576) printf "%.0f MB", b/1048576;
        else printf "%.0f KB", b/1024;
    }'
}

hash_of() { sha256sum "${DIST}/$1" | cut -d' ' -f1; }
size_of() { human "$(stat -c%s "${DIST}/$1")"; }

# The build date comes from the image, not from today: running this
# script again tomorrow must not claim the images are a day newer.
BUILD_DATE="$(date -u -r "${DIST}/${AMD}" '+%Y-%m-%d')"

sed -e "s|@PI_NAME@|${PI}|g" \
    -e "s|@ZEROW_NAME@|${ZEROW}|g" \
    -e "s|@ZEROW_SIZE@|$(size_of "$ZEROW")|g" \
    -e "s|@ZEROW_HASH@|$(hash_of "$ZEROW")|g" \
    -e "s|@PI_SIZE@|$(size_of "$PI")|g" \
    -e "s|@PI_HASH@|$(hash_of "$PI")|g" \
    -e "s|@AMD_NAME@|${AMD}|g" \
    -e "s|@AMD_SIZE@|$(size_of "$AMD")|g" \
    -e "s|@AMD_HASH@|$(hash_of "$AMD")|g" \
    -e "s|@UTM_NAME@|${UTM}|g" \
    -e "s|@UTM_SIZE@|$(size_of "$UTM")|g" \
    -e "s|@UTM_HASH@|$(hash_of "$UTM")|g" \
    -e "s|@REPO_URL@|${REPO_URL}|g" \
    -e "s|@REPO_SHORT@|${REPO_URL#https://}|g" \
    -e "s|@BUILD_DATE@|${BUILD_DATE}|g" \
    "$TEMPLATE" > "${OUT}/index.html"

# A placeholder left behind means a silently wrong page - a download
# button pointing at "files/@PI_NAME@". Refuse to ship that.
if grep -q '@[A-Z_]*@' "${OUT}/index.html"; then
    grep -o '@[A-Z_]*@' "${OUT}/index.html" | sort -u >&2
    die "채우지 못한 자리가 남았습니다 (위 목록)"
fi

printf '  index.html  %s bytes\n' "$(stat -c%s "${OUT}/index.html")"
printf '  %s  %s\n' "$(size_of "$PI")"  "$PI"
printf '  %s  %s\n' "$(size_of "$AMD")" "$AMD"
printf '  %s  %s\n' "$(size_of "$UTM")" "$UTM"
