/* board.c - 라즈베리파이 리비전 코드 해석.
 *
 * 신형(2016년 이후) 리비전 코드 비트 배치:
 *   [31:25] 예약     [24] 보증무효    [23] 신형 포맷 플래그(=1)
 *   [22:20] 메모리   [19:16] 제조사   [15:12] 프로세서
 *   [11:4]  모델     [3:0] 보드 리비전
 *
 * Pi Zero 2 W 는 모델 0x12 / 프로세서 BCM2837 / 메모리 512MB 로 나온다. */
#include "board.h"
#include "mmio.h"
#include "bcm2710.h"

static const char *model_name(u32 id)
{
    switch (id) {
    case 0x00: return "Model A";
    case 0x01: return "Model B";
    case 0x02: return "Model A+";
    case 0x03: return "Model B+";
    case 0x04: return "2 Model B";
    case 0x06: return "Compute Module 1";
    case 0x08: return "3 Model B";
    case 0x09: return "Zero";
    case 0x0A: return "Compute Module 3";
    case 0x0C: return "Zero W";
    case 0x0D: return "3 Model B+";
    case 0x0E: return "3 Model A+";
    case 0x10: return "Compute Module 3+";
    case 0x11: return "4 Model B";
    case 0x12: return "Zero 2 W";          /* <-- 우리 보드 */
    case 0x13: return "400";
    case 0x14: return "Compute Module 4";
    case 0x17: return "5 Model B";
    default:   return "Unknown";
    }
}

static const char *processor_name(u32 id)
{
    switch (id) {
    case 0: return "BCM2835";
    case 1: return "BCM2836";
    case 2: return "BCM2837";      /* Zero 2 W = BCM2710A1, 2837 계열 */
    case 3: return "BCM2711";
    case 4: return "BCM2712";
    default: return "Unknown";
    }
}

static const char *manufacturer_name(u32 id)
{
    switch (id) {
    case 0: return "Sony UK";
    case 1: return "Egoman";
    case 2: return "Embest";
    case 3: return "Sony Japan";
    case 4: return "Embest";
    case 5: return "Stadium";
    default: return "Unknown";
    }
}

void board_decode_revision(u32 rev, board_info_t *out)
{
    out->raw = rev;
    out->new_style = (rev & (1u << 23)) != 0;

    if (!out->new_style) {
        /* 구형 코드는 단순 룩업 테이블이라 여기선 다루지 않는다.
         * Zero 2 W 는 항상 신형이므로 실제로 걸릴 일이 없다. */
        out->revision          = 0;
        out->model_id          = 0;
        out->processor_id      = 0;
        out->manufacturer_id   = 0;
        out->memory_mb         = 0;
        out->model_name        = "Legacy revision code";
        out->processor_name    = "Unknown";
        out->manufacturer_name = "Unknown";
        return;
    }

    out->revision        = rev & 0x0F;
    out->model_id        = (rev >> 4)  & 0xFF;
    out->processor_id    = (rev >> 12) & 0x0F;
    out->manufacturer_id = (rev >> 16) & 0x0F;

    /* 메모리 필드는 2의 거듭제곱 인덱스: 0=256MB, 1=512MB, 2=1GB ... */
    u32 mem_idx = (rev >> 20) & 0x07;
    out->memory_mb = 256u << mem_idx;

    out->model_name        = model_name(out->model_id);
    out->processor_name    = processor_name(out->processor_id);
    out->manufacturer_name = manufacturer_name(out->manufacturer_id);
}

void board_reset(void)
{
    /* BCM2710 에는 전용 리셋 라인이 없다. 파워매니지먼트 블록의
     * 워치독을 최소 틱으로 걸고 전체 리셋을 요청하면 재부팅된다.
     * 모든 PM 레지스터 쓰기에는 상위 8비트에 패스워드가 필요하다. */
    mmio_write32(PM_WDOG, PM_PASSWORD | 1);

    u32 r = mmio_read32(PM_RSTC) & ~0x30u;
    mmio_write32(PM_RSTC, PM_PASSWORD | r | PM_RSTC_WRCFG_FULL);

    for (;;)
        __asm__ volatile("wfe");
}
