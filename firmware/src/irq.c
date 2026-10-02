/* irq.c - BCM2710 인터럽트 컨트롤러와 ARM generic timer.
 *
 * ── 이 칩의 인터럽트가 두 군데인 이유 ────────────────────────────
 * BCM2710 에는 GIC 가 없다. 대신 두 개의 서로 다른 물건이 있다.
 *
 *   1) 0x3F00B200 의 레거시 컨트롤러. UART, SD, 시스템 타이머 같은
 *      GPU 쪽 페리페럴 64개가 여기 달린다. 우선순위도 없고 어느
 *      코어로 보낼지도 못 고른다. 대기 비트맵을 읽어서 직접 고른다.
 *
 *   2) 0x40000000 의 코어별 로컬 블록. Pi 2 에서 코어가 넷이 되면서
 *      생겼다. ARM generic timer 의 인터럽트는 여기로만 온다 -
 *      레거시 컨트롤러에는 그 선 자체가 없다.
 *
 * 그래서 IRQ 가 들어오면 먼저 (2)의 CORE_IRQ_SOURCE 를 보고, 거기에
 * "GPU 인터럽트" 비트가 서 있으면 그때 (1)을 들여다본다.
 *
 * ── 타이머를 시스템 타이머가 아니라 generic timer 로 하는 이유 ───
 * 0x3F003000 의 시스템 타이머는 1MHz 고정이라 지연 루프에는 완벽하다.
 * 다만 채널 0 과 2 는 GPU 가 쓰고 있어서 ARM 이 건드리면 안 되고,
 * 남는 채널은 32비트라 72분마다 랩어라운드한다.
 *
 * ARM generic timer 는 코어에 붙어 있고 64비트이며, 무엇보다 CNTFRQ_EL0
 * 를 읽으면 자기 주파수를 스스로 알려준다. 실기(19.2MHz)와 QEMU(보통
 * 62.5MHz)의 주파수가 다른데, 숫자를 박아두지 않아도 양쪽에서 같은
 * 코드가 맞는 시간에 울린다.
 */
#include "irq.h"
#include "exception.h"
#include "bcm2710.h"
#include "mmio.h"
#include "printf.h"
#include "types.h"

#define NUM_GPU_IRQS  64

/* boot.S 가 적어둔, 우리가 도는 예외 레벨. */
extern u32 boot_el;

static irq_fn handlers[NUM_GPU_IRQS];
static volatile u64 ticks;
static u32 tick_reload;          /* 틱 하나에 몇 카운트인가 */
static bool tick_running;

/* ── generic timer 레지스터 ──────────────────────────────────────*/

static inline u64 cntfrq(void)
{
    u64 v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v;
}

/* ── EL 마다 타이머가 다르다 ─────────────────────────────────────
 *
 * 코어마다 물리 타이머가 여러 벌 있고, 예외 레벨마다 자기 것을 쓴다.
 *
 *   CNTP_*   EL1 의 물리 타이머  -> 인터럽트 선 CNTPNSIRQ (비트 1)
 *   CNTHP_*  EL2 의 물리 타이머  -> 인터럽트 선 CNTHPIRQ  (비트 2)
 *
 * 처음에는 EL 과 무관하게 CNTP_* 만 썼다. EL1 에서 돌 때는 맞았고,
 * 펌웨어를 EL2 에 머물게 고친 순간 타이머가 조용히 멈췄다 - 레지스터
 * 쓰기는 전부 성공하고, CNTP_CTL 을 되읽으면 켜져 있다고 나오는데,
 * 인터럽트만 오지 않는다. 라즈베리파이의 로컬 인터럽트 컨트롤러에서
 * 두 타이머가 서로 다른 선에 물려 있기 때문이다.
 *
 * 그래서 도는 예외 레벨에 맞는 타이머를 쓰고, 그에 맞는 선을 연다. */
static inline void set_tval(u32 v)
{
    if (boot_el == 2)
        __asm__ volatile("msr cnthp_tval_el2, %0" :: "r"((u64)v));
    else
        __asm__ volatile("msr cntp_tval_el0, %0" :: "r"((u64)v));
}

static inline void set_ctl(u64 v)
{
    if (boot_el == 2)
        __asm__ volatile("msr cnthp_ctl_el2, %0" :: "r"(v));
    else
        __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
    __asm__ volatile("isb");
}

/* 이 예외 레벨의 타이머가 물린 인터럽트 선. */
static inline u32 timer_irq_bit(void)
{
    return (boot_el == 2) ? CORE_IRQ_CNTHP : CORE_IRQ_CNTPNS;
}

/* ── 컨트롤러 ────────────────────────────────────────────────────*/

void irq_init(void)
{
    /* 들어오는 것을 전부 막고 시작한다.
     *
     * start.elf 가 무엇을 켜둔 채로 넘겨주는지 우리는 모른다. 벡터
     * 테이블을 걸자마자 남의 인터럽트가 들어오면 첫 번째 패닉이
     * "알 수 없는 IRQ" 가 되는데, 그건 원인을 찾는 데 아무 도움이
     * 안 되면서 진짜 부팅을 막는다. */
    mmio_write32(DISABLE_IRQS_1, 0xFFFFFFFFu);
    mmio_write32(DISABLE_IRQS_2, 0xFFFFFFFFu);
    mmio_write32(DISABLE_BASIC_IRQS, 0xFFFFFFFFu);

    for (u32 i = 0; i < NUM_GPU_IRQS; i++)
        handlers[i] = NULL;

    /* 코어 0 만 쓴다. 나머지 셋은 boot.S 에서 자고 있다. */
    mmio_write32(CORE_TIMER_IRQCNTL(0), 0);
    mmio_write32(CORE_MBOX_IRQCNTL(0), 0);

    /* GPU 인터럽트를 코어 0 의 IRQ 로 보낸다. 리셋값이 이미 그렇지만,
     * 부트로더가 다르게 두고 갔을 수 있다. */
    mmio_write32(LOCAL_GPU_ROUTING, 0);

    ticks = 0;
    tick_running = false;
}

void irq_register(u32 n, irq_fn fn)
{
    if (n >= NUM_GPU_IRQS)
        return;

    handlers[n] = fn;
    if (n < 32)
        mmio_write32(ENABLE_IRQS_1, 1u << n);
    else
        mmio_write32(ENABLE_IRQS_2, 1u << (n - 32));
}

void irq_unregister(u32 n)
{
    if (n >= NUM_GPU_IRQS)
        return;

    if (n < 32)
        mmio_write32(DISABLE_IRQS_1, 1u << n);
    else
        mmio_write32(DISABLE_IRQS_2, 1u << (n - 32));
    handlers[n] = NULL;
}

/* ── 타이머 틱 ───────────────────────────────────────────────────*/

void timer_tick_start(u32 hz)
{
    if (hz == 0)
        return;

    u64 freq = cntfrq();
    if (freq == 0) {
        /* 펌웨어가 CNTFRQ 를 안 채워준 보드. 값을 지어내면 시간이
         * 전부 틀리므로, 조용히 틀리느니 말하고 그만둔다. */
        kprintf("[irq] CNTFRQ_EL0 이 0 이다 - 타이머 틱 없이 진행\n");
        return;
    }

    tick_reload = (u32)(freq / hz);
    if (tick_reload == 0)
        tick_reload = 1;

    mmio_write32(CORE_TIMER_IRQCNTL(0), timer_irq_bit());
    set_tval(tick_reload);
    set_ctl(1);                     /* ENABLE=1, IMASK=0 */
    tick_running = true;
}

void timer_tick_stop(void)
{
    set_ctl(0);
    mmio_write32(CORE_TIMER_IRQCNTL(0), 0);
    tick_running = false;
}

u64 timer_ticks(void)
{
    return ticks;
}

/* 진단용. 타이머가 안 울릴 때 "몇 Hz 로 무엇을 걸었나" 를 먼저 봐야
 * 한다 - 주파수를 잘못 읽으면 재장전 값이 터무니없어지고, 그러면
 * 인터럽트가 한 번 오고 다시는 안 온다. */
void timer_tick_debug(void)
{
    u64 ctl, tval, cval, cnt;
    if (boot_el == 2) {
        __asm__ volatile("mrs %0, cnthp_ctl_el2"  : "=r"(ctl));
        __asm__ volatile("mrs %0, cnthp_tval_el2" : "=r"(tval));
        __asm__ volatile("mrs %0, cnthp_cval_el2" : "=r"(cval));
    } else {
        __asm__ volatile("mrs %0, cntp_ctl_el0"  : "=r"(ctl));
        __asm__ volatile("mrs %0, cntp_tval_el0" : "=r"(tval));
        __asm__ volatile("mrs %0, cntp_cval_el0" : "=r"(cval));
    }
    __asm__ volatile("mrs %0, cntpct_el0"    : "=r"(cnt));

    kprintf("  CNTFRQ %llu Hz, reload %u (running=%d)\n",
            (unsigned long long)cntfrq(), tick_reload, (int)tick_running);
    kprintf("  CNTP_CTL %llx  next fire in %lld counts (%llu -> %llu)\n",
            (unsigned long long)ctl, (long long)(s64)(s32)tval,
            (unsigned long long)cnt, (unsigned long long)cval);
    kprintf("  EL%u 타이머, 인터럽트 선 %08x\n", boot_el, timer_irq_bit());
    kprintf("  core 0 routing %08x, pending %08x\n",
            mmio_read32(CORE_TIMER_IRQCNTL(0)),
            mmio_read32(CORE_IRQ_SOURCE(0)));
}

/* ── 디스패치 ────────────────────────────────────────────────────*/

static void handle_gpu_irqs(const exc_frame_t *f)
{
    u32 p1 = mmio_read32(IRQ_PENDING_1);
    u32 p2 = mmio_read32(IRQ_PENDING_2);

    for (u32 i = 0; i < 32; i++)
        if ((p1 & (1u << i)) && handlers[i])
            handlers[i]();

    for (u32 i = 0; i < 32; i++)
        if ((p2 & (1u << i)) && handlers[i + 32])
            handlers[i + 32]();

    /* 대기 중인데 핸들러가 없는 인터럽트는 영원히 대기 상태로 남아
     * 있게 된다. 그러면 eret 하자마자 다시 들어와서 보드가 인터럽트
     * 폭풍 속에 갇힌다 - 겉으로는 그냥 멈춘 것처럼 보인다. 어느
     * 선인지 말하고 세우는 편이 훨씬 낫다. */
    u32 unclaimed1 = p1, unclaimed2 = p2;
    for (u32 i = 0; i < 32; i++) {
        if (handlers[i])      unclaimed1 &= ~(1u << i);
        if (handlers[i + 32]) unclaimed2 &= ~(1u << i);
    }
    if (unclaimed1 || unclaimed2)
        panic(f, "처리할 수 없는 GPU 인터럽트 (pending1=%08x pending2=%08x)",
              unclaimed1, unclaimed2);
}

void irq_handler(exc_frame_t *f)
{
    /* 프레임의 ESR/ELR 은 아직 비어 있다. 여기서 쓰지 않지만, 아래
     * 패닉 경로가 프레임을 넘기므로 그때 필요하다. panic() 은 프레임의
     * 범용 레지스터와 백트레이스만 쓰므로 이대로도 맞는다. */
    u32 src = mmio_read32(CORE_IRQ_SOURCE(0));

    if (src & timer_irq_bit()) {
        ticks++;
        if (tick_running)
            set_tval(tick_reload);  /* 다음 틱 예약 */
        else
            set_ctl(0);
        src &= ~timer_irq_bit();
    }

    if (src & CORE_IRQ_GPU) {
        handle_gpu_irqs(f);
        src &= ~CORE_IRQ_GPU;
    }

    if (src)
        panic(f, "처리할 수 없는 코어 인터럽트 (source=%08x)", src);
}
