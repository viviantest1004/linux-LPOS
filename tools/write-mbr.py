#!/usr/bin/env python3
"""write-mbr.py - 이미지 파일에 MBR 파티션 테이블을 쓴다.

sfdisk/parted 없이 바이트를 직접 찍는다. 컨테이너나 CI 에서도 돌아간다.

사용법: write-mbr.py <이미지> <부트시작LBA> <부트섹터수> <데이터시작LBA> <데이터섹터수>
"""
import struct
import sys


def chs(lba, heads=255, spt=63):
    """LBA -> CHS 3바이트.

    1023 실린더를 넘으면 표현할 수 없으므로 관례대로 최대값으로 고정한다.
    라즈베리파이 부트롬은 LBA 를 보므로 CHS 는 형식상 채우는 값이다.
    """
    cyl, rem = divmod(lba, heads * spt)
    head, sec = divmod(rem, spt)
    sec += 1
    if cyl > 1023:
        cyl, head, sec = 1023, 254, 63
    return bytes([head & 0xFF, ((cyl >> 2) & 0xC0) | (sec & 0x3F), cyl & 0xFF])


def entry(start, count, ptype, bootable=False):
    """16바이트 파티션 항목."""
    return (
        bytes([0x80 if bootable else 0x00])
        + chs(start)
        + bytes([ptype])
        + chs(start + count - 1)
        + struct.pack("<I", start)
        + struct.pack("<I", count)
    )


def main():
    if len(sys.argv) != 6:
        print(__doc__, file=sys.stderr)
        return 2

    image = sys.argv[1]
    b_start, b_count, d_start, d_count = (int(x) for x in sys.argv[2:6])

    if b_start + b_count > d_start:
        print("error: 부트 파티션이 데이터 파티션과 겹칩니다", file=sys.stderr)
        return 1

    # The boot flag on partition 1. UEFI does not read it - it looks for
    # EFI/BOOT/BOOTAA64.EFI on any FAT volume it can see - and neither
    # does the Pi's boot ROM. But firmware that predates UEFI does, and
    # some of it refuses a disk where no partition is marked active. It
    # costs one bit to be unambiguous about which partition boots.
    table = entry(b_start, b_count, 0x0C, bootable=True)   # FAT32 (LBA)
    table += entry(d_start, d_count, 0x83)                 # Linux
    table += b"\x00" * 32                    # 파티션 3, 4 는 비움
    assert len(table) == 64

    with open(image, "r+b") as f:
        # The disk signature, at offset 440. It identifies this disk, and
        # firmware puts it in the device path of everything on it -
        # HD(1,MBR,<signature>,...). Left at zero, every image we ever
        # write has the same identity, and firmware that remembers boot
        # entries by device path can match a stale one against the wrong
        # disk. A fixed non-zero value is enough: it is our image.
        f.seek(440)
        f.write(struct.pack("<I", 0x4C50305A))   # "LP0Z"

        f.seek(446)
        f.write(table)
        f.seek(510)
        f.write(b"\x55\xAA")

    print(f"  MBR: p1 FAT32 {b_count * 512 // 1024 // 1024}MiB, "
          f"p2 ext4 {d_count * 512 // 1024 // 1024}MiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
