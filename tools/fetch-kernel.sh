#!/usr/bin/env bash
#
# fetch-kernel.sh - 커널 소스를 받는다.
#
# 이 저장소에는 리눅스 소스가 들어 있지 않다. 1GB 가 넘고, 우리가 고친
# 것은 몇 줄뿐이기 때문이다. 우리 것은 커널 설정(kernel/lp-zero.config),
# 고정 커밋에 아직 없는 수정(kernel/patches/), 그리고 그것들을 적용하는
# 스크립트뿐이다. 패치는 kernel/build.sh 가 받은 소스에 한 번 적용한다.
#
# 받는 것은 라즈베리파이 재단의 rpi-6.12.y 다. 순정 리눅스가 아니라
# 이쪽인 이유는 VideoCore GPU, 카메라, 그리고 Zero 2 W 의 디바이스 트리가
# 여기에만 있기 때문이다.
#
# 커밋을 하나로 고정한다. 브랜치 끝을 따라가면 어제 되던 빌드가 오늘
# 안 되는 일이 생기고, 그때 우리 잘못인지 저쪽 변경인지 알 방법이 없다.
#
# 사용법:
#   ./tools/fetch-kernel.sh              고정된 커밋을 받는다
#   ./tools/fetch-kernel.sh --update     브랜치 최신으로 옮기고 커밋을 다시 적는다

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"

KERNEL_URL="https://github.com/raspberrypi/linux.git"
KERNEL_BRANCH="rpi-6.12.y"
PIN_FILE="${REPO_ROOT}/kernel/linux.commit"

die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
log()  { printf '  %s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }

UPDATE=false
[[ "${1:-}" == "--update" ]] && UPDATE=true

command -v git >/dev/null || die "git 이 필요합니다"

[[ -f "$PIN_FILE" ]] || die "${PIN_FILE} 이 없습니다"
PIN="$(tr -d '[:space:]' < "$PIN_FILE")"
[[ -n "$PIN" ]] || die "${PIN_FILE} 이 비어 있습니다"

mkdir -p "$LPZERO_WORK"

if [[ ! -d "${LINUX_SRC}/.git" ]]; then
    step "커널 소스 받기 (${KERNEL_BRANCH})"
    log "1GB 가 넘습니다. 몇 분 걸립니다."
    # --depth 로 얕게 받되, 고정 커밋이 그 안에 없을 수 있으므로
    # 브랜치 전체가 아니라 그 커밋을 직접 요청한다.
    git init -q "$LINUX_SRC"
    git -C "$LINUX_SRC" remote add origin "$KERNEL_URL"
    if ! git -C "$LINUX_SRC" fetch -q --depth 1 origin "$PIN" 2>/dev/null; then
        log "서버가 커밋 직접 요청을 거부했습니다 - 브랜치를 받습니다"
        git -C "$LINUX_SRC" fetch -q --depth 200 origin "$KERNEL_BRANCH"
    fi
    git -C "$LINUX_SRC" checkout -q "$PIN" 2>/dev/null \
        || die "고정 커밋 ${PIN} 을 찾지 못했습니다. --update 로 다시 고정하세요."
    log "받음: ${LINUX_SRC}"
fi

if $UPDATE; then
    step "브랜치 최신으로 (${KERNEL_BRANCH})"
    git -C "$LINUX_SRC" fetch -q --depth 200 origin "$KERNEL_BRANCH"
    git -C "$LINUX_SRC" checkout -q FETCH_HEAD
    NEW="$(git -C "$LINUX_SRC" rev-parse HEAD)"
    printf '%s\n' "$NEW" > "$PIN_FILE"
    log "고정 커밋을 ${NEW} 로 바꿨습니다"
    log "kernel/linux.commit 을 커밋하세요"
else
    CUR="$(git -C "$LINUX_SRC" rev-parse HEAD 2>/dev/null || echo '')"
    if [[ "$CUR" != "$PIN" ]]; then
        step "고정 커밋으로 되돌리기"
        git -C "$LINUX_SRC" fetch -q --depth 200 origin "$KERNEL_BRANCH" || true
        git -C "$LINUX_SRC" checkout -q "$PIN" \
            || die "고정 커밋 ${PIN} 을 찾지 못했습니다"
    fi
fi

step "결과"
VER=$(sed -n 's/^VERSION = //p;'  "${LINUX_SRC}/Makefile" | head -1)
PAT=$(sed -n 's/^PATCHLEVEL = //p' "${LINUX_SRC}/Makefile" | head -1)
SUB=$(sed -n 's/^SUBLEVEL = //p'   "${LINUX_SRC}/Makefile" | head -1)
log "리눅스 ${VER}.${PAT}.${SUB}"
log "$(git -C "$LINUX_SRC" rev-parse HEAD)"
log "${LINUX_SRC}"
echo ""
echo "  이제 ./kernel/build.sh 를 돌리면 됩니다."
