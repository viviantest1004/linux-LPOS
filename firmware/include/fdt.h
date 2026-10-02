/* fdt.h - 디바이스 트리(플랫 형식) 최소 편집기.
 *
 * ── 왜 DTB 를 건드려야 하나 ──────────────────────────────────────
 * 리눅스는 커맨드라인을 레지스터로 받지 않는다. 디바이스 트리 안의
 * /chosen/bootargs 라는 속성에서 읽는다. 그러니까 cmdline.txt 를
 * 커널에 전달한다는 것은 DTB 를 고쳐서 넘긴다는 뜻이다.
 *
 * ── 편집이 아니라 재작성인 이유 ──────────────────────────────────
 * DTB 는 길이가 앞에 붙은 블록들이 촘촘히 이어 붙은 형식이라, 속성
 * 하나를 늘리면 그 뒤의 모든 것이 밀린다. 제자리에서 밀어내는 코드는
 * 짧게 쓸 수 있지만 경계 조건에서 조용히 틀리기 쉽다 - 그리고 여기서
 * 조용히 틀리면 커널이 부팅 도중 아무 말 없이 멈춘다.
 *
 * 그래서 새 버퍼에 처음부터 다시 쓴다. 원본을 순서대로 읽어 옮기다가
 * /chosen 을 만나면 bootargs 를 갈아끼우고, /chosen 이 아예 없으면
 * 만들어 넣는다. 길이 계산이 한 방향으로만 흘러서 검산하기 쉽다.
 */
#ifndef _FDT_H
#define _FDT_H

#include "types.h"

#define FDT_MAGIC   0xD00DFEEDu

/* 이 주소에 쓸 만한 DTB 가 있는가. */
bool fdt_valid(const void *fdt);

/* DTB 전체 크기(바이트). 유효하지 않으면 0. */
u32  fdt_size(const void *fdt);

/* 넘기기 전에 고쳐야 하는 것들.
 *
 * 라즈베리파이가 배포하는 .dtb 는 완성품이 아니다. 몇 자리를 비워두고
 * start.elf 가 채우는 것을 전제로 만들어져 있다 - 우리가 그 자리를
 * 대신하는 순간, 비워둔 자리를 채우는 일도 우리 몫이 된다. */
typedef struct {
    const char *bootargs;   /* NULL 이면 원래 것을 둔다 */
    u64 mem_base;
    u64 mem_size;           /* 0 이면 /memory 를 건드리지 않는다 */
} fdt_fixup_t;

/* src 의 DTB 를 dst 로 옮기면서 fix 가 말하는 자리를 고친다.
 * dst 는 capacity 바이트. 성공하면 새 크기를 out_size 에 넣는다. */
bool fdt_copy_with_fixups(const void *src, void *dst, u32 capacity,
                          const fdt_fixup_t *fix, u32 *out_size);

/* 지금 들어 있는 커맨드라인을 보여준다 (없으면 그렇게 말한다). */
void fdt_describe(const void *fdt);

#endif /* _FDT_H */
