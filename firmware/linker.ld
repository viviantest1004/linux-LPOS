/* linker.ld - kernel8.img 메모리 배치.
 *
 * start.elf 는 arm_64bit=1 일 때 kernel8.img 를 0x80000 에 로드한다.
 * 따라서 링크 주소도 0x80000 이어야 한다 (위치 독립이 아니다). */

ENTRY(_start)

SECTIONS
{
    . = 0x80000;
    __image_start = .;

    /* .text.boot 이 반드시 최상단 = 0x80000 에 와야 한다.
     * GPU 가 이 주소로 무조건 점프하기 때문. */
    .text : ALIGN(8) {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata : ALIGN(8) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(8) {
        *(.data .data.*)
    }

    /* NOLOAD: BSS 는 이미지에 포함되지 않는다 (부팅 시 boot.S 가 0으로 채움) */
    .bss (NOLOAD) : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    __image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.ARM.attributes)
    }
}
