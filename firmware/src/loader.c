/* loader.c - 커널을 올리고 리눅스로 넘어간다.
 *
 * ── 어디에 올리나 ────────────────────────────────────────────────
 *
 *   0x00000000  펌웨어 스핀 테이블 (0xd8~0xf0). 건드리면 안 된다
 *   0x00080000  우리 펌웨어. 스택은 여기서 아래로 자란다
 *   0x00200000  리눅스 커널 - 2MB 정렬이 규약이다
 *   0x08000000  디바이스 트리 - 커널이 자라도 닿지 않을 만큼 멀리
 *
 * 커널을 2MB 에 두는 이유는 arm64 부팅 규약이 2MB 정렬을 요구하기
 * 때문이다. DTB 를 128MB 에 두는 이유는 조금 다르다. 커널 이미지는
 * 파일 크기보다 큰 자리를 쓴다 - 헤더의 image_size 가 그 크기이고,
 * 뒤쪽은 BSS 라 커널이 스스로 0 으로 채운다. 파일 바로 뒤에 DTB 를
 * 두면 커널이 자기 BSS 를 지우면서 DTB 도 같이 지운다. 증상은
 * "커널이 조용히 멈춤" 이고, 원인을 찾는 데 아주 오래 걸린다.
 *
 * ── 넘길 때의 규약 (Documentation/arm64/booting.rst) ─────────────
 *   · MMU 꺼짐, D-캐시 꺼짐
 *   · x0 = DTB 물리 주소, x1 = x2 = x3 = 0
 *   · EL2 또는 EL1 (우리는 EL1)
 *   · 보조 코어는 스핀 테이블에서 대기 (boot.S 가 처리한다)
 */
#include "loader.h"
#include "emmc.h"
#include "fat32.h"
#include "fdt.h"
#include "exception.h"
#include "irq.h"
#include "printf.h"
#include "string.h"
#include "uart.h"
#include "mbox.h"
#include "types.h"

/* 앞부분만 비교한다. 우리 string.h 에는 strncmp 가 없고, 필요한 곳이
 * 여기 한 군데뿐이라 라이브러리를 늘리지 않는다. */
static int strncmp_local(const char *a, const char *b, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

#define KERNEL_DEST     0x00200000UL
#define KERNEL_MAX      0x06000000UL    /* 96MB - 어떤 커널이든 넉넉하다 */
#define DTB_DEST        0x08000000UL
#define DTB_MAX         0x00040000UL    /* 256KB */

/* boot.S 가 붙잡아 둔, 펌웨어가 준 DTB 주소. */
extern u64 boot_dtb_addr;

/* boot.S 가 적어둔, 우리가 도는 예외 레벨. */
extern u32 boot_el;

/* config.mk 가 정한 리눅스 커널 파일 이름. Makefile 이 -D 로 넘긴다. */
#ifndef LINUX_IMAGE_NAME
#define LINUX_IMAGE_NAME "Image"
#endif

/* ── arm64 커널 이미지 헤더 ──────────────────────────────────────
 *
 * 압축되지 않은 arm64 Image 는 앞에 64바이트 헤더를 달고 있다.
 * 오프셋 56 의 매직으로 진짜 커널인지 확인할 수 있는데, 이 검사가
 * 없으면 엉뚱한 파일로 뛰어서 아무 메시지 없이 멈춘다 - 부트로더가
 * 낼 수 있는 가장 알아보기 어려운 실패다. */
typedef struct {
    u32 code0;
    u32 code1;
    u64 text_offset;
    u64 image_size;
    u64 flags;
    u64 res2, res3, res4;
    u32 magic;          /* "ARM\x64" = 0x644D5241 */
    u32 res5;
} __attribute__((packed)) arm64_header_t;

#define ARM64_MAGIC 0x644D5241u

/* ── 파일 찾기 ───────────────────────────────────────────────────*/

/* 커널일 법한 이름들. config.mk 가 정한 이름을 가장 먼저 본다. */
static const char *kernel_names[] = {
    LINUX_IMAGE_NAME,
    "Image",
    "vmlinuz",
    "linux.img",
};

/* 디바이스 트리 파일. Zero 2 W 것을 먼저 보고, Pi 3 것도 받아준다 -
 * 같은 SoC 라 대부분 통한다. */
static const char *dtb_names[] = {
    "bcm2710-rpi-zero-2-w.dtb",
    "bcm2710-rpi-3-b-plus.dtb",
    "bcm2710-rpi-3-b.dtb",
    "bcm2837-rpi-3-b.dtb",
};

static bool find_any(const char **names, u32 count,
                     fat_file_t *out, const char **found_name)
{
    for (u32 i = 0; i < count; i++) {
        if (fat32_find(names[i], out)) {
            if (found_name) *found_name = names[i];
            return true;
        }
    }
    return false;
}

/* ── 커맨드라인 ──────────────────────────────────────────────────*/

static char cmdline[512];

/* cmdline.txt 를 읽는다. 줄바꿈은 잘라낸다 - 그대로 넘기면 커널이
 * 마지막 인자에 개행이 붙은 것으로 읽는다. */
static const char *read_cmdline(void)
{
    fat_file_t f;
    if (!fat32_find("cmdline.txt", &f))
        return NULL;
    if (f.size == 0 || f.size >= sizeof cmdline)
        return NULL;

    u32 got = 0;
    if (!fat32_read_file(&f, cmdline, sizeof cmdline - 1, &got))
        return NULL;

    cmdline[got] = '\0';
    for (u32 i = 0; i < got; i++)
        if (cmdline[i] == '\r' || cmdline[i] == '\n')
            cmdline[i] = '\0';

    return cmdline[0] ? cmdline : NULL;
}

/* config.txt 에 dtoverlay= 가 있으면 그 이름들을 말한다.
 *
 * ── 오버레이를 적용하지 않는 이유 ────────────────────────────────
 * 실기에서는 이 문제가 없다. start.elf 가 config.txt 를 읽어 오버레이를
 * 전부 적용한 트리를 만들고, 그 주소를 x0 으로 넘겨준다. 우리는 그
 * 트리를 우선적으로 쓰므로 오버레이가 이미 들어 있다.
 *
 * 카드의 .dtb 파일을 직접 읽는 경로는 start.elf 가 없을 때만 쓰인다 -
 * 실질적으로 QEMU 다. 그 경로에서는 오버레이가 빠지고, 그게 무엇을
 * 뜻하는지는 오버레이마다 다르다. 예를 들어 disable-bt 가 빠지면
 * Zero 2 W 의 PL011 이 블루투스에 물린 채로 남아서 ttyAMA0 콘솔이
 * 아예 생기지 않는다 - 커널은 뜨는데 화면에도 시리얼에도 한 글자도
 * 안 나오고, 원인을 짐작할 단서가 없다.
 *
 * 오버레이 병합을 직접 구현하는 것은 phandle 재배치까지 따라오는 큰
 * 일이고, 정작 실기에서는 쓰이지 않는 경로를 위한 것이다. 그래서
 * 구현하는 대신, 무엇이 빠졌는지 이름을 대고 넘어간다. 조용히 다른
 * 트리로 부팅하는 것보다 낫다. */
static void warn_about_overlays(void)
{
    fat_file_t f;
    if (!fat32_find("config.txt", &f) || f.size == 0 || f.size > 8192)
        return;

    static char cfg[8192];
    u32 got = 0;
    if (!fat32_read_file(&f, cfg, sizeof cfg - 1, &got))
        return;
    cfg[got] = '\0';

    bool said = false;
    for (u32 i = 0; i < got; i++) {
        /* 줄 시작에서만 본다. 주석 안의 dtoverlay 를 세지 않기 위해서다. */
        if (i != 0 && cfg[i - 1] != '\n')
            continue;
        if (strncmp_local(&cfg[i], "dtoverlay=", 10) != 0)
            continue;

        if (!said) {
            kprintf("[boot] 주의: config.txt 가 오버레이를 요구하는데"
                    " 이 경로에서는 적용되지 않는다\n");
            said = true;
        }
        kprintf("[boot]   dtoverlay=");
        for (u32 j = i + 10; j < got && cfg[j] != '\n' && cfg[j] != '\r'; j++)
            kprintf("%c", cfg[j]);
        kprintf("\n");
    }
    if (said)
        kprintf("[boot]   실기에서는 start.elf 가 적용한 트리를 쓰므로"
                " 이 경고가 나오지 않는다\n");
}

/* ── 넘기기 ──────────────────────────────────────────────────────*/

static void jump_to_kernel(u64 entry, u64 dtb) __attribute__((noreturn));
static void jump_to_kernel(u64 entry, u64 dtb)
{
    uart_flush();

    /* 인터럽트를 닫고 타이머를 세운다. 커널이 자기 벡터 테이블을 걸기
     * 전에 우리 타이머가 울리면, 아무도 없는 주소로 예외가 날아간다. */
    timer_tick_stop();
    irq_disable();

    /* 우리가 쓴 커널 이미지가 메모리에 확실히 도달했는지. D-캐시를 켠
     * 적이 없으므로 사실 이미 도달해 있지만, 명령 캐시에는 이 주소의
     * 옛 내용이 남아 있을 수 있다. */
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("ic iallu" ::: "memory");
    __asm__ volatile("dsb sy; isb" ::: "memory");

    /* 인터럽트를 EL2 로 끌어오던 설정을 되돌린다.
     *
     * 우리가 EL2 에서 돌기 위해 HCR_EL2 의 IMO/FMO/AMO 를 세워두었다.
     * 리눅스는 자기 HCR_EL2 를 스스로 설정하지만, 넘겨줄 때의 상태는
     * 다음 주인이 예상할 만한 것이어야 한다 - 부트로더가 남긴 설정이
     * 다음 코드의 첫 인터럽트를 삼키는 일은 없어야 한다. */
    if (boot_el == 2) {
        u64 hcr;
        __asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
        hcr &= ~((1ULL << 5) | (1ULL << 4) | (1ULL << 3));
        __asm__ volatile("msr hcr_el2, %0" :: "r"(hcr));
        __asm__ volatile("isb");
    }

    /* 규약대로 x0 에 DTB, 나머지는 0. */
    register u64 x0 __asm__("x0") = dtb;
    register u64 x1 __asm__("x1") = 0;
    register u64 x2 __asm__("x2") = 0;
    register u64 x3 __asm__("x3") = 0;
    register u64 target __asm__("x4") = entry;

    __asm__ volatile("br x4"
                     :: "r"(x0), "r"(x1), "r"(x2), "r"(x3), "r"(target)
                     : "memory");
    __builtin_unreachable();
}

/* ── 조사 ────────────────────────────────────────────────────────*/

static bool open_card(void)
{
    if (!emmc_init())
        return false;
    kprintf("[sd]");
    emmc_describe();

    if (!fat32_mount())
        return false;
    kprintf("[fat]");
    fat32_describe();
    return true;
}

void boot_survey(void)
{
    if (!open_card()) {
        kprintf("카드를 읽을 수 없다\n");
        return;
    }
    fat32_list();

    fat_file_t f;
    const char *name = NULL;
    if (find_any(kernel_names, sizeof kernel_names / sizeof *kernel_names,
                 &f, &name))
        kprintf("  커널 후보: %s (%u KB)\n", name, f.size / 1024);
    else
        kprintf("  커널로 쓸 파일이 없다\n");

    if (fdt_valid((const void *)(uptr)boot_dtb_addr)) {
        kprintf("  펌웨어가 준 디바이스 트리: %p\n",
                (void *)(uptr)boot_dtb_addr);
        fdt_describe((const void *)(uptr)boot_dtb_addr);
    } else if (find_any(dtb_names, sizeof dtb_names / sizeof *dtb_names,
                        &f, &name)) {
        kprintf("  카드의 디바이스 트리: %s (%u KB)\n", name, f.size / 1024);
    } else {
        kprintf("  디바이스 트리가 없다\n");
    }

    const char *cl = read_cmdline();
    kprintf("  커맨드라인: %s\n", cl ? cl : "(cmdline.txt 없음)");
}

/* ── 부팅 ────────────────────────────────────────────────────────*/

void boot_linux(void)
{
    kprintf("\n[boot] SD 카드에서 리눅스를 찾는다\n");

    if (!open_card()) {
        kprintf("[boot] 카드를 읽을 수 없어 중단한다\n");
        return;
    }

    /* ── 커널 ──────────────────────────────────────────────────*/
    fat_file_t kf;
    const char *kname = NULL;
    if (!find_any(kernel_names, sizeof kernel_names / sizeof *kernel_names,
                  &kf, &kname)) {
        kprintf("[boot] 커널 파일이 없다. 카드에 있는 것:\n");
        fat32_list();
        return;
    }

    kprintf("[boot] %s (%u KB) 를 %p 로 읽는다\n",
            kname, kf.size / 1024, (void *)KERNEL_DEST);

    u32 got = 0;
    if (!fat32_read_file(&kf, (void *)KERNEL_DEST, KERNEL_MAX, &got)) {
        kprintf("[boot] 커널을 읽지 못했다\n");
        return;
    }

    const arm64_header_t *h = (const arm64_header_t *)KERNEL_DEST;
    if (h->magic != ARM64_MAGIC) {
        kprintf("[boot] %s 는 arm64 커널이 아니다"
                " (매직 %08x, 기대값 %08x)\n",
                kname, h->magic, ARM64_MAGIC);
        kprintf("       압축된 이미지는 풀어서 넣어야 한다\n");
        return;
    }

    /* text_offset 은 커널이 원하는 배치 오프셋이다. 요즘 커널은 0 이고
     * 대신 flags 비트 3 으로 "아무 2MB 경계나 좋다" 고 말한다. */
    u64 entry = KERNEL_DEST + h->text_offset;
    if (h->text_offset != 0)
        kprintf("[boot] 커널이 +0x%llx 오프셋을 요구한다\n",
                (unsigned long long)h->text_offset);

    kprintf("[boot] 커널 %u KB 읽음, 진입점 %p, 필요 메모리 %llu KB\n",
            got / 1024, (void *)(uptr)entry,
            (unsigned long long)(h->image_size / 1024));

    /* ── 디바이스 트리 ─────────────────────────────────────────*/
    const void *src_dtb = NULL;

    if (fdt_valid((const void *)(uptr)boot_dtb_addr)) {
        src_dtb = (const void *)(uptr)boot_dtb_addr;
        kprintf("[boot] 펌웨어가 준 디바이스 트리를 쓴다 (%p)\n", src_dtb);
    } else {
        fat_file_t df;
        const char *dname = NULL;
        if (!find_any(dtb_names, sizeof dtb_names / sizeof *dtb_names,
                      &df, &dname)) {
            kprintf("[boot] 디바이스 트리가 없다 - 리눅스는 이것 없이"
                    " 부팅하지 못한다\n");
            return;
        }
        /* 카드에서 읽은 것은 목적지 뒤쪽에 임시로 둔다. 어차피 바로
         * 앞으로 옮겨 적을 것이라 겹치지만 않으면 된다. */
        void *tmp = (void *)(DTB_DEST + DTB_MAX);
        if (!fat32_read_file(&df, tmp, DTB_MAX, NULL)) {
            kprintf("[boot] %s 를 읽지 못했다\n", dname);
            return;
        }
        if (!fdt_valid(tmp)) {
            kprintf("[boot] %s 가 올바른 디바이스 트리가 아니다\n", dname);
            return;
        }
        src_dtb = tmp;
        kprintf("[boot] 카드의 %s 를 쓴다 (%u 바이트)\n",
                dname, fdt_size(tmp));
        warn_about_overlays();
    }

    const char *cl = read_cmdline();
    if (cl)
        kprintf("[boot] 커맨드라인: %s\n", cl);
    else
        kprintf("[boot] cmdline.txt 가 없다 - 트리에 있는 것을 그대로 쓴다\n");

    /* 램이 얼마나 붙어 있는지는 GPU 에게 물어본다. 배포되는 .dtb 의
     * /memory 는 크기가 0 으로 비어 있고, 실기에서는 start.elf 가 이
     * 값을 채워 넣는다. 우리가 그 자리를 대신하는 이상 우리가 채워야
     * 한다 - 안 채우면 커널이 램 0바이트인 기계로 알고 페이지 테이블을
     * 만들다 패닉한다. */
    fdt_fixup_t fix = { cl, 0, 0 };
    u32 mem_base = 0, mem_size = 0;
    if (mbox_get_arm_memory(&mem_base, &mem_size) && mem_size) {
        fix.mem_base = mem_base;
        fix.mem_size = mem_size;
        kprintf("[boot] 램 %u MB (기준 %p) 를 트리에 적는다\n",
                mem_size / (1024 * 1024), (void *)(uptr)mem_base);
    } else {
        kprintf("[boot] GPU 가 램 크기를 알려주지 않는다 -"
                " 트리에 있는 값을 그대로 쓴다\n");
    }

    u32 dtb_size = 0;
    if (!fdt_copy_with_fixups(src_dtb, (void *)DTB_DEST, DTB_MAX,
                              &fix, &dtb_size)) {
        kprintf("[boot] 디바이스 트리를 준비하지 못했다\n");
        return;
    }
    kprintf("[boot] 디바이스 트리 %u 바이트를 %p 에 두었다\n",
            dtb_size, (void *)DTB_DEST);

    kprintf("[boot] 리눅스로 넘어간다\n\n");
    jump_to_kernel(entry, DTB_DEST);
}
