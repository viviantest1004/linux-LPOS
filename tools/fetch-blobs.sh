#!/usr/bin/env bash
#
# fetch-blobs.sh - 직접 만들 수 없는 클로즈드 바이너리만 받아온다.
#
# 왜 이것만은 못 만드나:
#   BCM2710 은 ARM 이 아니라 VideoCore GPU 가 먼저 부팅한다. 실리콘에
#   박힌 GPU 부트ROM 이 SD카드에서 아래 파일들을 읽어 SDRAM 을 초기화하고
#   ARM 코어를 릴리즈한다. Broadcom 이 소스를 공개하지 않았고 대체 불가다.
#
#     bootcode.bin  2단계 부트로더 (GPU L2 캐시에서 실행)
#     start.elf     GPU 펌웨어 본체. SDRAM 초기화 + config.txt 해석 + ARM 기동
#     fixup.dat     start.elf 의 메모리 분할 재배치 정보
#
#   이 3개가 ARM 을 0x80000 으로 점프시키는 순간부터는 전부 우리 코드다.
#
# 사용법:
#   ./tools/fetch-blobs.sh            # 기본 ref(stable)에서 받기
#   FW_REF=1.20240529 ./tools/fetch-blobs.sh
#   ./tools/fetch-blobs.sh --verify   # 받지 않고 체크섬만 검증

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BLOB_DIR="${REPO_ROOT}/blobs"
SUMS_FILE="${REPO_ROOT}/tools/blobs.sha256"

FW_REF="${FW_REF:-stable}"
BASE_URL="https://raw.githubusercontent.com/raspberrypi/firmware/${FW_REF}/boot"

# Pi Zero 2 W (BCM2710) 는 start.elf 계열을 쓴다.
# start4.elf 는 Pi 4 (BCM2711) 전용이므로 받지 않는다.
BLOBS=(bootcode.bin start.elf fixup.dat)

log()  { printf '  %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

verify_only=false
[[ "${1:-}" == "--verify" ]] && verify_only=true

if $verify_only; then
    [[ -f "$SUMS_FILE" ]] || die "체크섬 파일이 없습니다: $SUMS_FILE"
    cd "$BLOB_DIR" || die "blobs/ 디렉터리가 없습니다. 먼저 받으세요."
    sha256sum -c "$SUMS_FILE"
    exit 0
fi

command -v curl >/dev/null 2>&1 || die "curl 이 필요합니다"

mkdir -p "$BLOB_DIR"
echo "raspberrypi/firmware @ ${FW_REF} 에서 받는 중..."

for f in "${BLOBS[@]}"; do
    log "GET  ${f}"
    # --fail: HTTP 4xx/5xx 를 에러로 (404 HTML 을 blob 으로 저장하는 사고 방지)
    curl --fail --location --silent --show-error \
         --retry 4 --retry-delay 2 --retry-all-errors \
         --output "${BLOB_DIR}/${f}" \
         "${BASE_URL}/${f}" \
        || die "${f} 다운로드 실패"
done

echo ""
echo "받은 파일:"
( cd "$BLOB_DIR" && ls -lh "${BLOBS[@]}" | awk '{printf "  %-16s %s\n", $9, $5}' )

# 다음 번 무결성 검증을 위해 체크섬 기록
( cd "$BLOB_DIR" && sha256sum "${BLOBS[@]}" ) > "$SUMS_FILE"
echo ""
echo "체크섬 기록: tools/blobs.sha256"
echo "  (다음부터는 ./tools/fetch-blobs.sh --verify 로 무결성 확인 가능)"
