/* emmc.h - SD 카드를 블록 단위로 읽는다.
 *
 * ── 이 칩에 SD 컨트롤러가 둘인 이유 ──────────────────────────────
 * BCM2710 에는 SD 카드를 다룰 수 있는 물건이 두 개 있다.
 *
 *   SDHOST (0x3F202000)  브로드컴이 직접 만든 것. 문서가 거의 없다.
 *   EMMC   (0x3F300000)  Arasan 의 SDHCI. 업계 표준 규격이 있다.
 *
 * Pi 3 계열(Zero 2 W 포함)은 부팅 후 SD 카드가 SDHOST 에 붙어 있고,
 * EMMC 는 WiFi 칩의 SDIO 를 담당한다. 우리는 SD 카드를 EMMC 쪽으로
 * 옮겨서 쓴다 - GPIO 48~53 을 ALT3 으로 돌리면 배선이 그쪽으로 간다.
 *
 * 표준 규격이 있는 컨트롤러를 고르는 쪽이, 문서 없는 컨트롤러를
 * 추측으로 다루는 것보다 낫다. QEMU 도 EMMC 쪽만 모델링한다.
 */
#ifndef _EMMC_H
#define _EMMC_H

#include "types.h"

#define SD_BLOCK_SIZE   512

/* 카드를 찾아 초기화한다. 성공하면 true.
 * 실패 이유는 시리얼로 나간다 - 여기서 실패하면 부팅이 끝나므로,
 * 어느 단계에서 멈췄는지가 유일한 단서다. */
bool emmc_init(void);

/* 블록 단위 읽기. lba 는 512바이트 블록 번호. */
bool emmc_read(u64 lba, u32 count, void *buf);

/* 카드 용량(바이트). 초기화 전에는 0. */
u64  emmc_capacity(void);

/* 사람이 읽을 카드 요약 - 종류와 용량. */
void emmc_describe(void);

#endif /* _EMMC_H */
