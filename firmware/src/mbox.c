/* mbox.c - VideoCore 메일박스 프로퍼티 인터페이스.
 *
 * 메시지 형식 (전부 32비트 워드, 리틀엔디언):
 *   [0] 전체 크기 (바이트)
 *   [1] 요청/응답 코드 (요청=0, 응답=0x80000000 성공 / 0x80000001 실패)
 *   [2] 태그 ID
 *   [3] 값 버퍼 크기 (바이트)  <- 요청/응답 중 큰 쪽
 *   [4] 요청 시: 요청 데이터 길이 / 응답 시: 0x80000000|응답길이
 *   [5..] 값 워드들
 *   [n] 0x00000000  (끝 태그)
 */
#include "mbox.h"
#include "mmio.h"
#include "bcm2710.h"

#define MBOX_REQUEST        0x00000000u
#define MBOX_RESP_SUCCESS   0x80000000u
#define MBOX_TAG_LAST       0x00000000u

/* 헤더 5워드 + 값 + 끝태그 1워드. 프레임버퍼 태그까지 감당할 크기. */
#define MBOX_BUF_WORDS      36

/* GPU 가 DMA 로 직접 읽으므로 16바이트 정렬 필수
 * (하위 4비트는 채널 번호로 쓰인다). */
static volatile u32 mbox_buf[MBOX_BUF_WORDS] __attribute__((aligned(16)));

/* ARM 물리주소 -> VideoCore 버스주소.
 * BCM2710 에서 SDRAM 은 GPU 쪽에 여러 별칭으로 보인다:
 *   0xC0000000  L2 캐시 우회 (uncached)
 *   0x40000000  L2 캐시 사용, 코히런트
 * 지금은 MMU/캐시를 아직 안 켰으므로 캐시 우회 별칭이 안전하다.
 * Phase 2 에서 MMU 를 켜면 0x40000000 으로 바꾼다. */
#define BUS_ADDR(p)   ((((u32)(uptr)(p)) & ~0xC0000000u) | 0xC0000000u)

static bool mbox_call(u32 channel)
{
    u32 msg = (BUS_ADDR(mbox_buf) & ~0xFu) | (channel & 0xFu);

    /* GPU 가 우리 버퍼를 읽기 전에 쓰기가 메모리에 도달해야 한다 */
    dsb();

    /* 보낼 자리가 날 때까지 */
    while (mmio_read32(MBOX_STATUS) & MBOX_FULL)
        ;
    mmio_write32(MBOX_WRITE, msg);

    /* 우리가 보낸 것과 같은 메시지가 돌아올 때까지.
     * 다른 채널 응답이 섞여 올 수 있으므로 반드시 비교해야 한다. */
    for (;;) {
        while (mmio_read32(MBOX_STATUS) & MBOX_EMPTY)
            ;
        if (mmio_read32(MBOX_READ) == msg) {
            dsb();
            return mbox_buf[1] == MBOX_RESP_SUCCESS;
        }
    }
}

bool mbox_prop(u32 tag, const u32 *req, u32 req_words,
               u32 *resp, u32 resp_words)
{
    /* 값 버퍼는 요청/응답 중 큰 쪽에 맞춘다 */
    u32 val_words = (req_words > resp_words) ? req_words : resp_words;

    if (val_words + 6 > MBOX_BUF_WORDS)
        return false;

    u32 i = 0;
    mbox_buf[i++] = (val_words + 6) * 4;   /* 전체 크기 */
    mbox_buf[i++] = MBOX_REQUEST;
    mbox_buf[i++] = tag;
    mbox_buf[i++] = val_words * 4;         /* 값 버퍼 크기 */
    mbox_buf[i++] = req_words * 4;         /* 요청 데이터 길이 */

    for (u32 j = 0; j < val_words; j++)
        mbox_buf[i++] = (j < req_words) ? req[j] : 0;

    mbox_buf[i++] = MBOX_TAG_LAST;

    if (!mbox_call(MBOX_CH_PROP))
        return false;

    /* 태그별 응답 코드: 최상위 비트가 서야 처리된 것 */
    if (!(mbox_buf[4] & 0x80000000u))
        return false;

    for (u32 j = 0; j < resp_words; j++)
        resp[j] = mbox_buf[5 + j];

    return true;
}

bool mbox_send(u32 *msg, u32 words)
{
    if (words > MBOX_BUF_WORDS)
        return false;

    for (u32 i = 0; i < words; i++)
        mbox_buf[i] = msg[i];

    if (!mbox_call(MBOX_CH_PROP))
        return false;

    for (u32 i = 0; i < words; i++)
        msg[i] = mbox_buf[i];

    return true;
}

/* ── 헬퍼들 ───────────────────────────────────────────────────── */

u32 mbox_get_clock_rate(u32 clock_id)
{
    u32 req[1] = { clock_id };
    u32 resp[2] = { 0, 0 };     /* [0]=클럭ID, [1]=Hz */

    if (!mbox_prop(TAG_GET_CLOCK_RATE, req, 1, resp, 2))
        return 0;
    return resp[1];
}

u32 mbox_get_max_clock_rate(u32 clock_id)
{
    u32 req[1] = { clock_id };
    u32 resp[2] = { 0, 0 };

    if (!mbox_prop(TAG_GET_MAX_CLOCK_RATE, req, 1, resp, 2))
        return 0;
    return resp[1];
}

u32 mbox_get_board_revision(void)
{
    u32 resp[1] = { 0 };
    if (!mbox_prop(TAG_GET_BOARD_REV, NULL, 0, resp, 1))
        return 0;
    return resp[0];
}

u32 mbox_get_firmware_revision(void)
{
    u32 resp[1] = { 0 };
    if (!mbox_prop(TAG_GET_FIRMWARE_REV, NULL, 0, resp, 1))
        return 0;
    return resp[0];
}

u64 mbox_get_board_serial(void)
{
    u32 resp[2] = { 0, 0 };     /* 64비트를 두 워드로 (하위 먼저) */
    if (!mbox_prop(TAG_GET_BOARD_SERIAL, NULL, 0, resp, 2))
        return 0;
    return ((u64)resp[1] << 32) | resp[0];
}

bool mbox_get_arm_memory(u32 *base, u32 *size)
{
    u32 resp[2] = { 0, 0 };
    if (!mbox_prop(TAG_GET_ARM_MEMORY, NULL, 0, resp, 2))
        return false;
    if (base) *base = resp[0];
    if (size) *size = resp[1];
    return true;
}

bool mbox_get_vc_memory(u32 *base, u32 *size)
{
    u32 resp[2] = { 0, 0 };
    if (!mbox_prop(TAG_GET_VC_MEMORY, NULL, 0, resp, 2))
        return false;
    if (base) *base = resp[0];
    if (size) *size = resp[1];
    return true;
}

s32 mbox_get_temperature_mc(void)
{
    u32 req[1]  = { 0 };        /* 온도 센서 ID 0 */
    u32 resp[2] = { 0, 0 };     /* [0]=센서ID, [1]=밀리섭씨 */

    if (!mbox_prop(TAG_GET_TEMPERATURE, req, 1, resp, 2))
        return -1;
    return (s32)resp[1];
}
