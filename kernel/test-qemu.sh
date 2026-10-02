#!/usr/bin/env bash
#
# test-qemu.sh - 빌드한 커널을 QEMU 에서 부팅시켜 본다.
#
# 우리 유저랜드가 커널에 내장되어 있으므로 Image 파일 하나만 주면
# 부팅해서 셸까지 올라온다. SD 이미지도 initramfs 파일도 필요 없다.
#
# 머신은 virt 를 쓴다. raspi3ap 이 하드웨어에는 더 가깝지만, QEMU 의 raspi
# 머신은 디바이스 트리를 만들어주지 않는다. 실기에서는 start.elf 가 DTB 를
# 읽어 메모리 크기·시리얼·활성 노드를 패치해서 커널에 넘기는데, QEMU 에는
# 그 과정이 없다. 라즈베리파이 공식 DTB 를 -dtb 로 줘도 패치되지 않은
# 상태라 콘솔조차 올라오지 않는다.
#
# virt 는 QEMU 가 DTB 를 직접 생성해준다. CPU 를 cortex-a53, 메모리를
# 512MB 로 맞추면 Zero 2 W 와 같은 조건이 된다. BCM 고유 주변장치는
# 검증되지 않지만, 커널이 부팅하고 initramfs 를 풀고 우리 init 과 셸이
# 도는지는 여기서 확인할 수 있다. BCM 부분은 실기에서 확인한다.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${REPO_ROOT}/kernel/out/Image"
LOG_DIR="${REPO_ROOT}/kernel/out"
LOG="${LOG_DIR}/boot.log"
MACHINE=virt
CPU=cortex-a53
MEM=512
TIMEOUT="${TIMEOUT:-90}"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ -f "$IMAGE" ]] || die "kernel/out/Image 가 없습니다. ./kernel/build.sh 를 먼저."
command -v qemu-system-aarch64 >/dev/null || die "qemu-system-aarch64 가 없습니다"

MODE="${1:-log}"

case "$MODE" in
interactive)
    echo "QEMU ${MACHINE}. 종료: Ctrl-A 다음 X"
    exec qemu-system-aarch64 -M "$MACHINE" -cpu "$CPU" -m "$MEM" \
        -kernel "$IMAGE" \
        -append "console=ttyAMA0 rootwait" \
        -serial mon:stdio -display none
    ;;
log)
    rm -f "$LOG"
    echo "${TIMEOUT}초간 부팅하고 로그를 수집합니다..."
    # QEMU 의 stdio 채드브는 stdout 이 TTY 가 아니면 출력을 버린다.
    # 파일로 받아야 파이프/CI 에서도 확실하다.
    timeout "$TIMEOUT" qemu-system-aarch64 -M "$MACHINE" -cpu "$CPU" -m "$MEM" \
        -kernel "$IMAGE" \
        -append "earlycon console=ttyAMA0 rootwait" \
        -serial "file:${LOG}" -display none >/dev/null 2>&1 || true

    echo ""
    tail -20 "$LOG"
    echo ""
    echo "── 판정 ──"
    if grep -q "LP-zero OS" "$LOG" 2>/dev/null; then
        echo "  OK: init 이 실행되었습니다"
    else
        echo "  실패: init 배너가 없습니다"
    fi
    if grep -q 'init (pid 1)' "$LOG" 2>/dev/null; then
        echo "  OK: init 이 PID 1 로 실행되었습니다"
    else
        echo "  실패: init 이 PID 1 이 아닙니다"
    fi
    if grep -qE '/ \$' "$LOG" 2>/dev/null; then
        echo "  OK: 셸 프롬프트가 나왔습니다"
    else
        echo "  실패: 셸 프롬프트가 없습니다"
    fi
    grep -qi "kernel panic" "$LOG" 2>/dev/null && echo "  !! 커널 패닉 발생"
    echo ""
    printf "  부팅 시간: "
    grep -oE '^\[ *[0-9]+\.[0-9]+\] Run /init' "$LOG" 2>/dev/null \
        | grep -oE '[0-9]+\.[0-9]+' || echo "?"
    echo "  전체 로그: ${LOG}"
    ;;
disk)
    # SD 이미지를 디스크로 붙여서 부팅한다. 이래야 /data 파티션 마운트,
    # expandfs, /data/bin 의 프로그램까지 확인할 수 있다.
    #
    # 주의: 우리 커널에는 VIRTIO_MMIO 만 있고 VIRTIO_PCI 는 없다.
    # QEMU 의 '-drive if=virtio' 는 virt 머신에서 virtio-blk-PCI 를 만들기
    # 때문에 커널이 디스크를 못 본다. -device virtio-blk-device 로
    # MMIO 트랜스포트를 명시해야 한다.
    SRC="${2:-${REPO_ROOT}/sdcard/lp-zero.img}"
    [[ -f "$SRC" ]] || die "SD 이미지가 없습니다: $SRC  ('make sdcard-linux')"

    # 원본을 건드리지 않도록 복사한다. 겸사겸사 크게 만들어
    # 첫 부팅 확장(expandfs)이 실제로 도는지도 본다.
    WORK="${LOG_DIR}/.qemu-disk.img"
    DISK_MB="${DISK_MB:-1024}"
    cp "$SRC" "$WORK"
    truncate -s "${DISK_MB}M" "$WORK"

    rm -f "$LOG"
    echo "${TIMEOUT}초간 부팅합니다 (디스크 ${DISK_MB}MB)..."
    timeout "$TIMEOUT" qemu-system-aarch64 -M "$MACHINE" -cpu "$CPU" -m "$MEM" \
        -kernel "$IMAGE" \
        -append "earlycon console=ttyAMA0" \
        -drive "file=${WORK},format=raw,if=none,id=hd0" \
        -device virtio-blk-device,drive=hd0 \
        -netdev user,id=n0 -device virtio-net-device,netdev=n0 \
        -serial "file:${LOG}" -display none >/dev/null 2>&1 || true

    echo ""
    echo "── 판정 ──"
    check() {   # check <설명> <정규식>
        if grep -qE "$2" "$LOG" 2>/dev/null; then echo "  OK: $1"
        else echo "  실패: $1"; fi
    }
    check "init 이 PID 1 로 실행"        'init \(pid 1\)'
    check "zram 압축 스왑 생성"          'zram: [0-9]+MB'
    check "데이터 파티션을 카드 끝까지 확장" 'expandfs: 완료'
    check "/data 마운트"                 'mount: /dev/[a-z0-9]+2 -> /data'
    check "DHCP 주소 획득"               'dhcp: [a-z0-9]+ +주소 [0-9]'
    check "셸 프롬프트"                  '/ \$'
    grep -qi "kernel panic" "$LOG" 2>/dev/null && echo "  !! 커널 패닉"
    echo ""
    echo "  전체 로그: ${LOG}"
    ;;
*)
    die "알 수 없는 모드 '$MODE' (interactive | log | disk)"
    ;;
esac
