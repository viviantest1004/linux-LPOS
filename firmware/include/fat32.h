/* fat32.h - MBR 파티션 테이블과 FAT32 읽기.
 *
 * 부트로더가 필요한 것은 딱 하나다. 이름을 주면 파일 내용을 메모리에
 * 올려주는 것. 쓰기도, 디렉터리 생성도, 파일 삭제도 필요 없다 -
 * 부팅에 쓰지 않는 코드는 부팅을 망칠 기회만 늘린다.
 *
 * 긴 이름(LFN)은 읽는다. 라즈베리파이 부트 파티션의 디바이스트리
 * 파일 이름이 bcm2710-rpi-zero-2-w.dtb 인데, 8.3 이름만 읽으면
 * BCM271~1.DTB 같은 것으로 보이고 그건 카드를 포맷한 도구에 따라
 * 달라진다. 이름으로 찾을 수 없는 파일은 없는 파일이나 같다.
 */
#ifndef _FAT32_H
#define _FAT32_H

#include "types.h"

typedef struct {
    u32 first_cluster;
    u32 size;               /* 바이트 */
} fat_file_t;

/* 카드에서 첫 번째 FAT 파티션을 찾아 마운트한다.
 * 파티션 테이블이 없으면 카드 전체를 FAT 로 보고 한 번 더 시도한다 -
 * 통째로 포맷된 카드가 실제로 있다. */
bool fat32_mount(void);

/* 루트 디렉터리에서 이름으로 찾는다. 대소문자 구분 없음. */
bool fat32_find(const char *name, fat_file_t *out);

/* 파일 전체를 dst 로 읽는다. max 바이트를 넘으면 실패.
 * 실제로 읽은 바이트 수를 read_out 에 넣는다. */
bool fat32_read_file(const fat_file_t *f, void *dst, u32 max, u32 *read_out);

/* 루트 디렉터리에 무엇이 있는지 보여준다. 파일 이름을 틀렸을 때
 * "없다" 한 줄만 보는 것과 목록을 보는 것은 아주 다르다. */
void fat32_list(void);

/* 마운트된 파티션의 요약. */
void fat32_describe(void);

#endif /* _FAT32_H */
