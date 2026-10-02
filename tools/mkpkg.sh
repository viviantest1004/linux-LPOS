#!/usr/bin/env bash
#
# mkpkg.sh - 패키지 하나를 만들고, 그 아키텍처의 저장소 인덱스를 다시 쓴다.
#
# 패키지는 압축하지 않은 평범한 tar 다. 우리만의 형식이 아니라서
# 아무 기계에서나 tar 로 열어볼 수 있고, 기기 쪽에서 풀어야 하는 코드도
# 절반이면 된다 (userland/pkg/pkg.c 참고).
#
# tar 안의 경로는 상대경로여야 하고, 기기에서는 /data 아래로 풀린다.
# bin/foo 를 담으면 /data/bin/foo 가 되고, /data/bin 은 이미 PATH 에 있다.
# 절대경로나 ".." 이 들어 있으면 기기 쪽 pkg 가 통째로 거부한다.
#
# ── 저장소는 아키텍처별로 나뉜다 ───────────────────────────────────
#
#   repo/arm64/index   repo/arm64/hexdump-1.0.tar
#   repo/armv6/index   repo/armv6/hexdump-1.0.tar
#   repo/amd64/index   repo/amd64/hexdump-1.0.tar
#
# 하나로 합칠 수 없다. 패키지에는 컴파일된 프로그램이 들어 있고, 섞인
# 인덱스는 Pi Zero W 에 aarch64 바이너리를 건네준다 - 설치는 멀쩡히
# 되고 exec 에서 죽는다. 기기 쪽 pkg 는 자기가 어느 기계용으로
# 빌드됐는지 알고 있어서, 자기 아키텍처 디렉터리만 본다.
#
# 사용법:
#   tools/mkpkg.sh <이름> <버전> <디렉터리> <아키텍처> [저장소디렉터리]
#   tools/mkpkg.sh --index <아키텍처> [저장소디렉터리]
#
# 예:
#   mkdir -p stage/bin && cp myprog stage/bin/
#   tools/mkpkg.sh myprog 1.0 stage arm64
#   python3 -m http.server -d repo 8000
#   # 기기에서: pkg repo http://<주소>:8000 ; pkg install myprog

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DEFAULT="${REPO_ROOT}/repo"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
log() { printf '  %s\n' "$*"; }

check_arch() {
    case "$1" in
        arm64|armv6|amd64) ;;
        *) die "아키텍처는 arm64 / armv6 / amd64 중 하나여야 합니다: $1" ;;
    esac
}

# 인덱스 한 줄: 이름 버전 크기 sha256 파일명
# 기기 쪽 index_find() 가 공백으로 잘라 읽으므로 필드에 공백이 있으면 안 된다.
write_index() {
    local dir="$1"
    local idx="${dir}/index"
    {
        printf '# name version size sha256 file\n'
        # 정렬해서 쓴다. 그래야 같은 내용이면 인덱스도 같은 파일이 되고,
        # 저장소를 diff 해서 무엇이 바뀌었는지 볼 수 있다.
        local t
        for t in $(cd "$dir" && ls -1 ./*.tar 2>/dev/null | sort); do
            t="${t#./}"
            local base="${t%.tar}"
            local name="${base%-*}"
            local ver="${base##*-}"
            [[ "$name" == "$base" ]] && die "$t: 이름-버전.tar 형식이 아닙니다"
            printf '%s %s %s %s %s\n' \
                "$name" "$ver" \
                "$(stat -c%s "${dir}/${t}")" \
                "$(sha256sum "${dir}/${t}" | cut -d' ' -f1)" \
                "$t"
        done
    } > "$idx"
    log "index  $(grep -cv '^#' "$idx") 개 패키지  ($idx)"
}

if [[ "${1:-}" == "--index" ]]; then
    ARCH="${2:-}"
    [[ -n "$ARCH" ]] || die "아키텍처를 지정하세요: --index <arm64|armv6|amd64>"
    check_arch "$ARCH"
    OUT="${3:-$OUT_DEFAULT}/$ARCH"
    [[ -d "$OUT" ]] || die "저장소 디렉터리가 없습니다: $OUT"
    write_index "$OUT"
    exit 0
fi

[[ $# -ge 4 ]] || {
    printf '사용법: mkpkg.sh <이름> <버전> <디렉터리> <아키텍처> [저장소디렉터리]\n' >&2
    printf '        mkpkg.sh --index <아키텍처> [저장소디렉터리]\n' >&2
    exit 2
}

NAME="$1"; VERSION="$2"; SRC="$3"; ARCH="$4"; OUT="${5:-$OUT_DEFAULT}/$4"
check_arch "$ARCH"

# 이름과 버전에는 공백도 '-' 도 들어가면 안 된다. 파일 이름을
# "이름-버전.tar" 로 만들고 다시 갈라 읽기 때문이다.
[[ "$NAME"    =~ ^[A-Za-z0-9_.]+$ ]] || die "이름에 쓸 수 없는 글자: $NAME"
[[ "$VERSION" =~ ^[A-Za-z0-9_.]+$ ]] || die "버전에 쓸 수 없는 글자: $VERSION"
[[ -d "$SRC" ]] || die "디렉터리가 없습니다: $SRC"

mkdir -p "$OUT"
TAR="${OUT}/${NAME}-${VERSION}.tar"

# --format=ustar: 기기 쪽 tar 는 GNU 확장 헤더를 읽지 않는다.
# --numeric-owner + --owner/--group: 빌드한 사람의 계정 이름이 아카이브에
#   들어가지 않게. 기기에는 그 사용자가 없다.
# --mtime + --sort: 같은 내용이면 같은 tar 가 나오게 (재현 가능한 빌드).
( cd "$SRC" && tar --format=ustar --numeric-owner \
      --owner=0 --group=0 --mtime='@0' --sort=name \
      -cf "$TAR" . )

# 기기 쪽 pkg 가 거부할 것을 여기서 먼저 잡는다. 만든 다음에 기기에서
# 실패하는 것보다, 만들 때 실패하는 편이 낫다.
BAD=$(tar -tf "$TAR" | grep -E '^/|(^|/)\.\.(/|$)' || true)
[[ -z "$BAD" ]] || die "절대경로나 '..' 가 들어 있습니다:
$BAD"

log "$(basename "$TAR")  $(stat -c%s "$TAR") bytes, $(tar -tf "$TAR" | grep -cv '/$') 개 파일  [$ARCH]"
write_index "$OUT"
