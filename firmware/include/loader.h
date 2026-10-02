/* loader.h - SD 카드에서 리눅스를 읽어 넘긴다.
 *
 * 여기까지 오면 부팅 사슬이 완성된다.
 *
 *   부트ROM -> bootcode.bin -> start.elf -> 우리 펌웨어 -> 리눅스
 *                              (Broadcom)   (여기)
 *
 * start.elf 도 커널을 직접 올릴 수 있으므로, 이 단계는 없던 기능을
 * 만드는 것이 아니라 사슬의 한 칸을 우리 것으로 바꾸는 일이다.
 * 로드맵이 이것을 유저랜드 뒤로 미뤄둔 이유도 그래서다 - 부트로더와
 * 유저랜드를 동시에 의심해야 하는 상황을 만들지 않으려고.
 */
#ifndef _LOADER_H
#define _LOADER_H

#include "types.h"

/* SD 카드를 열고, 커널과 디바이스 트리를 올리고, 리눅스로 뛴다.
 * 성공하면 돌아오지 않는다. 돌아왔다면 실패했고, 이유는 시리얼에
 * 남아 있다. */
void boot_linux(void);

/* 무엇이 있는지만 보여준다. 실제로 부팅하지는 않는다. */
void boot_survey(void);

#endif /* _LOADER_H */
