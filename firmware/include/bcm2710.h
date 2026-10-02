/* bcm2710.h - BCM2710A1 (RP3A0) 하드웨어 주소 맵.
 *
 * 라즈베리파이 제로 2 W는 BCM2710A1 SoC를 쓴다. Pi 3와 같은 계열이라
 * 페리페럴 베이스가 0x3F000000 이다.
 *   Pi 1 / Zero  (BCM2835) : 0x20000000
 *   Pi 2 / 3 / Zero 2 W    : 0x3F000000   <-- 우리 보드
 *   Pi 4         (BCM2711) : 0xFE000000
 *
 * 데이터시트에는 VideoCore 버스 주소(0x7Exxxxxx)로 적혀 있다.
 * ARM 물리 주소로 바꾸려면 0x7E000000 -> 0x3F000000 으로 치환하면 된다.
 */
#ifndef _BCM2710_H
#define _BCM2710_H

#define PERIPHERAL_BASE     0x3F000000UL
#define ARM_LOCAL_BASE      0x40000000UL   /* 코어별 타이머/메일박스 */

/* --- 주요 블록 --- */
#define SYSTIMER_BASE       (PERIPHERAL_BASE + 0x00003000)
#define MBOX_BASE           (PERIPHERAL_BASE + 0x0000B880)
#define IRQ_BASE            (PERIPHERAL_BASE + 0x0000B200)
#define PM_BASE             (PERIPHERAL_BASE + 0x00100000)  /* power mgmt / watchdog */
#define GPIO_BASE           (PERIPHERAL_BASE + 0x00200000)
#define UART0_BASE          (PERIPHERAL_BASE + 0x00201000)  /* PL011 */
#define AUX_BASE            (PERIPHERAL_BASE + 0x00215000)  /* mini UART / SPI1,2 */
#define EMMC_BASE           (PERIPHERAL_BASE + 0x00300000)  /* SD 카드 */

/* --- GPIO (오프셋은 GPIO_BASE 기준) --- */
#define GPFSEL0             (GPIO_BASE + 0x00)   /* +0x04 씩 GPFSEL5 까지 */
#define GPSET0              (GPIO_BASE + 0x1C)
#define GPSET1              (GPIO_BASE + 0x20)
#define GPCLR0              (GPIO_BASE + 0x28)
#define GPCLR1              (GPIO_BASE + 0x2C)
#define GPLEV0              (GPIO_BASE + 0x34)
#define GPLEV1              (GPIO_BASE + 0x38)
/* BCM2710은 구형 풀업/풀다운 시퀀스(GPPUD + GPPUDCLK)를 쓴다.
 * BCM2711(Pi4)의 GPIO_PUP_PDN_CNTRL 레지스터가 아니다. */
#define GPPUD               (GPIO_BASE + 0x94)
#define GPPUDCLK0           (GPIO_BASE + 0x98)
#define GPPUDCLK1           (GPIO_BASE + 0x9C)

/* --- PL011 UART0 (오프셋은 UART0_BASE 기준) --- */
#define UART0_DR            (UART0_BASE + 0x00)
#define UART0_FR            (UART0_BASE + 0x18)
#define UART0_IBRD          (UART0_BASE + 0x24)
#define UART0_FBRD          (UART0_BASE + 0x28)
#define UART0_LCRH          (UART0_BASE + 0x2C)
#define UART0_CR            (UART0_BASE + 0x30)
#define UART0_IMSC          (UART0_BASE + 0x38)
#define UART0_ICR           (UART0_BASE + 0x44)

#define UART_FR_RXFE        (1u << 4)   /* 수신 FIFO 비어있음 */
#define UART_FR_TXFF        (1u << 5)   /* 송신 FIFO 가득참 */
#define UART_FR_BUSY        (1u << 3)

#define UART_LCRH_FEN       (1u << 4)   /* FIFO 사용 */
#define UART_LCRH_WLEN8     (3u << 5)   /* 8비트 워드 */

#define UART_CR_UARTEN      (1u << 0)
#define UART_CR_TXE         (1u << 8)
#define UART_CR_RXE         (1u << 9)

/* --- 시스템 타이머 (1MHz 고정, 64비트 프리러닝) --- */
#define SYSTIMER_CS         (SYSTIMER_BASE + 0x00)
#define SYSTIMER_CLO        (SYSTIMER_BASE + 0x04)
#define SYSTIMER_CHI        (SYSTIMER_BASE + 0x08)

/* --- 메일박스 0 (VideoCore <-> ARM) --- */
#define MBOX_READ           (MBOX_BASE + 0x00)
#define MBOX_STATUS         (MBOX_BASE + 0x18)
#define MBOX_WRITE          (MBOX_BASE + 0x20)

#define MBOX_FULL           0x80000000u
#define MBOX_EMPTY          0x40000000u

/* --- 레거시 인터럽트 컨트롤러 (0x3F00B200) ---
 *
 * GPU 와 ARM 이 공유하는 구형 컨트롤러다. GIC 가 아니라서 우선순위도
 * 없고 인터럽트마다 CPU 를 고를 수도 없다. 대기 중인 것을 비트맵에서
 * 직접 읽어 처리한다. */
#define IRQ_BASIC_PENDING   (IRQ_BASE + 0x00)
#define IRQ_PENDING_1       (IRQ_BASE + 0x04)
#define IRQ_PENDING_2       (IRQ_BASE + 0x08)
#define FIQ_CONTROL         (IRQ_BASE + 0x0C)
#define ENABLE_IRQS_1       (IRQ_BASE + 0x10)
#define ENABLE_IRQS_2       (IRQ_BASE + 0x14)
#define ENABLE_BASIC_IRQS   (IRQ_BASE + 0x18)
#define DISABLE_IRQS_1      (IRQ_BASE + 0x1C)
#define DISABLE_IRQS_2      (IRQ_BASE + 0x20)
#define DISABLE_BASIC_IRQS  (IRQ_BASE + 0x24)

/* 자주 쓰는 GPU IRQ 번호 (IRQ_PENDING_1/2 비트 위치) */
#define IRQ_SYSTIMER_1      1
#define IRQ_SYSTIMER_3      3
#define IRQ_AUX             29
#define IRQ_EMMC            62
#define IRQ_UART0           57

/* --- 코어별 로컬 페리페럴 (0x40000000) ---
 *
 * Pi 2 부터 생긴 블록이다. ARM generic timer 의 인터럽트가 여기를
 * 거쳐야 코어에 도달한다 - 레거시 컨트롤러에는 그 선이 없다. */
#define LOCAL_CONTROL       (ARM_LOCAL_BASE + 0x000)
#define LOCAL_PRESCALER     (ARM_LOCAL_BASE + 0x008)
#define LOCAL_GPU_ROUTING   (ARM_LOCAL_BASE + 0x00C)
#define CORE_TIMER_IRQCNTL(c) (ARM_LOCAL_BASE + 0x040 + 4 * (c))
#define CORE_MBOX_IRQCNTL(c)  (ARM_LOCAL_BASE + 0x050 + 4 * (c))
#define CORE_IRQ_SOURCE(c)    (ARM_LOCAL_BASE + 0x060 + 4 * (c))
#define CORE_FIQ_SOURCE(c)    (ARM_LOCAL_BASE + 0x070 + 4 * (c))

/* CORE_TIMER_IRQCNTL / CORE_IRQ_SOURCE 의 비트.
 * EL1 에서 쓰는 물리 타이머는 non-secure 쪽(비트 1)이다. */
#define CORE_IRQ_CNTPS      (1u << 0)
#define CORE_IRQ_CNTPNS     (1u << 1)
#define CORE_IRQ_CNTHP      (1u << 2)
#define CORE_IRQ_CNTV       (1u << 3)
#define CORE_IRQ_MBOX0      (1u << 4)
#define CORE_IRQ_GPU        (1u << 8)

/* --- 워치독 / 리셋 --- */
#define PM_RSTC             (PM_BASE + 0x1C)
#define PM_WDOG             (PM_BASE + 0x24)
#define PM_PASSWORD         0x5A000000u
#define PM_RSTC_WRCFG_FULL  0x00000020u

/* --- 보드 고유 --- */
/* Pi Zero 2 W의 ACT LED. 업스트림 디바이스트리
 * (bcm2710-rpi-zero-2-w.dts)에서 GPIO 29 / active-low 로 정의되어 있다.
 * 극성이 불확실해도 "토글"은 어느 쪽이든 깜빡이므로 안전하다. */
#define BOARD_ACT_LED_PIN   29

/* 커널 로드 주소. config.txt 에 arm_64bit=1 이면 start.elf 가
 * kernel8.img 를 여기에 올리고 점프한다. */
#define KERNEL_LOAD_ADDR    0x80000UL

#endif /* _BCM2710_H */
