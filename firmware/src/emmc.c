/* emmc.c - Arasan SDHCI (BCM2710 EMMC) 드라이버, 읽기 전용.
 *
 * ── SD 카드를 깨우는 순서 ────────────────────────────────────────
 * 규격이 정한 순서가 있고, 건너뛸 수 있는 단계가 없다. 카드는 전원이
 * 들어온 직후 아무 명령도 받지 않는 "idle" 상태에 있고, 정해진 대화를
 * 거쳐야 데이터를 준다.
 *
 *   CMD0    GO_IDLE          - 모든 카드를 알려진 상태로
 *   CMD8    SEND_IF_COND     - 2.0 이후 카드인지, 전압이 맞는지
 *   ACMD41  SD_SEND_OP_COND  - 준비될 때까지 반복해서 물어본다
 *   CMD2    ALL_SEND_CID     - 카드 식별 정보
 *   CMD3    SEND_REL_ADDR    - 카드에 주소(RCA)를 받아온다
 *   CMD7    SELECT_CARD      - 그 주소로 카드 하나를 고른다
 *   CMD16   SET_BLOCKLEN     - 512바이트로 (SDHC 는 이미 512 고정)
 *
 * ACMD 는 "응용 명령" 이라 앞에 CMD55 를 붙여야 한다. 붙이지 않으면
 * 같은 번호의 전혀 다른 일반 명령으로 해석된다.
 *
 * ── 400kHz 로 시작해서 25MHz 로 올리는 이유 ─────────────────────
 * 카드가 어떤 속도를 견디는지는 대화를 해봐야 안다. 그래서 규격이
 * 식별 구간의 속도를 400kHz 이하로 못박아 두었다. 카드를 고르고 나서
 * 올린다. 처음부터 빠르게 시작하면 어떤 카드는 되고 어떤 카드는
 * 안 되는데, 그 차이가 왜 나는지는 영영 알 수 없다.
 */
#include "emmc.h"
#include "bcm2710.h"
#include "mmio.h"
#include "gpio.h"
#include "timer.h"
#include "printf.h"
#include "string.h"

/* ── SDHCI 레지스터 (EMMC_BASE 기준) ─────────────────────────────*/
#define EMMC_ARG2           (EMMC_BASE + 0x00)
#define EMMC_BLKSIZECNT     (EMMC_BASE + 0x04)
#define EMMC_ARG1           (EMMC_BASE + 0x08)
#define EMMC_CMDTM          (EMMC_BASE + 0x0C)
#define EMMC_RESP0          (EMMC_BASE + 0x10)
#define EMMC_RESP1          (EMMC_BASE + 0x14)
#define EMMC_RESP2          (EMMC_BASE + 0x18)
#define EMMC_RESP3          (EMMC_BASE + 0x1C)
#define EMMC_DATA           (EMMC_BASE + 0x20)
#define EMMC_STATUS         (EMMC_BASE + 0x24)
#define EMMC_CONTROL0       (EMMC_BASE + 0x28)
#define EMMC_CONTROL1       (EMMC_BASE + 0x2C)
#define EMMC_INTERRUPT      (EMMC_BASE + 0x30)
#define EMMC_IRPT_MASK      (EMMC_BASE + 0x34)
#define EMMC_IRPT_EN        (EMMC_BASE + 0x38)
#define EMMC_CONTROL2       (EMMC_BASE + 0x3C)
#define EMMC_SLOTISR_VER    (EMMC_BASE + 0xFC)

/* STATUS */
#define SR_CMD_INHIBIT      (1u << 0)
#define SR_DAT_INHIBIT      (1u << 1)
#define SR_READ_AVAILABLE   (1u << 11)

/* CONTROL1 */
#define C1_CLK_INTLEN       (1u << 0)
#define C1_CLK_STABLE       (1u << 1)
#define C1_CLK_EN           (1u << 2)
#define C1_TOUNIT_MAX       (0xEu << 16)
#define C1_SRST_HC          (1u << 24)
#define C1_SRST_CMD         (1u << 25)
#define C1_SRST_DATA        (1u << 26)

/* INTERRUPT */
#define INT_CMD_DONE        (1u << 0)
#define INT_DATA_DONE       (1u << 1)
#define INT_READ_RDY        (1u << 5)
#define INT_ERROR_MASK      0xFFFF0000u

/* CMDTM - 명령을 어떻게 보낼지 */
#define TM_DAT_DIR_CD       (1u << 4)   /* 카드 -> 호스트 */
#define TM_MULTI_BLOCK      (1u << 5)
#define TM_BLKCNT_EN        (1u << 1)
#define TM_AUTO_CMD12       (1u << 2)
#define CMD_RSPNS_136       (1u << 16)
#define CMD_RSPNS_48        (2u << 16)
#define CMD_RSPNS_48B       (3u << 16)
#define CMD_CRCCHK_EN       (1u << 19)
#define CMD_IS_DATA         (1u << 21)
#define CMD_INDEX(n)        ((u32)(n) << 24)

/* 우리가 쓰는 명령들. 응답 형식이 명령마다 달라서 같이 묶어둔다. */
#define CMD_GO_IDLE         (CMD_INDEX(0))
#define CMD_ALL_SEND_CID    (CMD_INDEX(2)  | CMD_RSPNS_136 | CMD_CRCCHK_EN)
#define CMD_SEND_REL_ADDR   (CMD_INDEX(3)  | CMD_RSPNS_48  | CMD_CRCCHK_EN)
#define CMD_SELECT_CARD     (CMD_INDEX(7)  | CMD_RSPNS_48B | CMD_CRCCHK_EN)
#define CMD_SEND_IF_COND    (CMD_INDEX(8)  | CMD_RSPNS_48  | CMD_CRCCHK_EN)
#define CMD_SEND_CSD        (CMD_INDEX(9)  | CMD_RSPNS_136 | CMD_CRCCHK_EN)
#define CMD_SET_BLOCKLEN    (CMD_INDEX(16) | CMD_RSPNS_48  | CMD_CRCCHK_EN)
#define CMD_READ_SINGLE     (CMD_INDEX(17) | CMD_RSPNS_48  | CMD_CRCCHK_EN \
                             | CMD_IS_DATA | TM_DAT_DIR_CD)
#define CMD_READ_MULTI      (CMD_INDEX(18) | CMD_RSPNS_48  | CMD_CRCCHK_EN \
                             | CMD_IS_DATA | TM_DAT_DIR_CD \
                             | TM_MULTI_BLOCK | TM_BLKCNT_EN | TM_AUTO_CMD12)
#define CMD_APP             (CMD_INDEX(55) | CMD_RSPNS_48  | CMD_CRCCHK_EN)
#define ACMD_SEND_OP_COND   (CMD_INDEX(41) | CMD_RSPNS_48)

/* ── 카드 상태 ───────────────────────────────────────────────────*/
static struct {
    bool ready;
    bool sdhc;          /* 블록 주소인가 바이트 주소인가 */
    u32  rca;           /* 카드 주소 */
    u64  blocks;        /* 512바이트 블록 수 */
    u32  cid[4];
} card;

/* ── 낮은 층 ─────────────────────────────────────────────────────*/

/* 조건이 될 때까지 기다린다. 무한정 기다리지 않는 것이 핵심이다.
 * 카드가 없거나 죽어 있으면 여기서 영원히 멈추는데, 그건 부팅
 * 실패보다 나쁘다 - 실패는 메시지를 남기지만 정지는 아무것도 남기지
 * 않는다. */
static bool wait_status(u32 mask, bool set, u32 timeout_ms)
{
    u64 deadline = timer_get_us() + (u64)timeout_ms * 1000;
    for (;;) {
        u32 s = mmio_read32(EMMC_STATUS);
        if (((s & mask) != 0) == set)
            return true;
        if (timer_get_us() > deadline)
            return false;
    }
}

static bool wait_interrupt(u32 mask, u32 timeout_ms, u32 *err_out)
{
    u64 deadline = timer_get_us() + (u64)timeout_ms * 1000;
    for (;;) {
        u32 i = mmio_read32(EMMC_INTERRUPT);
        if (i & (mask | INT_ERROR_MASK)) {
            /* 읽은 비트를 되써서 지운다 (write-1-to-clear). */
            mmio_write32(EMMC_INTERRUPT, i & (mask | INT_ERROR_MASK));
            if (i & INT_ERROR_MASK) {
                if (err_out) *err_out = i;
                return false;
            }
            return true;
        }
        if (timer_get_us() > deadline) {
            if (err_out) *err_out = 0;
            return false;
        }
    }
}

/* SD 클럭을 원하는 주파수에 가장 가깝게 맞춘다.
 *
 * 분주기는 2의 거듭제곱만 되고, 레지스터에 넣는 값은 "나누고 싶은
 * 수의 절반" 이다. 규격이 그렇게 정해두었다 - 처음 보면 반드시
 * 두 배 틀리는 자리라서 적어둔다. */
static bool set_clock(u32 target_hz)
{
    const u32 base_hz = 41666667;   /* EMMC 입력 클럭 (Pi 3 계열) */

    u32 div = 1;
    while ((base_hz / div) > target_hz && div < 2048)
        div <<= 1;

    u32 half = div >> 1;            /* 레지스터가 원하는 값 */

    if (!wait_status(SR_CMD_INHIBIT | SR_DAT_INHIBIT, false, 1000))
        return false;

    /* 클럭을 끄고 분주기를 바꾼 뒤 다시 켠다. 도는 클럭의 분주기를
     * 바꾸면 카드가 글리치를 명령으로 잘못 볼 수 있다. */
    mmio_clear_bits(EMMC_CONTROL1, C1_CLK_EN);
    delay_us(10);

    u32 c1 = mmio_read32(EMMC_CONTROL1);
    c1 &= ~0x0000FFE0u;                       /* 기존 분주기/타임아웃 제거 */
    c1 |= ((half & 0xFF) << 8);               /* 하위 8비트 */
    c1 |= (((half >> 8) & 0x3) << 6);         /* 상위 2비트 (SDHCI 3.0) */
    c1 |= C1_TOUNIT_MAX | C1_CLK_INTLEN;
    mmio_write32(EMMC_CONTROL1, c1);

    if (!wait_status(0, false, 1))            /* 짧은 정착 대기 */
        { }
    u64 deadline = timer_get_us() + 100000;
    while (!(mmio_read32(EMMC_CONTROL1) & C1_CLK_STABLE))
        if (timer_get_us() > deadline)
            return false;

    mmio_set_bits(EMMC_CONTROL1, C1_CLK_EN);
    delay_us(10);
    return true;
}

/* 명령 하나를 보내고 응답을 기다린다. */
static bool send_cmd(u32 cmd, u32 arg, u32 timeout_ms)
{
    if (!wait_status(SR_CMD_INHIBIT, false, timeout_ms))
        return false;
    if ((cmd & CMD_IS_DATA) &&
        !wait_status(SR_DAT_INHIBIT, false, timeout_ms))
        return false;

    mmio_write32(EMMC_INTERRUPT, mmio_read32(EMMC_INTERRUPT));  /* 묵은 것 정리 */
    mmio_write32(EMMC_ARG1, arg);
    mmio_write32(EMMC_CMDTM, cmd);

    u32 err = 0;
    if (!wait_interrupt(INT_CMD_DONE, timeout_ms, &err)) {
        kprintf("[emmc] CMD%u 실패 (interrupt=%08x)\n",
                (cmd >> 24) & 0x3F, err);
        return false;
    }
    return true;
}

/* CMD55 를 앞세운 응용 명령. */
static bool send_acmd(u32 cmd, u32 arg, u32 timeout_ms)
{
    if (!send_cmd(CMD_APP, card.rca << 16, timeout_ms))
        return false;
    return send_cmd(cmd, arg, timeout_ms);
}

/* 128비트 CSD 에서 [start + width - 1 : start] 를 꺼낸다.
 *
 * csd[0] 이 하위 32비트다. 데이터시트가 쓰는 비트 번호를 그대로 넣을
 * 수 있게 만들어 두면, 필드를 옮겨 적다 틀리는 일이 없다. */
static u32 csd_bits(const u32 csd[4], u32 start, u32 width)
{
    u32 word = start / 32;
    u32 shift = start % 32;
    u64 v = csd[word] >> shift;

    /* 필드가 워드 경계를 넘으면 다음 워드에서 마저 가져온다. */
    if (shift && word < 3)
        v |= (u64)csd[word + 1] << (32 - shift);

    return (u32)(v & ((width >= 32) ? 0xFFFFFFFFu : ((1u << width) - 1)));
}

/* ── 초기화 ──────────────────────────────────────────────────────*/

/* SD 카드 배선을 EMMC 컨트롤러로 돌린다.
 *
 * Pi 3 계열은 부팅 후 SD 카드가 SDHOST 에 붙어 있다. GPIO 48~53 을
 * ALT3 으로 두면 EMMC 쪽으로 넘어온다. 이 여섯 줄이 없으면 그 뒤의
 * 모든 명령이 타임아웃으로 실패하는데, 컨트롤러 자체는 멀쩡히
 * 응답하므로 원인이 배선이라는 것을 알아채기가 아주 어렵다. */
static void route_sd_to_emmc(void)
{
    for (u32 pin = 48; pin <= 53; pin++) {
        gpio_set_function(pin, GPIO_FUNC_ALT3);
        /* CLK(48) 말고는 전부 풀업. 카드가 빠져 있을 때 라인이
         * 떠다니면 없는 응답을 읽을 수 있다. */
        gpio_set_pull(pin, pin == 48 ? GPIO_PULL_NONE : GPIO_PULL_UP);
    }
}

static bool reset_host(void)
{
    mmio_write32(EMMC_CONTROL0, 0);
    mmio_set_bits(EMMC_CONTROL1, C1_SRST_HC);

    u64 deadline = timer_get_us() + 200000;
    while (mmio_read32(EMMC_CONTROL1) & (C1_SRST_HC | C1_SRST_CMD | C1_SRST_DATA))
        if (timer_get_us() > deadline) {
            kprintf("[emmc] 컨트롤러 리셋이 끝나지 않는다\n");
            return false;
        }
    return true;
}

bool emmc_init(void)
{
    memset(&card, 0, sizeof card);

    route_sd_to_emmc();
    delay_ms(2);

    if (!reset_host())
        return false;

    u32 ver = (mmio_read32(EMMC_SLOTISR_VER) >> 16) & 0xFF;
    kprintf("[emmc] SDHCI %u.0 호스트\n", ver + 1);

    /* 식별 구간은 400kHz 이하 - 규격이 정한 값이다. */
    if (!set_clock(400000)) {
        kprintf("[emmc] 400kHz 클럭을 세우지 못했다\n");
        return false;
    }

    /* 인터럽트로 알리지는 않고(IRPT_EN=0) 상태 비트만 올린다(MASK).
     * 부팅 로더는 어차피 기다리는 것 말고 할 일이 없으므로 폴링이
     * 단순하고, 인터럽트 경로를 하나 덜 의심해도 된다. */
    mmio_write32(EMMC_IRPT_EN, 0);
    mmio_write32(EMMC_INTERRUPT, 0xFFFFFFFFu);
    mmio_write32(EMMC_IRPT_MASK, 0xFFFFFFFFu);
    delay_ms(2);

    if (!send_cmd(CMD_GO_IDLE, 0, 1000)) {
        kprintf("[emmc] 카드가 CMD0 에 응답하지 않는다 - 카드가 없는가?\n");
        return false;
    }

    /* CMD8. 응답이 오면 2.0 이후 카드다. 응답이 없는 것도 정상적인
     * 답이다 - 1.x 카드라는 뜻이므로 그때는 SDHC 를 요구하지 않는다. */
    bool v2 = send_cmd(CMD_SEND_IF_COND, 0x000001AA, 200);
    if (v2 && (mmio_read32(EMMC_RESP0) & 0xFFF) != 0x1AA) {
        kprintf("[emmc] CMD8 응답이 이상하다 (%08x)\n",
                mmio_read32(EMMC_RESP0));
        return false;
    }

    /* ACMD41 을 준비될 때까지 반복. 카드가 내부 초기화를 하는 동안
     * busy 를 반환하는데, 규격이 최대 1초를 허용한다. */
    u64 deadline = timer_get_us() + 2000000;
    u32 resp = 0;
    for (;;) {
        u32 arg = 0x00FF8000u                   /* 3.2~3.4V 지원 */
                | (v2 ? (1u << 30) : 0);        /* HCS: SDHC 도 받는다 */
        if (!send_acmd(ACMD_SEND_OP_COND, arg, 200)) {
            kprintf("[emmc] ACMD41 이 실패했다\n");
            return false;
        }
        resp = mmio_read32(EMMC_RESP0);
        if (resp & (1u << 31))                  /* 초기화 끝 */
            break;
        if (timer_get_us() > deadline) {
            kprintf("[emmc] 카드가 준비되지 않는다 (OCR=%08x)\n", resp);
            return false;
        }
        delay_ms(10);
    }
    card.sdhc = (resp & (1u << 30)) != 0;

    if (!send_cmd(CMD_ALL_SEND_CID, 0, 200)) {
        kprintf("[emmc] CID 를 읽지 못했다\n");
        return false;
    }
    card.cid[0] = mmio_read32(EMMC_RESP0);
    card.cid[1] = mmio_read32(EMMC_RESP1);
    card.cid[2] = mmio_read32(EMMC_RESP2);
    card.cid[3] = mmio_read32(EMMC_RESP3);

    if (!send_cmd(CMD_SEND_REL_ADDR, 0, 200)) {
        kprintf("[emmc] 카드가 주소를 주지 않는다\n");
        return false;
    }
    card.rca = (mmio_read32(EMMC_RESP0) >> 16) & 0xFFFF;

    /* CSD 로 용량을 계산한다. 카드가 몇 바이트인지 모르면 파티션
     * 테이블이 가리키는 곳이 카드 밖인지 판단할 수 없다. */
    if (send_cmd(CMD_SEND_CSD, card.rca << 16, 200)) {
        /* ── R2 응답이 8비트 밀려 있다 ──────────────────────────
         *
         * 136비트 응답에서 컨트롤러가 CRC 와 시작/전송 비트를 떼어내고
         * 넣어주기 때문에, RESP0~3 에 들어 있는 것은 CSD[127:8] 이다.
         * 즉 전체가 오른쪽으로 8비트 밀려 있다.
         *
         * 이걸 모르고 RESP 를 그대로 CSD 로 쓰면 모든 필드가 8비트씩
         * 어긋난다. 처음에 그렇게 했더니 256MB 카드가 30784MB 로
         * 보였다 - 읽기는 멀쩡히 되니까 한참 뒤에야 알아챘다.
         *
         * 그래서 먼저 128비트를 제자리로 돌려놓고, 비트 번호로 필드를
         * 꺼낸다. 데이터시트에 적힌 비트 번호를 그대로 쓸 수 있어야
         * 나중에 검산할 수 있다. */
        u32 r0 = mmio_read32(EMMC_RESP0), r1 = mmio_read32(EMMC_RESP1);
        u32 r2 = mmio_read32(EMMC_RESP2), r3 = mmio_read32(EMMC_RESP3);

        u32 csd[4];
        csd[0] = r0 << 8;
        csd[1] = (r1 << 8) | (r0 >> 24);
        csd[2] = (r2 << 8) | (r1 >> 24);
        csd[3] = (r3 << 8) | (r2 >> 24);

        u32 ver = csd_bits(csd, 126, 2);

        if (ver == 1) {
            /* CSD v2 (SDHC/SDXC): C_SIZE 하나로 끝난다.
             * 용량 = (C_SIZE + 1) * 512KB. */
            u32 c_size = csd_bits(csd, 48, 22);
            card.blocks = ((u64)c_size + 1) * 1024;
        } else {
            /* CSD v1 (2GB 이하): 지수 셋을 곱해야 나온다. */
            u32 read_bl_len = csd_bits(csd, 80, 4);
            u32 c_size      = csd_bits(csd, 62, 12);
            u32 c_size_mult = csd_bits(csd, 47, 3);
            card.blocks = ((u64)c_size + 1)
                        * (1ULL << (c_size_mult + 2))
                        * (1ULL << read_bl_len) / SD_BLOCK_SIZE;
        }
    }

    if (!send_cmd(CMD_SELECT_CARD, card.rca << 16, 200)) {
        kprintf("[emmc] 카드를 선택하지 못했다\n");
        return false;
    }

    /* SDHC 는 블록 길이가 512 로 고정이지만, 구형 카드에는 말해줘야
     * 한다. SDHC 에 보내도 무시되므로 조건 없이 보낸다. */
    send_cmd(CMD_SET_BLOCKLEN, SD_BLOCK_SIZE, 200);

    /* 이제 속도를 올린다. 25MHz 는 어떤 SD 카드든 지원하는 값이다. */
    if (!set_clock(25000000)) {
        kprintf("[emmc] 25MHz 로 올리지 못했다 - 400kHz 로 계속한다\n");
    }

    card.ready = true;
    return true;
}

/* ── 읽기 ────────────────────────────────────────────────────────*/

bool emmc_read(u64 lba, u32 count, void *buf)
{
    if (!card.ready || count == 0)
        return false;

    if (card.blocks && (lba + count) > card.blocks) {
        kprintf("[emmc] 카드 밖을 읽으려 한다 (lba %llu, 카드 %llu 블록)\n",
                (unsigned long long)lba, (unsigned long long)card.blocks);
        return false;
    }

    /* SDHC 는 블록 번호로, 구형 카드는 바이트 오프셋으로 주소를 준다.
     * 이걸 틀리면 512배 엉뚱한 곳을 읽는데, 읽기는 성공하므로
     * "데이터가 이상하다" 로만 보인다. */
    u32 addr = card.sdhc ? (u32)lba : (u32)(lba * SD_BLOCK_SIZE);

    if (!wait_status(SR_DAT_INHIBIT, false, 1000))
        return false;

    mmio_write32(EMMC_BLKSIZECNT, (count << 16) | SD_BLOCK_SIZE);

    u32 cmd = (count == 1) ? CMD_READ_SINGLE : CMD_READ_MULTI;
    if (!send_cmd(cmd, addr, 1000))
        return false;

    u32 *out = (u32 *)buf;
    for (u32 b = 0; b < count; b++) {
        u32 err = 0;
        if (!wait_interrupt(INT_READ_RDY, 1000, &err)) {
            kprintf("[emmc] 블록 %u 에서 데이터가 오지 않는다 (%08x)\n",
                    b, err);
            return false;
        }
        /* FIFO 를 32비트씩 128번. DMA 를 쓰지 않는 이유는 부팅
         * 로더가 읽는 양이 수 메가바이트뿐이고, DMA 를 붙이면
         * 캐시 일관성까지 따라오기 때문이다. */
        for (u32 w = 0; w < SD_BLOCK_SIZE / 4; w++)
            *out++ = mmio_read32(EMMC_DATA);
    }

    if (count > 1) {
        u32 err = 0;
        if (!wait_interrupt(INT_DATA_DONE, 1000, &err)) {
            kprintf("[emmc] 다중 블록 전송이 끝나지 않았다 (%08x)\n", err);
            return false;
        }
    }
    return true;
}

u64 emmc_capacity(void)
{
    return card.blocks * SD_BLOCK_SIZE;
}

void emmc_describe(void)
{
    if (!card.ready) {
        kprintf("  카드 없음\n");
        return;
    }

    u64 mb = emmc_capacity() / (1024 * 1024);
    kprintf("  %s, %llu MB (블록 %llu개), RCA %04x\n",
            card.sdhc ? "SDHC/SDXC" : "SDSC",
            (unsigned long long)mb,
            (unsigned long long)card.blocks, card.rca);
}
