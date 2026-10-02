#!/usr/bin/env bash
#
# fetch-fonts.sh - the two faces the design spec names that Debian does
# not carry.
#
# shell-design-spec.md 1-1 gives the UI face as Pretendard and the
# terminal face as D2Coding. Neither is in Debian, and neither can be
# substituted without changing what the system looks like: Pretendard is
# the reason Korean and Latin sit on the same baseline at the same
# weight, and D2Coding is the reason a Korean character in the terminal
# is exactly two columns wide. Noto Sans CJK is the fallback in the theme
# for the case where this script could not run, not an equivalent.
#
# Both are OFL. They are fetched rather than committed because they are
# 68MB of zip for 15MB of font, and a repository is a bad place to keep
# either number. The hashes below are what makes fetching them safe.
#
#   ./tools/fetch-fonts.sh <destdir>
#
# Downloads land in $LP_FONT_CACHE (default /home/user/kernel-work/fontcache)
# and are reused, so a rebuild is offline.
set -euo pipefail

DEST="${1:?사용법: fetch-fonts.sh <설치할 곳>}"
CACHE="${LP_FONT_CACHE:-/home/user/kernel-work/fontcache}"

PRETENDARD_URL='https://github.com/orioncactus/pretendard/releases/download/v1.3.9/Pretendard-1.3.9.zip'
PRETENDARD_SHA=04be351a74d6bf7d60c480a3087e51d185485d35a52023142af1df19eb8c428a

D2CODING_URL='https://github.com/naver/d2codingfont/releases/download/VER1.3.2/D2Coding-Ver1.3.2-20180524.zip'
D2CODING_SHA=0f1c9192eac7d56329dddc620f9f1666b707e9c8ed38fe1f988d0ae3e30b24e6

log() { printf '  %s\n' "$*"; }

get() {
    local url="$1" want="$2" out="$3"
    if [[ -f "$out" ]] && printf '%s  %s' "$want" "$out" | sha256sum -c - >/dev/null 2>&1; then
        log "$(basename "$out") 는 이미 받아 두었습니다"
        return 0
    fi
    log "$(basename "$out") 받는 중"
    curl -fsSL -o "$out.part" "$url"
    if ! printf '%s  %s' "$want" "$out.part" | sha256sum -c - >/dev/null 2>&1; then
        rm -f "$out.part"
        printf '오류: %s 의 해시가 맞지 않습니다\n' "$url" >&2
        exit 1
    fi
    mv "$out.part" "$out"
}

mkdir -p "$CACHE" "$DEST/pretendard" "$DEST/d2coding"

get "$PRETENDARD_URL" "$PRETENDARD_SHA" "$CACHE/pretendard.zip"
get "$D2CODING_URL"   "$D2CODING_SHA"   "$CACHE/d2coding.zip"

# The variable font only. The zip also ships nine static weights, and the
# spec asks for three of them (400/500/600), which a variable font covers
# in one sixth of the space.
unzip -j -o -q "$CACHE/pretendard.zip" \
    'public/variable/PretendardVariable.ttf' -d "$DEST/pretendard"

# Regular and Bold, not the .ttc. A collection saves a megabyte and
# fontconfig on this base indexes the faces inside one inconsistently -
# the terminal ends up with a face that has no Korean in it.
unzip -j -o -q "$CACHE/d2coding.zip" \
    'D2Coding/D2Coding-Ver1.3.2-20180524.ttf' \
    'D2Coding/D2CodingBold-Ver1.3.2-20180524.ttf' -d "$DEST/d2coding"

log "$(du -sh "$DEST" | cut -f1)  $DEST"
