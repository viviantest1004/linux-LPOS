# common.sh - tools/*.sh 가 공유하는 설정. source 해서 쓴다.
# config.mk 를 파싱해 Makefile 과 같은 이미지 이름을 쓰게 한다.

_read_kernel_image() {
    local mk="${REPO_ROOT}/config.mk"
    if [[ -f "$mk" ]]; then
        sed -n 's/^[[:space:]]*KERNEL_IMAGE[[:space:]]*:*=[[:space:]]*\(.*\)$/\1/p' "$mk" \
            | tail -1 | tr -d '[:space:]'
    fi
}

_read_mk_var() {
    local mk="${REPO_ROOT}/config.mk"
    [[ -f "$mk" ]] || return 0
    sed -n "s/^[[:space:]]*$1[[:space:]]*:*=[[:space:]]*\(.*\)$/\1/p" "$mk" \
        | tail -1 | tr -d '[:space:]'
}

KERNEL_IMAGE="$(_read_kernel_image)"
: "${KERNEL_IMAGE:=kernel8.img}"

LINUX_IMAGE="$(_read_mk_var LINUX_IMAGE)"
: "${LINUX_IMAGE:=Image}"

# The Pi Zero W kernel has its own name. Two boards, two instruction
# sets, two files that cannot be swapped for each other - and nothing
# about a file called "kernel.img" says which one you have.
LINUX_IMAGE_ARMV6="$(_read_mk_var LINUX_IMAGE_ARMV6)"
: "${LINUX_IMAGE_ARMV6:=zImage}"

# ── 빌드 산출물이 놓이는 곳 ──────────────────────────────────────
#
# 커널 소스, 서드파티 소스, 크로스 sysroot, 파이썬 스테이징 - 저장소에
# 커밋하지 않는 것들이 전부 여기 들어간다.
#
# 예전에는 스크립트마다 /home/user/kernel-work 를 기본값으로 박아두었다.
# 그 경로가 있는 기계는 이것을 만든 기계 하나뿐이므로, 저장소를 받은
# 사람은 빌드를 시작할 수조차 없었다. 이제 저장소 옆의 .build 다.
#
#   LPZERO_WORK=/mnt/big/lpzero ./kernel/build.sh
#
# 처럼 환경변수로 옮길 수 있다. 디스크가 부족하거나 여러 저장소가 소스를
# 공유하게 하고 싶을 때 쓴다.
LPZERO_WORK="${LPZERO_WORK:-${REPO_ROOT}/.build}"

WORK="${WORK:-${LPZERO_WORK}/thirdparty}"
SYSROOT="${SYSROOT:-${WORK}/sysroot}"
LINUX_SRC="${LINUX_SRC:-${LPZERO_WORK}/linux}"
BUILD_DIR="${BUILD_DIR:-${LPZERO_WORK}/build}"
PYSTAGE_ROOT="${PYSTAGE_ROOT:-${LPZERO_WORK}/python-stage}"

# ── Is everything in this tree built for the machine we are building for?
#
# This exists because an amd64 image shipped with two aarch64 binaries in
# its initramfs and a 43MB aarch64 /data partition - glibc, CPython and
# micropython, all of them for the wrong instruction set. The image
# booted perfectly, because the parts that boot were right; WiFi died
# with "Exec format error" and Python did not run at all. Nothing in the
# build said a word, because nothing was looking.
#
# The mistake was checking the code I had ported and not the binaries I
# was copying in beside it. So this checks the tree, not the intention:
# every ELF file, against the ELF header's own idea of what machine it
# is for.
#
#   e_machine sits at byte 18 of any ELF, two bytes little-endian:
#     0x3E  x86-64        0xB7  AArch64
#
# It dies rather than warns. A warning in a hundred lines of build output
# is how this got shipped in the first place.
elf_machine_of() {
    python3 - "$1" <<'PY'
import sys, struct
try:
    with open(sys.argv[1], 'rb') as f:
        head = f.read(20)
    if len(head) < 20 or head[:4] != b'\x7fELF':
        print("")            # not an ELF: nothing to check
    else:
        print(hex(struct.unpack_from('<H', head, 18)[0]))
except Exception:
    print("")
PY
}

# check_tree_arch <directory> <arm64|amd64> <label for the message>
check_tree_arch() {
    local dir="$1" arch="$2" label="$3"
    local want name
    case "$arch" in
        amd64) want=0x3e; name="x86-64"  ;;
        arm64) want=0xb7; name="aarch64" ;;
        armv6) want=0x28; name="32-bit ARM" ;;
        *) die "check_tree_arch: unknown architecture '${arch}'" ;;
    esac

    [[ -d "$dir" ]] || return 0

    local bad=0 f m first=""
    while IFS= read -r -d '' f; do
        m="$(elf_machine_of "$f")"
        [[ -z "$m" ]] && continue
        if [[ "$m" != "$want" ]]; then
            bad=$((bad + 1))
            [[ -z "$first" ]] && first="$f"
            (( bad <= 8 )) && printf '  %s  (e_machine %s)\n' \
                "${f#"$dir"/}" "$m" >&2
        fi
    done < <(find "$dir" -type f -print0)

    if (( bad > 0 )); then
        (( bad > 8 )) && printf '  ... and %d more\n' $((bad - 8)) >&2
        die "${label}: ${bad} file(s) are not ${name}.
       This image would boot and then fail at exec, which is the hardest
       kind of broken to notice. Build those parts for ${arch}, or leave
       them out."
    fi
}
