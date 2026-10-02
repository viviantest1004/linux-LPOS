#!/usr/bin/env bash
#
# mkarmv6config.sh - build kernel/lp-zero-armv6.config from the arm64
# config plus kernel/lp-zero-armv6.fragment.
#
# Same shape, and the same reason, as tools/mkamd64config.sh: the file
# is generated because merge_config takes the LAST value it sees, and
# ordering that by hand is how a Pi ends up with no console.
#
#   arm64 answers first  - a Pi Zero W is a Broadcom board with a PL011,
#                          an SDHCI controller and a BCM43438 on SDIO,
#                          exactly like a Zero 2 W. Almost everything
#                          carries straight over.
#   armv6 answers last   - and therefore win.
#
# What does NOT carry over is everything that assumes a 64-bit machine
# or a virtual one: UEFI, PCI, ACPI, virtio, NVMe, and the crypto
# extensions that live in the ARMv8 instruction set. A Pi Zero W has
# none of them, and asking for them produces a config that either fails
# to build or quietly enables nothing.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/.." && pwd)"

ARM="${REPO_ROOT}/kernel/lp-zero.config"
FRAG="${REPO_ROOT}/kernel/lp-zero-armv6.fragment"
OUT="${REPO_ROOT}/kernel/lp-zero-armv6.config"

[[ -f "$ARM"  ]] || { echo "missing $ARM"  >&2; exit 1; }
[[ -f "$FRAG" ]] || { echo "missing $FRAG" >&2; exit 1; }

# Symbols that assume a 64-bit or a virtual machine.
#
# EFI_ZBOOT in particular: the arm64 image is booted by UEFI in a VM and
# by the GPU firmware on a board, so it is built twice, compressed and
# not. A Pi Zero W has only the second path - the GPU firmware loads
# zImage, which carries its own decompressor - so the EFI half is not
# smaller here, it is absent.
DROP='ARM64|EFI|ACPI|^CONFIG_PCI|PCIE|PCIEPORT|VIRTIO|XEN|NVME|_CE=|KERNEL_MODE_NEON|COMPAT_32BIT|CONFIG_COMPAT|HYPERV|CONFIG_64BIT|VC4|V3D|CONFIG_DRM_|BCM2711|BCM2712|ARM_SMMU|ARM_SCMI|GENERIC_CPU_VULNERABILITIES|CONFIG_CPU_ISOLATION'

{
    cat <<'HEADER'
#
# lp-zero-armv6.config
#
# GENERATED - do not edit. Change kernel/lp-zero-armv6.fragment and run
# tools/mkarmv6config.sh (or just build: kernel/build.sh regenerates it).
#
# The arm64 answers come first and the armv6 answers last, because
# merge_config takes the last value it sees.
#
HEADER
    grep -Ev "$DROP" "$ARM"
    echo
    echo "# ══ armv6 answers, last so they win ═══════════════════════════"
    cat "$FRAG"
} > "$OUT"

printf 'wrote %s (%d settings)\n' "$OUT" "$(grep -c '^CONFIG_' "$OUT")"
