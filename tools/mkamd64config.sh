#!/usr/bin/env bash
#
# mkamd64config.sh - build kernel/lp-zero-amd64.config from the arm64
# config plus kernel/lp-zero-amd64.fragment.
#
# It is generated rather than kept by hand because the order matters and
# hand-editing got it wrong three times in a row. merge_config takes the
# LAST value it sees, so:
#
#   arm64 answers first  - everything that is about Linux rather than
#                          about a Raspberry Pi carries straight over
#   amd64 answers last   - and therefore win, with no list of exceptions
#
# Each of those three mistakes was the same shape: a line that is right
# for a Pi read after the line that is right for a PC. "# CONFIG_SERIAL_
# 8250 is not set" (a Pi has a PL011) left the machine with no console
# at all; CONFIG_DRM=n left it with no graphics; CONFIG_SND=n left it
# with no sound. None of them was a build error.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/.." && pwd)"

ARM="${REPO_ROOT}/kernel/lp-zero.config"
FRAG="${REPO_ROOT}/kernel/lp-zero-amd64.fragment"
OUT="${REPO_ROOT}/kernel/lp-zero-amd64.config"

[[ -f "$ARM"  ]] || { echo "missing $ARM"  >&2; exit 1; }
[[ -f "$FRAG" ]] || { echo "missing $FRAG" >&2; exit 1; }

# Symbols that are about this particular board rather than about Linux.
# Anything matching is dropped instead of carried over: a PC has no
# Broadcom GPU, no PL011, no VideoCore.
DROP='BCM2835|BCM2711|BCM283|RASPBERRY|ARM64|ARCH_BCM|SERIAL_AMBA|BRCMFMAC|BRCMUTIL|MMC_SDHCI_IPROC|PL011|DMA_BCM|SND_BCM|VC4|V3D|ARM_SCMI|ARM_SMMU|ARM_GIC|PCIE_BRCM|I2C_BCM|SPI_BCM|GPIO_BCM|HW_RANDOM_BCM|CRYPTO_SHA2_ARM|CRYPTO_AES_ARM|KERNEL_MODE_NEON|EFI_ZBOOT|FB_BCM|ARM'

{
    cat <<'HEADER'
#
# lp-zero-amd64.config
#
# GENERATED - do not edit. Change kernel/lp-zero-amd64.fragment and run
# tools/mkamd64config.sh (or just build: kernel/build.sh regenerates it).
#
# The arm64 answers come first and the amd64 answers last, because
# merge_config takes the last value it sees.
#
HEADER
    grep -Ev "$DROP" "$ARM"
    echo
    echo "# ══ amd64 answers, last so they win ═══════════════════════════"
    cat "$FRAG"
} > "$OUT"

printf 'wrote %s (%d settings)\n' "$OUT" "$(grep -c '^CONFIG_' "$OUT")"
