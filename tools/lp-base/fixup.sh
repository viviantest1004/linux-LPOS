# fixup.sh - keep dpkg's idea of /bin and the disk in agreement.
#
# This file is not run on its own. mkdeb.sh pastes it into lp-base's
# maintainer scripts and ships it as /usr/lib/lp-base/fixup, so the
# same code runs when the package goes in and after every dpkg run.
#
# ── The problem it exists for ──
#
# The desktop root is a Debian bookworm base with our userland in /bin.
# The base came out of debootstrap merged - /bin a symlink into /usr/bin
# - and the first desktop builds replaced that symlink with a real /bin
# holding only our commands. dpkg still believes it owns 105 paths under
# /bin. Twenty-eight of them had become our programs, so the next
# `apt upgrade` of coreutils would have written GNU ls over ours. The
# other seventy-seven were simply gone - /bin/bash among them - and every
# `#!/bin/bash` script on the machine failed with "not found".
#
# ── The layout it restores ──
#
#   /bin/X             ours, for every command this OS writes itself
#   /bin/Y             Debian's, for what Debian puts in /bin and we do
#                      not replace (bash, dash, ...), moved back here
#   /usr/bin/Y -> /bin/Y           so /usr/bin/bash still works
#   /usr/lib/lp-base/debian/bin/X  Debian's copy of what we replace,
#                                  where dpkg-divert sends its upgrades
#   /usr/bin/X -> that copy        so GNU ls is still at /usr/bin/ls
#
# With apt told to run dpkg with /usr/bin first (DPkg::Path, shipped
# alongside), Debian's maintainer scripts get the GNU tools they were
# written against, and a person at the prompt gets ours.
#
# Everything below is plain POSIX sh for dash, and every tool it calls
# is looked up in /usr/bin first. Our own shell is not assumed anywhere:
# this code is what makes /bin/sh dash again.

PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

# dash remembers where it found each command. This code moves the very
# commands it runs - /usr/bin/ln becomes /bin/ln half way through - and
# a remembered path that no longer exists is "ln: not found", exit 127,
# a failed preinst. So every move is followed by forgetting.
lpb_rehash() { hash -r 2>/dev/null || true; }

LPB_DIVERT_ROOT=/usr/lib/lp-base/debian
LPB_INFO=/var/lib/dpkg/info
LPB_QUIET=${LPB_QUIET:-0}
LPB_TMP=

lpb_say()  { [ "$LPB_QUIET" = 1 ] || echo "lp-base: $*"; }
lpb_warn() { echo "lp-base: warning: $*" >&2; }

# One pass over dpkg's database instead of a dpkg-query per path: with
# two hundred commands that is the difference between a second and a
# minute.
#   owners  "path pkg" for every /bin/*, /sbin/*, /usr/bin/* and /usr/sbin/*
#           some package lists (/sbin/init is one we divert)
#   md5     "md5 path" for every /bin/* file some package ships
#   divs    "from to pkg" for every diversion
lpb_index() {
    LPB_TMP=$(mktemp -d /tmp/lp-base.XXXXXX) || return 1
    for l in "$LPB_INFO"/*.list; do
        pkg=${l##*/}; pkg=${pkg%.list}; pkg=${pkg%%:*}
        [ "$pkg" = lp-base ] && continue
        grep -h '^/\(usr/\)\{0,1\}s\{0,1\}bin/[^/]*$' "$l" 2>/dev/null |
            sed "s|\$| $pkg|"
    done > "$LPB_TMP/owners"
    cat "$LPB_INFO"/*.md5sums 2>/dev/null |
        sed -n 's|^\([0-9a-f]*\)  \(bin/[^/]*\)$|\1 /\2|p' > "$LPB_TMP/md5"
    lpb_reindex_divs
}

lpb_reindex_divs() {
    dpkg-divert --list 2>/dev/null |
        sed -n 's|^diversion of \([^ ]*\) to \([^ ]*\) by \([^ ]*\)$|\1 \2 \3|p' \
        > "$LPB_TMP/divs"
}

lpb_done() { [ -n "$LPB_TMP" ] && rm -rf "$LPB_TMP"; LPB_TMP=; }

lpb_owner()    { awk -v p="$1" '$1 == p { print $2; exit }' "$LPB_TMP/owners"; }
lpb_md5_of()   { awk -v p="$1" '$2 == p { print $1; exit }' "$LPB_TMP/md5"; }
lpb_diverter() { awk -v p="$1" '$1 == p { print $3; exit }' "$LPB_TMP/divs"; }
lpb_md5()      { md5sum "$1" 2>/dev/null | cut -d' ' -f1; }

# Does PATH hold what dpkg installed there? With a recorded md5 the
# content has to match; without one the entry is a symlink in the
# package, and any symlink will do.
lpb_is_debian() {
    if [ -n "$2" ]; then
        [ -f "$1" ] && [ ! -L "$1" ] && [ "$(lpb_md5 "$1")" = "$2" ]
    else
        [ -L "$1" ]
    fi
}

# Replace LINK with a symlink to TARGET in one rename, so there is no
# instant where the name is missing - it may be the mv we are running.
lpb_link() {
    ln -sf "$1" "$2.lp-base-new" && mv -f "$2.lp-base-new" "$2"
    lpb_rehash
}

# Put Debian's /bin back where dpkg thinks it is.
lpb_repair_bin() {
    [ -d /bin ] && [ ! -L /bin ] || return 0
    fixed=0; broken=0
    for p in $(awk '$1 ~ /^\/bin\// { print $1 }' "$LPB_TMP/owners" | sort -u); do
        div=$(lpb_diverter "$p")
        # Ours to manage: Debian's copy is under LPB_DIVERT_ROOT.
        [ "$div" = lp-base ] && continue
        want=$(lpb_md5_of "$p")
        lpb_is_debian "$p" "$want" && continue
        alt=/usr/bin/${p#/bin/}
        if lpb_is_debian "$alt" "$want"; then
            rm -f "$p"
            mv "$alt" "$p"
            lpb_rehash
            [ -n "$(lpb_owner "$alt")" ] || ln -s "$p" "$alt"
            fixed=$((fixed + 1))
        elif [ -n "$div" ]; then
            # Diverted by somebody else - dash diverts /bin/sh and sets
            # it up in its postinst. Handled below.
            :
        else
            pkg=$(lpb_owner "$p")
            lpb_warn "$p is not what $pkg installed: apt install --reinstall $pkg"
            broken=$((broken + 1))
        fi
    done
    # /bin/sh is dash's, made in dash's postinst. If something replaced
    # the symlink with a file, put dash back.
    if [ ! -L /bin/sh ] && [ -x /bin/dash ]; then
        lpb_link dash /bin/sh
        fixed=$((fixed + 1))
    fi
    [ "$fixed" = 0 ] || lpb_say "put $fixed of Debian's files back in /bin"
    [ "$broken" = 0 ] || lpb_warn "$broken paths under /bin differ from dpkg's record"
    return 0
}

# /usr/bin/X -> Debian's copy of a /bin/X we divert, unless a package
# owns /usr/bin/X itself.
lpb_compat_link() {
    case "$1" in /bin/*) ;; *) return 0 ;; esac
    alt=/usr/bin/${1#/bin/}
    [ -n "$(lpb_owner "$alt")" ] && return 0
    [ -L "$alt" ] || [ ! -e "$alt" ] || return 0
    [ "$(readlink "$alt" 2>/dev/null)" = "$2" ] || lpb_link "$2" "$alt"
}

# Divert one path we ship away from the package that owns it.
#
# Not dpkg-divert --rename. That moves Debian's file away at once, and
# for coreutils - Essential - it leaves /bin/ln or /bin/mkdir missing
# until our package is unpacked: dpkg warns that this is dangerous, and
# this very loop, which runs ln and mkdir, is the first thing it breaks.
# Instead Debian's file is COPIED to the diversion target, /usr/bin/X is
# pointed at the copy, and only then is the diversion recorded without a
# rename. /bin/X is never absent: it is Debian's until dpkg unpacks ours
# over it in one rename.
lpb_divert() {
    p=$1
    div=$(lpb_diverter "$p")
    [ "$div" = lp-base ] && return 0
    if [ -n "$div" ]; then
        lpb_warn "$p is diverted by $div; keeping theirs"
        return 0
    fi
    [ -n "$(lpb_owner "$p")" ] || return 0
    to=$LPB_DIVERT_ROOT$p
    mkdir -p "${to%/*}"
    if [ -e "$p" ] || [ -L "$p" ]; then
        cp -a "$p" "$to.lp-base-new" && mv -f "$to.lp-base-new" "$to"
    fi
    lpb_compat_link "$p" "$to"
    dpkg-divert --quiet --package lp-base --no-rename \
        --divert "$to" --add "$p" || return 1
    echo "$p $to lp-base" >> "$LPB_TMP/divs"
}

lpb_compat_links() {
    awk '$3 == "lp-base" { print $1, $2 }' "$LPB_TMP/divs" |
    while read -r from to; do
        [ -e "$to" ] || [ -L "$to" ] || continue
        lpb_compat_link "$from" "$to"
    done
    return 0
}

# Take one of our diversions away again and give Debian its file back:
# the same order in reverse, copy first, so /bin/X always holds a
# working program.
lpb_undivert() {
    p=$1
    [ "$(lpb_diverter "$p")" = lp-base ] || return 0
    to=$LPB_DIVERT_ROOT$p
    if [ -e "$to" ] || [ -L "$to" ]; then
        cp -a "$to" "$p.lp-base-new" && mv -f "$p.lp-base-new" "$p"
        lpb_rehash
    fi
    dpkg-divert --quiet --package lp-base --no-rename \
        --divert "$to" --remove "$p"
    case "$p" in
        /bin/*)
            alt=/usr/bin/${p#/bin/}
            if [ -L "$alt" ] && [ -z "$(lpb_owner "$alt")" ]; then
                lpb_link "$p" "$alt"
            fi ;;
    esac
    rm -f "$to"
    lpb_rehash
    awk -v p="$p" '$1 != p' "$LPB_TMP/divs" > "$LPB_TMP/divs.new" &&
        mv -f "$LPB_TMP/divs.new" "$LPB_TMP/divs"
}

lpb_our_diversions() {
    awk '$3 == "lp-base" { print $1 }' "$LPB_TMP/divs"
}
