/* exception.c - 예외가 났을 때 무슨 일인지 말해주는 코드.
 *
 * ── 왜 이게 필요한가 ─────────────────────────────────────────────
 * Phase 1 의 펌웨어는 잘못되면 그냥 조용해졌다. 널 포인터를 쓰든,
 * 정렬 안 된 주소에서 64비트를 읽든, 스택을 넘기든 증상이 똑같았다 -
 * UART 가 멈추고 그걸로 끝. 어느 줄에서 죽었는지 알아내는 방법이
 * "코드를 반으로 잘라 kprintf 를 넣고 다시 굽기" 밖에 없었다.
 *
 * CPU 는 사실 다 알고 있다. ESR_EL1 에 무슨 예외인지, FAR_EL1 에 어느
 * 주소를 건드리다 그랬는지, ELR_EL1 에 어느 명령에서였는지가 들어
 * 있다. 벡터 테이블만 걸어두면 그 순간 우리 코드로 넘어온다. 이 파일은
 * 그 셋을 사람이 읽을 수 있는 문장으로 옮긴다.
 */
#include "exception.h"
#include "asm-offsets.h"
#include "printf.h"
#include "console.h"
#include "uart.h"
#include "timer.h"
#include "bcm2710.h"
#include "mmio.h"
#include "types.h"

#include <stdarg.h>

/* asm-offsets.h 의 숫자와 실제 구조체가 어긋나면 레지스터 덤프가
 * 조용히 거짓말을 한다. 틀린 값을 보여주는 패닉 출력은 없느니만
 * 못하므로, 빌드에서 세운다. */
STATIC_ASSERT(__builtin_offsetof(exc_frame_t, sp)   == FRAME_SP,   "FRAME_SP");
STATIC_ASSERT(__builtin_offsetof(exc_frame_t, elr)  == FRAME_ELR,  "FRAME_ELR");
STATIC_ASSERT(__builtin_offsetof(exc_frame_t, spsr) == FRAME_SPSR, "FRAME_SPSR");
STATIC_ASSERT(__builtin_offsetof(exc_frame_t, esr)  == FRAME_ESR,  "FRAME_ESR");
STATIC_ASSERT(__builtin_offsetof(exc_frame_t, far)  == FRAME_FAR,  "FRAME_FAR");
STATIC_ASSERT(__builtin_offsetof(exc_frame_t, kind) == FRAME_KIND, "FRAME_KIND");
STATIC_ASSERT(sizeof(exc_frame_t) <= FRAME_SIZE, "frame does not fit");
STATIC_ASSERT(FRAME_SIZE % 16 == 0, "AArch64 SP must stay 16-byte aligned");

extern char vector_table[];

/* boot.S 가 적어둔, 우리가 실제로 도는 예외 레벨 (2 또는 1). */
extern u32 boot_el;

/* 예외의 사정은 도는 예외 레벨의 레지스터에 담긴다. EL2 에서 돌면서
 * ESR_EL1 을 읽으면 남의 값을 - 대개 0 을 - 읽게 되고, 패닉 출력이
 * 전부 거짓말이 된다. 그래서 벡터에서 어셈블리로 읽지 않고 여기서
 * 갈라 읽는다. 분기 하나가 어셈블리 두 벌보다 낫다. */
static void read_exception_state(exc_frame_t *f)
{
    if (boot_el == 2) {
        __asm__ volatile("mrs %0, esr_el2"  : "=r"(f->esr));
        __asm__ volatile("mrs %0, elr_el2"  : "=r"(f->elr));
        __asm__ volatile("mrs %0, spsr_el2" : "=r"(f->spsr));
        __asm__ volatile("mrs %0, far_el2"  : "=r"(f->far));
    } else {
        __asm__ volatile("mrs %0, esr_el1"  : "=r"(f->esr));
        __asm__ volatile("mrs %0, elr_el1"  : "=r"(f->elr));
        __asm__ volatile("mrs %0, spsr_el1" : "=r"(f->spsr));
        __asm__ volatile("mrs %0, far_el1"  : "=r"(f->far));
    }
}
extern char __image_start[];
extern char __image_end[];

/* 벡터 테이블의 몇 번째 칸으로 들어왔는지. 이것만으로도 절반은
 * 알 수 있다 - 예를 들어 "하위 EL, AArch32" 칸으로 들어왔다면
 * 예외 자체보다 어떻게 거기까지 갔는지가 진짜 문제다. */
static const char *kind_name(u64 kind)
{
    static const char *names[16] = {
        "Sync (현재 EL, SP0)",   "IRQ (현재 EL, SP0)",
        "FIQ (현재 EL, SP0)",    "SError (현재 EL, SP0)",
        "Sync",                  "IRQ",
        "FIQ",                   "SError",
        "Sync (하위 EL, 64비트)", "IRQ (하위 EL, 64비트)",
        "FIQ (하위 EL, 64비트)",  "SError (하위 EL, 64비트)",
        "Sync (하위 EL, 32비트)", "IRQ (하위 EL, 32비트)",
        "FIQ (하위 EL, 32비트)",  "SError (하위 EL, 32비트)",
    };
    return kind < 16 ? names[kind] : "알 수 없는 칸";
}

/* ESR_EL1 의 예외 클래스(EC, 상위 6비트). */
static const char *ec_name(u32 ec)
{
    switch (ec) {
    case 0x00: return "알 수 없는 이유";
    case 0x01: return "WFI/WFE 트랩";
    case 0x07: return "FP/SIMD 접근 트랩";
    case 0x0E: return "잘못된 실행 상태";
    case 0x15: return "SVC (시스템 콜)";
    case 0x18: return "MSR/MRS 트랩";
    case 0x20: return "명령 인출 실패 (하위 EL)";
    case 0x21: return "명령 인출 실패";
    case 0x22: return "PC 정렬 오류";
    case 0x24: return "데이터 접근 실패 (하위 EL)";
    case 0x25: return "데이터 접근 실패";
    case 0x26: return "SP 정렬 오류";
    case 0x2C: return "부동소수점 예외";
    case 0x2F: return "SError";
    case 0x30: case 0x31: return "브레이크포인트";
    case 0x32: case 0x33: return "소프트웨어 스텝";
    case 0x34: case 0x35: return "워치포인트";
    case 0x3C: return "BRK 명령";
    default:   return "정의되지 않은 클래스";
    }
}

/* 접근 실패(abort)의 세부 원인. ISS 하위 6비트(DFSC/IFSC).
 *
 * MMU 를 켜지 않은 우리 펌웨어에서 실제로 볼 수 있는 것은 거의
 * "외부 abort" 와 "정렬 오류" 두 가지다. 나머지는 MMU 를 켠 뒤에
 * 의미가 생기는데, 그때 이 표가 없으면 또 매뉴얼을 뒤져야 한다. */
static const char *fault_name(u32 fsc)
{
    if ((fsc & 0x3C) == 0x00) return "주소 크기 오류";
    if ((fsc & 0x3C) == 0x04) return "변환 오류 (매핑 없음)";
    if ((fsc & 0x3C) == 0x08) return "액세스 플래그 오류";
    if ((fsc & 0x3C) == 0x0C) return "권한 오류";
    switch (fsc) {
    case 0x10: return "동기 외부 abort (그런 주소가 없다)";
    case 0x11: return "태그 검사 오류";
    case 0x21: return "정렬 오류";
    case 0x30: return "TLB 충돌";
    default:   return "분류되지 않은 오류";
    }
}

/* 주소가 우리 이미지 안인지. 백트레이스에서 "이건 우리 코드"라고
 * 표시해주면 스택에 우연히 남은 값과 진짜 복귀 주소를 구별하기 쉽다. */
static bool in_image(u64 addr)
{
    return addr >= (u64)(uptr)__image_start && addr < (u64)(uptr)__image_end;
}

/* 스택을 되짚어 어디서 어디로 불려왔는지 보여준다.
 *
 * -fno-omit-frame-pointer 로 빌드하므로 x29 가 프레임 체인을 이룬다.
 * [x29] 에 이전 x29, [x29+8] 에 복귀 주소가 있다.
 *
 * 깨진 스택 위에서 도는 코드라 방어가 필요하다. 프레임 포인터는
 * 16바이트 정렬이어야 하고, 스택은 아래로 자라므로 다음 프레임은
 * 반드시 더 높은 주소여야 하며, 램 밖으로 나가면 멈춘다. 조건 하나만
 * 빠져도 패닉 핸들러가 패닉을 내면서 진짜 원인을 지워버린다. */
#define RAM_LIMIT   PERIPHERAL_BASE     /* 이 위는 램이 아니라 레지스터 */
#define MAX_FRAMES  12

static void backtrace(const exc_frame_t *f)
{
    kprintf("  호출 경로:\n");

    /* 가장 안쪽 프레임의 복귀 주소는 아직 스택이 아니라 x30 에 있다. */
    u64 lr = f->x[30];
    if (in_image(lr))
        kprintf("    %016llx  <- x30\n", (unsigned long long)lr);

    u64 fp = f->x[29];
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (fp == 0 || (fp & 15) || fp >= RAM_LIMIT || fp < 0x1000)
            break;

        u64 next = *(volatile u64 *)(uptr)fp;
        u64 ret  = *(volatile u64 *)(uptr)(fp + 8);

        if (ret == 0)
            break;
        kprintf("    %016llx%s\n", (unsigned long long)ret,
                in_image(ret) ? "" : "  (이미지 밖)");

        if (next <= fp)             /* 위로 가지 않으면 체인이 깨진 것 */
            break;
        fp = next;
    }
}

static void dump_registers(const exc_frame_t *f)
{
    kprintf("  레지스터:\n");
    for (int i = 0; i < 30; i += 2)
        kprintf("    x%-2d %016llx    x%-2d %016llx\n",
                i,     (unsigned long long)f->x[i],
                i + 1, (unsigned long long)f->x[i + 1]);
    kprintf("    x30 %016llx    sp  %016llx\n",
            (unsigned long long)f->x[30], (unsigned long long)f->sp);
}

/* 예외 하나를 사람이 읽는 형태로. */
static void report(const exc_frame_t *f)
{
    u32 ec  = (u32)(f->esr >> 26) & 0x3F;
    u32 iss = (u32)(f->esr & 0x1FFFFFF);

    kprintf("\n");
    kprintf("=====================================================\n");
    kprintf("  펌웨어 패닉 - %s\n", kind_name(f->kind));
    kprintf("=====================================================\n");
    kprintf("  원인:   %s (EC=0x%02x)\n", ec_name(ec), ec);

    /* 접근 실패라면 어느 주소를, 읽다가인지 쓰다가인지까지 말한다.
     * 이 한 줄이 "널 포인터를 썼다" 와 "널 포인터를 읽었다" 를
     * 구별해주고, 그 둘은 보통 아주 다른 버그다. */
    if (ec == 0x24 || ec == 0x25) {
        kprintf("  주소:   %016llx 를 %s 중\n",
                (unsigned long long)f->far,
                (iss & (1u << 6)) ? "쓰는" : "읽는");
        kprintf("  세부:   %s\n", fault_name(iss & 0x3F));
    } else if (ec == 0x20 || ec == 0x21) {
        kprintf("  주소:   %016llx 에서 명령을 읽으려다\n",
                (unsigned long long)f->far);
        kprintf("  세부:   %s\n", fault_name(iss & 0x3F));
    } else if (ec == 0x22 || ec == 0x26) {
        kprintf("  주소:   %016llx\n", (unsigned long long)f->far);
    }

    kprintf("  명령:   %016llx\n", (unsigned long long)f->elr);
    if (in_image(f->elr))
        kprintf("          (이미지 시작에서 +0x%llx)\n",
                (unsigned long long)(f->elr - (u64)(uptr)__image_start));
    else
        kprintf("          (우리 이미지 밖이다 - 함수 포인터가"
                " 깨졌을 가능성이 크다)\n");

    kprintf("  ESR:    %016llx    SPSR: %016llx\n",
            (unsigned long long)f->esr, (unsigned long long)f->spsr);
    kprintf("\n");

    backtrace(f);
    kprintf("\n");
    dump_registers(f);
}

/* ── 정지 ────────────────────────────────────────────────────────
 *
 * 재부팅하지 않고 세운다. 부팅 초반에 죽는 버그는 자동으로 재부팅하면
 * 화면을 읽기도 전에 사라지는 무한 루프가 되고, 그러면 패닉 출력이
 * 있으나 마나다. 사람이 읽고 전원을 내리는 편이 낫다. */
static void halt_forever(void) __attribute__((noreturn));
static void halt_forever(void)
{
    kprintf("=====================================================\n");
    kprintf("  보드를 세웁니다. 전원을 껐다 켜세요.\n");
    kprintf("=====================================================\n");

    uart_flush();
    irq_disable();
    for (;;)
        __asm__ volatile("wfe");
}

void exception_handler(exc_frame_t *f)
{
    read_exception_state(f);
    report(f);
    halt_forever();
}

void panic(const exc_frame_t *f, const char *fmt, ...)
{
    kprintf("\n");
    kprintf("=====================================================\n");
    kprintf("  펌웨어 패닉 - ");

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    kprintf("\n=====================================================\n");

    if (f) {
        backtrace(f);
        kprintf("\n");
        dump_registers(f);
    }
    halt_forever();
}

/* ── 설치 ────────────────────────────────────────────────────────*/

void exception_init(void)
{
    /* boot.S 가 이미 걸어두었다. 여기서 한 번 더 거는 이유는, 이
     * 함수를 부르는 쪽이 "이제 예외를 받을 준비가 됐다" 고 말할 수
     * 있어야 하기 때문이다. 같은 값을 다시 써도 해가 없다. */
    if (boot_el == 2)
        __asm__ volatile("msr vbar_el2, %0" :: "r"(vector_table));
    else
        __asm__ volatile("msr vbar_el1, %0" :: "r"(vector_table));
    __asm__ volatile("isb");
}

void irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2");   /* I 비트 해제 */
}

void irq_disable(void)
{
    __asm__ volatile("msr daifset, #2");
}
