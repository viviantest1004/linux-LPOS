#!/usr/bin/env bash
#
# run-qemu.sh - QEMU 에서 펌웨어를 돌린다. 실기 없이 개발할 때 쓴다.
#
# 중요: QEMU 는 VideoCore GPU 를 흉내내지 않는다. 그래서 SD카드
# 이미지(sdcard/lp-zero.img)는 QEMU 에서 부팅되지 않는다 —
# bootcode.bin / start.elf 가 실행될 GPU 자체가 없기 때문이다.
# 대신 -kernel 로 커널 이미지를 직접 올린다. ARM 쪽 코드는 동일하게 돈다.
#
# 머신: raspi3ap = Pi 3A+ (BCM2837, 512MB). Zero 2 W 와 SoC 계열과
# 메모리 크기가 같아 가장 가깝다.
#
# 사용법:
#   ./tools/run-qemu.sh            대화형 (시리얼이 터미널에 붙는다)
#   ./tools/run-qemu.sh log        출력을 파일로 받아 표시 (파이프/CI 안전)
#   ./tools/run-qemu.sh shot       부팅 화면을 PNG 로 캡처

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/common.sh
source "${REPO_ROOT}/tools/common.sh"
KERNEL="${REPO_ROOT}/firmware/${KERNEL_IMAGE}"
OUT_DIR="${REPO_ROOT}/qemu-out"
MACHINE="raspi3ap"
QEMU="${QEMU:-qemu-system-aarch64}"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

command -v "$QEMU" >/dev/null 2>&1 \
    || die "$QEMU 가 없습니다 (apt install qemu-system-arm)"
[[ -f "$KERNEL" ]] || die "firmware/${KERNEL_IMAGE} 가 없습니다. 'make firmware' 를 먼저."

MODE="${1:-interactive}"
mkdir -p "$OUT_DIR"

case "$MODE" in
interactive)
    echo "QEMU ${MACHINE} 에서 실행 중. 종료: Ctrl-A 다음 X"
    echo ""
    # mon:stdio 라야 Ctrl-A X 로 빠져나올 수 있다.
    exec "$QEMU" -M "$MACHINE" -kernel "$KERNEL" \
        -serial mon:stdio -display none
    ;;

log)
    LOG="${OUT_DIR}/serial.log"
    rm -f "$LOG"
    echo "12초간 실행하고 시리얼 출력을 수집합니다..."
    # QEMU 의 stdio 채드브는 stdout 이 TTY 가 아니면 출력을 흘리지 않는다.
    # 파일로 받으면 파이프/CI 환경에서도 확실하다.
    timeout 12 "$QEMU" -M "$MACHINE" -kernel "$KERNEL" \
        -serial "file:${LOG}" -display none >/dev/null 2>&1 || true
    echo ""
    cat "$LOG"
    echo ""
    echo "(저장 위치: ${LOG})"
    ;;

shot)
    command -v python3 >/dev/null 2>&1 || die "python3 가 필요합니다"
    python3 - "$QEMU" "$MACHINE" "$KERNEL" "$OUT_DIR" <<'PY'
import subprocess, sys, time, os
qemu, machine, kernel, outdir = sys.argv[1:5]
proc = subprocess.Popen(
    [qemu, "-M", machine, "-kernel", kernel,
     "-serial", f"file:{outdir}/serial.log",
     "-display", "none", "-monitor", "stdio"],
    stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def shot(name, when):
    time.sleep(when)
    proc.stdin.write(f"screendump {outdir}/{name}.ppm\n".encode())
    proc.stdin.flush()

shot("splash", 2.0)     # 스플래시가 떠 있는 동안
shot("console", 6.0)    # 부팅 로그로 넘어간 뒤
time.sleep(1.5)
proc.stdin.write(b"quit\n"); proc.stdin.flush()
try:
    proc.wait(timeout=10)
except Exception:
    proc.kill()

for n in ("splash", "console"):
    p = f"{outdir}/{n}.ppm"
    print(f"  {p}" if os.path.exists(p) else f"  {n}: 캡처 실패")
PY
    echo ""
    echo "PPM 은 대부분의 뷰어에서 열린다. PNG 가 필요하면:"
    echo "  python3 -c \"from PIL import Image; Image.open('${OUT_DIR}/splash.ppm').save('${OUT_DIR}/splash.png')\""
    ;;

*)
    die "알 수 없는 모드 '$MODE' (interactive | log | shot)"
    ;;
esac
