#!/bin/sh
# run-arm64.sh - start this image in QEMU with 4GB of RAM and a 16GB disk.
#
# The image ships small: it has to fit on any SD card somebody owns, and
# on the first boot expandfs grows /data into whatever it finds. So the
# 16GB here is not a different image - it is the same file, told to
# occupy a bigger disk, and the system fills it out by itself the first
# time it starts.
#
# The file stays sparse: `truncate` writes no data, and the disk only
# takes up what is actually used. Growing to 16GB costs nothing until
# something is stored.
#
#   ./run-arm64.sh                  4GB RAM, 16GB disk, a window
#   RAM=8192 ./run-arm64.sh         more memory
#   DISK=32   ./run-arm64.sh        a 32GB disk instead
#   HEADLESS=1 ./run-arm64.sh       no window; the console is this terminal
set -e

IMG="${IMG:-test_a_123_LPzero2W_linux.img}"
RAM="${RAM:-4096}"          # MB
DISK="${DISK:-16}"          # GB
CPUS="${CPUS:-4}"

[ -f "$IMG" ] || { echo "$IMG is not here. Unzip it first."; exit 1; }

# Grow the disk once. Never shrink: truncating down would cut the
# filesystem in half, so if it is already bigger, leave it.
WANT=$((DISK * 1024 * 1024 * 1024))
HAVE=$(stat -c%s "$IMG" 2>/dev/null || stat -f%z "$IMG")
if [ "$HAVE" -lt "$WANT" ]; then
    echo "growing $IMG to ${DISK}GB (sparse - it uses no space until written)"
    truncate -s "$WANT" "$IMG"
elif [ "$HAVE" -gt "$WANT" ]; then
    echo "note: $IMG is already larger than ${DISK}GB - leaving it alone"
fi

# UEFI firmware. QEMU needs it to find EFI/BOOT/BOOTAA64.EFI on the FAT
# partition, which is how this image boots on anything that is not a Pi.
FW=""
for f in /usr/share/AAVMF/AAVMF_CODE.fd \
         /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
         /opt/homebrew/share/qemu/edk2-aarch64-code.fd \
         /usr/local/share/qemu/edk2-aarch64-code.fd; do
    [ -f "$f" ] && { FW="$f"; break; }
done
[ -n "$FW" ] || {
    echo "No aarch64 UEFI firmware found."
    echo "  Debian/Ubuntu:  sudo apt install qemu-efi-aarch64"
    echo "  macOS:          brew install qemu"
    exit 1
}

# A writable copy of the firmware's variable store, so the VM can
# remember its boot entry between runs.
[ -f efi-vars.fd ] || { truncate -s 64M efi-vars.fd; }

echo "starting: ${RAM}MB RAM, ${CPUS} cpus, ${DISK}GB disk"
if [ -n "$HEADLESS" ]; then
    DISPLAY_ARGS="-nographic"
else
    DISPLAY_ARGS="-device virtio-gpu-pci -display default,show-cursor=on \
                  -device qemu-xhci -device usb-kbd -device usb-tablet \
                  -serial mon:stdio"
fi

# shellcheck disable=SC2086
exec qemu-system-aarch64 \
    -M virt -cpu cortex-a72 -smp "$CPUS" -m "$RAM" \
    -drive if=pflash,format=raw,readonly=on,file="$FW" \
    -drive if=pflash,format=raw,file=efi-vars.fd \
    -drive file="$IMG",format=raw,if=virtio \
    -netdev user,id=n0,hostfwd=tcp::2222-:22 \
    -device virtio-net-pci,netdev=n0 \
    $DISPLAY_ARGS
