/* asm-offsets.h - vectors.S 가 exc_frame_t 안을 짚는 데 쓰는 오프셋.
 *
 * 어셈블러는 C 구조체를 모른다. 그래서 필드 위치를 숫자로 적어줘야
 * 하는데, 숫자와 구조체가 어긋나면 레지스터 덤프가 조용히 거짓말을
 * 한다 - 틀린 값을 보여주는 패닉 출력은 없느니만 못하다.
 *
 * 그래서 여기 적힌 숫자 하나하나를 exception.c 가 _Static_assert 로
 * 대조한다. 구조체를 고치고 이 파일을 안 고치면 빌드가 선다.
 */
#ifndef _ASM_OFFSETS_H
#define _ASM_OFFSETS_H

/* x0..x30 이 0..247, 그 뒤로 이어진다. */
#define FRAME_SP        248
#define FRAME_ELR       256
#define FRAME_SPSR      264
#define FRAME_ESR       272
#define FRAME_FAR       280
#define FRAME_KIND      288

/* 실제 구조체는 296바이트지만 SP 는 16바이트 정렬이어야 한다.
 * AArch64 는 정렬 안 된 SP 로 스택에 접근하면 그 자리에서 또 예외를
 * 내는데, 예외 핸들러 안에서 나는 예외만큼 다루기 어려운 것이 없다. */
#define FRAME_SIZE      304

#endif /* _ASM_OFFSETS_H */
