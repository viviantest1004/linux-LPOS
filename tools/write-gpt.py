#!/usr/bin/env python3
"""write-gpt.py - put a GUID partition table on a disk image.

mkdisk.sh builds the amd64 image as a plain file and has to partition it
without a loop device, without root and without sfdisk, which the build
host does not have. The Raspberry Pi images use write-mbr.py for the same
reason; this is its GPT counterpart, because a PC's disk is GPT:

  - the UEFI firmware on the owner's laptop boots GPT disks, and the
    installed system's boot entry names its EFI partition by the GPT
    partition GUID;
  - the root partition carries the type "Linux root (x86-64)" and a
    PARTUUID of its own, which is how preinit tells this disk's root from
    the root on another LP disk plugged in beside it.

Every GUID is random, made fresh on every run - the disk's, and every
partition's. Two images built from the same tree still have different
PARTUUIDs, and so does every installed copy (the installer makes its own).

    write-gpt.py IMAGE START:SECTORS:TYPE:NAME [START:SECTORS:TYPE:NAME ...]

START and SECTORS are in 512-byte sectors; TYPE is "esp", "root" or a
GUID. The file is not resized: the backup table goes in its last 33
sectors, and the partitions must leave room for it. Prints one line per
partition, "N PARTUUID", for the caller to log.
"""
import os
import sys
import uuid
import zlib
import struct


# What a PC started in Legacy (BIOS/CSM) mode runs from this disk: the
# MBR's boot code. LP boots with UEFI only, and a BIOS that finds nothing
# here showed a blank screen or "no bootable device", which says nothing
# about what to change. So the code prints that, and stops. UEFI never
# runs it (it reads the GPT; the protective entry stays non-bootable, as
# the UEFI spec wants). 16-bit real mode, assembled by hand:
#   cli; xor ax,ax; mov ds,ax; mov es,ax; mov ss,ax; mov sp,0x7c00; sti
#   mov si,msg
#   next: lodsb; test al,al; jz stop
#         mov ah,0x0e; mov bx,7; int 0x10; jmp next    (BIOS teletype)
#   stop: hlt; jmp stop
LEGACY_MSG = (b"\r\nLP starts in UEFI mode only - this PC started it in Legacy mode.\r\n"
              b"\r\nIn the BIOS setup (F2 on a Dell):\r\n"
              b"  Boot Sequence > Boot List Option: UEFI\r\n"
              b"  Secure Boot: Disabled\r\n"
              b"\r\nThen press F12 at power-on and pick the UEFI entry of this disk.\r\n")
LEGACY_STUB = (bytes.fromhex("fa31c08ed88ec08ed0bc007cfb" "be217c"
                             "ac84c07409b40ebb0700cd10ebf2" "f4ebfd")
               + LEGACY_MSG + b"\x00")
assert len(LEGACY_STUB) <= 440

SECTOR = 512
ENTRIES = 128
ENTRY_SIZE = 128
TABLE_SECTORS = ENTRIES * ENTRY_SIZE // SECTOR       # 32

TYPES = {
    "esp":  uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b"),
    "root": uuid.UUID("4f68bce3-e8cd-4db1-96e7-fbcaf984b709"),
    "linux": uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4"),
}


def die(msg):
    sys.stderr.write("write-gpt.py: %s\n" % msg)
    sys.exit(1)


def header(disk_guid, this_lba, alt_lba, first, last, table_lba, table_crc):
    h = struct.pack(
        "<8sIIIIQQQQ16sQIII",
        b"EFI PART", 0x00010000, 92, 0, 0,
        this_lba, alt_lba, first, last,
        disk_guid.bytes_le, table_lba, ENTRIES, ENTRY_SIZE, table_crc)
    crc = zlib.crc32(h) & 0xffffffff
    h = h[:16] + struct.pack("<I", crc) + h[20:]
    return h.ljust(SECTOR, b"\0")


def main(argv):
    if len(argv) < 3:
        die("usage: write-gpt.py IMAGE START:SECTORS:TYPE:NAME ...")
    image = argv[1]
    total = os.path.getsize(image) // SECTOR
    first_usable = 2 + TABLE_SECTORS
    last_usable = total - 1 - TABLE_SECTORS - 1
    if last_usable <= first_usable:
        die("%s is too small for a partition table" % image)

    table = bytearray(ENTRIES * ENTRY_SIZE)
    out = []
    for i, spec in enumerate(argv[2:]):
        try:
            start, count, ptype, name = spec.split(":", 3)
            start, count = int(start), int(count)
        except ValueError:
            die("bad partition %r (want START:SECTORS:TYPE:NAME)" % spec)
        ptype = TYPES.get(ptype) or uuid.UUID(ptype)
        end = start + count - 1
        # A partition outside the usable range would overlap one of the
        # two tables, and the first thing to notice would be the firmware
        # refusing the disk - so it is refused here instead.
        if start < first_usable or end > last_usable:
            die("partition %d (%d..%d) is outside %d..%d"
                % (i + 1, start, end, first_usable, last_usable))
        part_guid = uuid.uuid4()
        entry = struct.pack("<16s16sQQQ72s", ptype.bytes_le, part_guid.bytes_le,
                            start, end, 0, name.encode("utf-16-le")[:72])
        table[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE] = entry
        out.append("%d %s" % (i + 1, part_guid))

    table = bytes(table)
    table_crc = zlib.crc32(table) & 0xffffffff
    disk_guid = uuid.uuid4()
    backup_table_lba = total - 1 - TABLE_SECTORS

    # The protective MBR: one partition of type 0xEE covering the disk,
    # so a tool that only knows MBR sees the disk as full rather than
    # empty and does not offer to "initialise" it.
    mbr = bytearray(SECTOR)
    mbr[:len(LEGACY_STUB)] = LEGACY_STUB
    size = min(total - 1, 0xffffffff)
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x02\x00", 0xee,
                               b"\xff\xff\xff", 1, size)
    mbr[510:512] = b"\x55\xaa"

    with open(image, "r+b") as f:
        f.seek(0)
        f.write(bytes(mbr))
        f.write(header(disk_guid, 1, total - 1, first_usable, last_usable,
                       2, table_crc))
        f.write(table)
        f.seek(backup_table_lba * SECTOR)
        f.write(table)
        f.write(header(disk_guid, total - 1, 1, first_usable, last_usable,
                       backup_table_lba, table_crc))
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv)
