/* mmio.h - 메모리 맵 I/O 접근.
 *
 * 주의: BCM2710 페리페럴은 서로 다른 블록 사이에서 읽기/쓰기 순서가
 * 보장되지 않는다(데이터시트 1.3절 "Peripheral access precautions").
 * 그래서 접근마다 데이터 메모리 배리어를 넣는다. */
#ifndef _MMIO_H
#define _MMIO_H

#include "types.h"

/* Data Synchronization Barrier - 앞선 메모리 접근이 끝날 때까지 대기 */
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }
/* Data Memory Barrier - 앞뒤 메모리 접근의 순서만 보장 */
static inline void dmb(void) { __asm__ volatile("dmb sy" ::: "memory"); }
/* Instruction Synchronization Barrier - 파이프라인 플러시 */
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }

static inline void mmio_write32(uptr addr, u32 val)
{
    dmb();
    *(volatile u32 *)addr = val;
}

static inline u32 mmio_read32(uptr addr)
{
    u32 v = *(volatile u32 *)addr;
    dmb();
    return v;
}

/* read-modify-write: 비트 세트 / 클리어 */
static inline void mmio_set_bits(uptr addr, u32 mask)
{
    mmio_write32(addr, mmio_read32(addr) | mask);
}

static inline void mmio_clear_bits(uptr addr, u32 mask)
{
    mmio_write32(addr, mmio_read32(addr) & ~mask);
}

#endif /* _MMIO_H */
