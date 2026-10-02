#!/usr/bin/env bash
#
# fetch-pc-fw.sh - the firmware the Dell XPS 15 9550 needs, and nothing
# else.
#
# The amd64 kernel drives this laptop with built-in drivers, and four of
# them upload a blob into their chip before it will do anything:
#
#   i915      i915/skl_dmc_ver1_27.bin     display power states (DC5/DC6);
#                                          without it i915 disables
#                                          runtime PM for good
#             i915/skl_{guc,huc}_*.bin     only with i915.enable_guc=2
#                                          (below)
#   ath10k    ath10k/QCA6174/hw3.0/...     Killer 1535 WiFi
#   brcmfmac  brcm/brcmfmac43602-pcie.bin  Dell DW1830 WiFi (BCM43602, the
#                                          other card this model was sold
#                                          with - checked on a real 9550
#                                          under Ubuntu, which loads this)
#             brcm/brcmfmac4350-pcie.bin   DW1820A (BCM4350), sold in
#                                          some later units
#   btusb     qca/rampatch + nvm           Killer 1535 Bluetooth
#             brcm/BCM20703A1-0a5c-6410.hcd  DW1830 Bluetooth (BCM20703A1,
#                                          USB 0a5c:6410); without the
#                                          patch it runs its ROM firmware
#                                          and pairs unreliably
#   microcode intel-ucode/06-5e-03         i7-6700HQ / i5-6300HQ (Skylake
#                                          H, stepping 3): the BIOS's 0xd6
#                                          becomes 0xf0 at boot, with the
#                                          fixes since. Built into the
#                                          kernel (kernel/build.sh), which
#                                          is how Linux loads it early
#                                          without an initrd of its own.
#
# plus regulatory.db and its signature, which built-in cfg80211 asks for
# once at boot and, if it is missing, never again: the radio then stays
# in the world domain until somebody runs `iw reg reload`.
#
# Debian packages these as firmware-misc-nonfree, firmware-atheros and
# firmware-brcm80211: together over 300MB, for about 2MB of files this
# machine can use. So this fetches exactly those files, from one pinned
# linux-firmware commit and one pinned wireless-regdb commit, and checks
# each against a sha256 written here. A mirror serving something else,
# or a later commit changing a blob, is a failed build and not a
# different kernel.
#
# All or nothing. Files are fetched into a staging directory and only
# replace blobs/pc-fw once every one of them has verified; a half set
# would build a kernel whose WiFi works on one card and not the other,
# and nothing at boot would say why. Run again with everything already
# present and verified, it does no network access at all, which is what
# lets kernel/build.sh call it on every build.
#
# The output mirrors /lib/firmware:
#
#   blobs/pc-fw/i915/skl_dmc_ver1_27.bin
#   blobs/pc-fw/ath10k/QCA6174/hw3.0/{board-2.bin,firmware-6.bin,...}
#   ...
#   blobs/pc-fw/LICENSES/   the redistribution terms the blobs ship under
#
# and is used twice:
#
#   kernel/build.sh    puts the firmware files (not the licences) into
#                      the disk kernel's initramfs, because the drivers
#                      probe before the root filesystem is mounted
#   the image build    copies the whole tree into /lib/firmware on the
#                      root, where the same drivers look after a resume
#                      or a reset
#
#   tools/fetch-pc-fw.sh           fetch (or confirm) the set
#   tools/fetch-pc-fw.sh --check   verify what is there; no network
#   tools/fetch-pc-fw.sh --list    print the firmware paths, one a line
#
# GuC and HuC are the exception to "only what it needs". i915 on Skylake
# requests neither unless i915.enable_guc is set (the default is off
# before Gen12), and nothing here sets it: it is a parameter i915 marks
# unsafe, so setting it taints the kernel, and what it buys on this GPU
# is HuC, which only VA-API video ENCODING uses. They are in the set so
# that the person who does turn it on gets a GPU that finds its firmware
# at probe time - i915 is built in and probes from the initramfs - and
# not a boot that asks for a file that is not there, stalls on the
# timeout, and carries on without it. 340KB.
#
# Where the files come from:
#   lf  linux-firmware, one pinned commit
#   rd  wireless-regdb, one pinned commit
#   bt  winterheart/broadcom-bt-firmware, one pinned commit: linux-firmware
#       has no patch for Broadcom's USB Bluetooth chips (Broadcom ships
#       them through Windows Update only); this repository collects them
#       from there, and it is where Ubuntu's brcm/BCM-0a5c-6410.hcd is
#       from too. The kernel asks for BCM20703A1-0a5c-6410.hcd first.
#   mc  Intel's microcode repository, one pinned release tag's commit
#
# Not here, on purpose:
#   brcmfmac4350c2-pcie.bin  early BCM4350 revisions (0-7); the DW1820A
#                 is revision 8 and takes brcmfmac4350-pcie.bin.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${PC_FW_DIR:-${REPO_ROOT}/blobs/pc-fw}"

# linux-firmware tag 20260910. A commit, not the tag name: a tag can be
# moved, a commit cannot.
LF_COMMIT="eeccccbe83daf22e1931e3557ba05b2c02427e4e"
LF_URLS=(
    "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/%s?id=${LF_COMMIT}"
    "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/${LF_COMMIT}/%s"
)
# wireless-regdb tag master-2026-09-03. The database is maintained in
# wens's tree now; the kernel carries his signing certificate
# (net/wireless/certs/wens.hex) beside the older one.
RD_COMMIT="389b9b702cf9018e9a9078ccc4fa8eaae2e054e3"
RD_URLS=(
    "https://git.kernel.org/pub/scm/linux/kernel/git/wens/wireless-regdb.git/plain/%s?id=${RD_COMMIT}"
)
BT_COMMIT="f326999656515a0749258dfefb7cc6731e9933d5"
BT_URLS=(
    "https://raw.githubusercontent.com/winterheart/broadcom-bt-firmware/${BT_COMMIT}/%s"
)
# microcode-20260925
MC_COMMIT="bdc92abe5c499c3fd7988b76a128d05c9120a520"
MC_URLS=(
    "https://raw.githubusercontent.com/intel/Intel-Linux-Processor-Microcode-Data-Files/${MC_COMMIT}/%s"
)

# kind  sha256  path-in-out  path-upstream  source
#   fw   goes into the kernel's initramfs and /lib/firmware
#   doc  licence text, /lib/firmware only
MANIFEST="
fw  d3b6dc1a39bb2aeb37a1179f2b4e8145c24986da78c533571a5712dc91ec3f61 i915/skl_dmc_ver1_27.bin                        i915/skl_dmc_ver1_27.bin                        lf
fw  ccb6e2abf19a88c1ab0fc09f5340a6ff85d52548340031c4e04088d9fc9dc2ce i915/skl_guc_70.1.1.bin                         i915/skl_guc_70.1.1.bin                         lf
fw  c7a1dce013050f823471de2cdc5f0170b1acf8c811ca8c8da41e35f526bcb1d7 i915/skl_huc_2.0.0.bin                          i915/skl_huc_2.0.0.bin                          lf
fw  66e83dde1c9af535df1fcd17c72971a96a263357300e921b358d35a353227d60 ath10k/QCA6174/hw3.0/board-2.bin                ath10k/QCA6174/hw3.0/board-2.bin                lf
fw  04d3bad5efa3f9fbe3ba53fd3e25fa9b0585ed227eea8111303b4e08861f979d ath10k/QCA6174/hw3.0/firmware-6.bin             ath10k/QCA6174/hw3.0/firmware-6.bin             lf
fw  bf4cfc23ee952a3d82ef33a0f5f87853201c98f1bed034876a910f354f37862d brcm/brcmfmac43602-pcie.bin                     brcm/brcmfmac43602-pcie.bin                     lf
fw  5691d1e0ceb70baf18efb7a0ec6cb84feb9edd2d0700c525b42930c4e7e4b845 brcm/brcmfmac4350-pcie.bin                      brcm/brcmfmac4350-pcie.bin                      lf
fw  e526fd12cd3529b7e01c0076f69189b7e2d9a0124a91e7583c6ddefecdbe0599 brcm/BCM20703A1-0a5c-6410.hcd                   brcm/BCM20703A1-0a5c-6410.hcd                   bt
fw  15e96637a89012390e2c254effad092fc9535912b709ffa316069fc5606efb65 intel-ucode/06-5e-03                            intel-ucode/06-5e-03                            mc
fw  f0d15f6d7c4ce17270c951287222699c0909bea9028ecb84d2e8be6fa364691e qca/rampatch_usb_00000302.bin                   qca/rampatch_usb_00000302.bin                   lf
fw  9ee2cff5bd51523b65c941b72ecf05a8b5e7b9e280c79fa08c70ade7205088a5 qca/nvm_usb_00000302.bin                        qca/nvm_usb_00000302.bin                        lf
fw  7e236caecd939c8ec98be4870bf30422f28ffef2565a38aaaa2d9ddabd0c2641 regulatory.db                                   regulatory.db                                   rd
fw  50332f0db09b8bcc719235ec4985c27377f2cc2f42abb7b5dcf951866c5a888a regulatory.db.p7s                               regulatory.db.p7s                               rd
doc 8ce5c6ea0542bf4aac31fc3ae16a39792ad22d0eae4543063fac56fb3380f021 ath10k/QCA6174/hw3.0/notice_ath10k_firmware-6.txt ath10k/QCA6174/hw3.0/notice_ath10k_firmware-6.txt lf
doc 8542aeabf2761935122d693561e16766ce1bcc2b0d003204f9040b7d6d929f2e LICENSES/LICENSE.i915                           LICENSES/LICENSE.i915                           lf
doc 337a55102138d7baa143ee4a4c6c91693e0113fece35d380b2a12109e8c23b3f LICENSES/LICENSE.QualcommAtheros_ath10k         LICENSES/LICENSE.QualcommAtheros_ath10k         lf
doc 600276e0992c8e5a85300d605fb2db6132ffcf2a21ddcfd0e8566a19e0f491c3 LICENSES/NOTICE.qca                             LICENSES/NOTICE.qca                             lf
doc b16056fc91b82a0e3e8de8f86c2dac98201aa9dc3cbd33e8d38f1b087fcec30d LICENSES/LICENCE.broadcom_bcm43xx               LICENSES/LICENCE.broadcom_bcm43xx               lf
doc 678b0df753c86198fc496d1f1033429bbd57f101472132ee7eaaf9f5e0a7fae1 LICENSES/LICENSE.wireless-regdb                 LICENSE                                         rd
doc a12e372fee6d54196c189d85b8a7b52221042c8585c048def2b32ee7294f95b9 LICENSES/LICENSE.broadcom_bcm20702             LICENSE.broadcom_bcm20702                       bt
doc 03efb1491c7e899feb2665fa299363e64035e5444c1b8bc1f6ebed30de964e12 LICENSES/LICENSE.intel-ucode                    license                                         mc
"

die() { printf 'fetch-pc-fw: %s\n' "$*" >&2; exit 1; }

entries() { printf '%s\n' "$MANIFEST" | awk 'NF == 5'; }

sha_ok() {  # <file> <sha256>
    [[ -f "$1" ]] && [[ "$(sha256sum "$1" | cut -d' ' -f1)" == "$2" ]]
}

# Is every file in <dir> present with the right contents?
complete() {
    local dir="$1" kind sum path up src
    while read -r kind sum path up src; do
        sha_ok "${dir}/${path}" "$sum" || return 1
    done < <(entries)
}

case "${1:-}" in
--list)
    entries | awk '$1 == "fw" {print $3}'
    exit 0
    ;;
--check)
    if complete "$OUT"; then
        echo "fetch-pc-fw: ${OUT} complete ($(entries | wc -l) files)"
        exit 0
    fi
    echo "fetch-pc-fw: ${OUT} is missing files or has the wrong ones" >&2
    exit 1
    ;;
"") ;;
*)  die "usage: $0 [--list|--check]" ;;
esac

if complete "$OUT"; then
    echo "fetch-pc-fw: ${OUT} already complete and verified"
    exit 0
fi

command -v curl >/dev/null || die "curl is needed"

STAGE="$(mktemp -d "${OUT%/*}/.pc-fw.stage.XXXXXX" 2>/dev/null)" || {
    mkdir -p "${OUT%/*}"
    STAGE="$(mktemp -d "${OUT%/*}/.pc-fw.stage.XXXXXX")"
}
trap 'rm -rf "$STAGE"' EXIT

echo "fetch-pc-fw: linux-firmware ${LF_COMMIT:0:12}, wireless-regdb ${RD_COMMIT:0:12}, broadcom-bt ${BT_COMMIT:0:12}, microcode ${MC_COMMIT:0:12}"
FAILED=0
while read -r kind sum path up src; do
    dst="${STAGE}/${path}"
    mkdir -p "$(dirname "$dst")"
    # Reuse a file that is already right, so a rerun after one failed
    # download fetches only what is missing.
    if sha_ok "${OUT}/${path}" "$sum"; then
        cp "${OUT}/${path}" "$dst"
        printf '  have  %s\n' "$path"
        continue
    fi
    case $src in
        lf) urls=("${LF_URLS[@]}") ;;
        rd) urls=("${RD_URLS[@]}") ;;
        bt) urls=("${BT_URLS[@]}") ;;
        mc) urls=("${MC_URLS[@]}") ;;
        *)  die "unknown source ${src} for ${path}" ;;
    esac
    got=false
    for fmt in "${urls[@]}"; do
        # shellcheck disable=SC2059
        url="$(printf "$fmt" "$up")"
        if curl --fail --location --silent --show-error \
                --retry 3 --retry-delay 2 --retry-all-errors \
                --output "$dst" "$url" 2>/dev/null && sha_ok "$dst" "$sum"; then
            got=true
            break
        fi
        rm -f "$dst"
    done
    if $got; then
        printf '  OK    %-50s %8s bytes\n' "$path" "$(stat -c%s "$dst")"
    else
        printf '  FAIL  %s (not fetched, or sha256 did not match)\n' "$path" >&2
        FAILED=$((FAILED + 1))
    fi
done < <(entries)

(( FAILED == 0 )) || die "${FAILED} file(s) failed; ${OUT} left as it was"
complete "$STAGE" || die "staging set does not verify; ${OUT} left as it was"

rm -rf "$OUT"
mv "$STAGE" "$OUT"
trap - EXIT
chmod -R u=rwX,go=rX "$OUT"
echo "fetch-pc-fw: $(entries | wc -l) files, $(du -sh "$OUT" | cut -f1) -> ${OUT}"
