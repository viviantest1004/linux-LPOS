/* board.h - 보드 리비전 코드 해석과 보드 제어. */
#ifndef _BOARD_H
#define _BOARD_H

#include "types.h"

typedef struct {
    u32         raw;
    bool        new_style;      /* 신형 리비전 코드 포맷인가 */
    u32         revision;       /* 보드 리비전 (1.0, 1.1 ...) */
    u32         model_id;
    u32         processor_id;
    u32         manufacturer_id;
    u32         memory_mb;
    const char *model_name;
    const char *processor_name;
    const char *manufacturer_name;
} board_info_t;

void board_decode_revision(u32 rev, board_info_t *out);
void board_reset(void) __attribute__((noreturn));   /* 워치독으로 재부팅 */

#endif /* _BOARD_H */
