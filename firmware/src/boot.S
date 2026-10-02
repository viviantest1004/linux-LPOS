/* boot.S - LP-zero OS 최초 진입점.
 *
 * ── 부팅 사슬 ─────────────────────────────────────────────────────
 *  1) BCM2710 실리콘 내부 부트ROM (GPU/VideoCore가 먼저 깨어난다)
 *  2) SD카드의 bootcode.bin          <- Broadcom 클로즈드 블롭
 *  3) SD카드의 start.elf + fixup.dat <- Broadcom 클로즈드 블롭
 *       · SDRAM 초기화, config.txt 파싱, ARM 코어 릴리즈
 *  4) kernel8.img 를 0x80000 에 적재하고 여기로 점프  <-- 우리 코드 시작
 *
 * 여기부터는 전부 우리가 쓴다.
 *
 * ── 진입 상태 ─────────────────────────────────────────────────────
 *  · config.txt 에 arm_64bit=1 이므로 AArch64
 *  · 기본 armstub 을 거쳐 보통 EL2 로 진입한다 (EL3/EL1 도 방어적으로 처리)
 *  · 코어 4개가 전부 같은 주소로 들어올 수 있다 -> 0번만 살리고 나머지 파킹
 *  · MMU/캐시 꺼짐, 스택 없음, BSS 미초기화
 */

.section ".text.boot"
.global _start

_start:
    /* ── 1. 나중에 필요한 두 가지를 먼저 챙긴다 ──────────────────
     *
     * x0 에는 펌웨어가 넘겨준 디바이스 트리 주소가, MPIDR 에는 내가 몇
     * 번 코어인지가 들어 있다. 둘 다 아래에서 x0 이 수십 번 덮이기 전에
     * 옮겨두어야 한다. x21/x22 를 고른 이유는 eret 이 범용 레지스터를
     * 건드리지 않아서, 예외 레벨을 두 단계 내려가는 동안 그대로 살아남기
     * 때문이다.
     *
     * 여기서 코어를 가르지 않는다. 예외 레벨 강하는 넷이 다 거쳐야 한다 -
     * 그 이유는 el1_entry 에 적어두었다. */
    mov     x21, x0                 /* 디바이스 트리 (코어 0 에만 의미 있다) */
    mrs     x22, mpidr_el1
    and     x22, x22, #0xFF         /* MPIDR_EL1[7:0] = Aff0 = 코어 번호 */
    b       check_el

/* ── 보조 코어: 스핀 테이블 ──────────────────────────────────────
 *
 * 처음에는 그냥 wfe 로 재웠다. 우리 펌웨어만 돌 때는 그것으로 충분한데,
 * 리눅스를 띄우는 순간 문제가 된다.
 *
 * start.elf 의 armstub 이 넘겨주는 디바이스 트리는 보조 코어의
 * enable-method 를 "spin-table" 이라고 적어두고, 코어마다 릴리즈
 * 주소를 하나씩 지정한다 (코어 N -> 0xd8 + N*8). 리눅스는 거기에
 * 진입 주소를 써넣고 SEV 를 보낸다. 잠들어 있기만 한 코어는 깨어나서
 * 아무것도 읽지 않고 도로 자므로, 리눅스는 코어 하나로만 돈다 -
 * 에러도 경고도 없이. 512MB 보드에서 코어 세 개를 조용히 잃는다.
 *
 * 그래서 규약대로 기다린다. 깨어나서 자기 주소를 읽고, 0 이 아니면
 * 그리로 뛴다. 진입할 때 x0~x3 을 0 으로 두는 것도 규약이다. */
secondary_park:
    mov     x1, #0xd8
    add     x1, x1, x22, lsl #3     /* 코어 N 의 릴리즈 주소 */

1:  wfe
    ldr     x2, [x1]
    cbz     x2, 1b

    mov     x0, #0
    mov     x1, #0
    mov     x3, #0
    br      x2

check_el:
    /* ── 2. 어느 예외 레벨로 들어왔나 ───────────────────────────
     *
     * ── EL1 로 내려가지 않는 이유 ─────────────────────────────
     * 처음에는 EL2 에서 EL1 로 내려갔다. 펌웨어 혼자 도는 동안은 어느
     * 쪽이든 상관없고, EL1 이 더 "보통의" 커널 자리라 그렇게 했다.
     *
     * 리눅스를 띄우기 시작하니 문제가 드러났다. 보조 코어 셋은 우리
     * 코드를 거치지 않는다 - start.elf 의 armstub(QEMU 에서는 QEMU 의
     * 스텁)이 이미 자기 스핀 테이블에 재워둔 상태로 우리에게 넘긴다.
     * 그 코어들은 EL2 에 있다. 우리만 EL1 로 내려가면, 리눅스가 코어를
     * 다 깨운 직후 이렇게 말한다:
     *
     *     CPU: CPUs started in inconsistent modes
     *     WARNING: ... smp_cpus_done
     *
     * 부팅은 되지만 KVM 이 꺼지고, 예외 레벨을 전제하는 판단이 코어마다
     * 달라진다. 한 번 내려간 예외 레벨은 다시 올라갈 수 없으므로,
     * 고치는 방법은 애초에 내려가지 않는 것뿐이다.
     *
     * 그래서 EL2 에 머문다. EL3 로 들어왔으면 EL2 까지만 내려간다.
     * UART, 메일박스, GPIO, EMMC 는 EL2 에서도 그대로 쓸 수 있다. */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #3
    b.eq    drop_from_el3
    cmp     x0, #2
    b.eq    el2_setup
    b       el1_setup               /* 이 SoC 에서는 오지 않는다 */

    /* ── 3a. EL3 -> EL2 강하 ────────────────────────────────────── */
drop_from_el3:
    mrs     x0, scr_el3
    orr     x0, x0, #(1 << 10)      /* RW  : 하위 EL 은 AArch64      */
    orr     x0, x0, #(1 << 0)       /* NS  : Non-secure              */
    msr     scr_el3, x0

    mov     x0, #0x3C9              /* DAIF 마스크 + M=0b01001(EL2h) */
    msr     spsr_el3, x0
    adr     x0, el2_setup
    msr     elr_el3, x0
    eret

    /* ── 3b. EL2 에서 계속 돈다 ─────────────────────────────────── */
el2_setup:
    /* HCR_EL2 두 가지를 세운다.
     *
     * RW(31): 하위 EL 은 AArch64. 우리는 하위 EL 로 내려가지 않지만
     * 리눅스가 EL1 을 쓸 때 이 비트가 서 있어야 한다.
     *
     * IMO(4)/FMO(3)/AMO(5): 물리 인터럽트를 EL2 로 보낸다. 이게 없으면
     * 인터럽트는 EL1 을 향하는데, 우리는 EL2 에서 돌고 있고 예외는
     * 자기보다 낮은 EL 로는 전달될 수 없다. 그래서 인터럽트가 컨트롤러에
     * 대기 상태로 걸린 채 영원히 오지 않는다.
     *
     * 증상이 지독하다. 타이머 레지스터는 전부 정상이고(ENABLE 켜짐,
     * ISTATUS 로 "울렸음" 표시까지), 로컬 컨트롤러의 pending 비트도
     * 서 있는데, 핸들러만 안 불린다. 어느 한 곳도 실패를 보고하지
     * 않으므로 전부 정상으로 보인다. */
    mrs     x0, hcr_el2
    orr     x0, x0, #(1 << 31)      /* RW  */
    orr     x0, x0, #(1 << 5)       /* AMO */
    orr     x0, x0, #(1 << 4)       /* IMO */
    orr     x0, x0, #(1 << 3)       /* FMO */
    msr     hcr_el2, x0

    /* FP/SIMD·트레이스 트랩 해제.
     * 0x33FF 는 CPTR_EL2 의 RES1 비트만 세운 값 = 아무것도 트랩 안 함. */
    mov     x0, #0x33FF
    msr     cptr_el2, x0
    msr     hstr_el2, xzr           /* CP15 트랩 없음 */

    /* EL1 이 물리 카운터/타이머를 직접 볼 수 있게 허용.
     * 우리가 EL2 에 머물러도 리눅스는 EL1 에서 이걸 쓴다. */
    mrs     x0, cnthctl_el2
    orr     x0, x0, #3              /* EL1PCTEN | EL1PCEN */
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr        /* 가상 카운터 오프셋 0 */

    /* EL2 SCTLR 초기값: MMU/캐시/정렬검사 전부 끄고 RES1 비트만.
     * 0x30C50830 이 AArch64 EL2 의 RES1 조합이다. */
    mov     x0, #0x0830
    movk    x0, #0x30C5, lsl #16
    msr     sctlr_el2, x0
    isb

    mov     w23, #2                 /* 우리가 도는 예외 레벨 */
    b       common_entry

    /* ── 3c. 이미 EL1 로 들어온 경우 ────────────────────────────
     *
     * BCM2710 에서는 일어나지 않는다. 그래도 남겨두는 이유는, 이 코드가
     * 다른 보드로 옮겨갈 때 조용히 이상하게 도는 것보다 제대로 도는
     * 편이 낫기 때문이다. 이 경로로 오면 리눅스 인계 시 예외 레벨이
     * 어긋날 수 있고, 그건 부팅 로그에 경고로 남는다. */
el1_setup:
    mov     x0, #0x0800
    movk    x0, #0x30D0, lsl #16
    msr     sctlr_el1, x0

    mov     x0, #(3 << 20)          /* CPACR_EL1.FPEN = 0b11 */
    msr     cpacr_el1, x0
    isb

    mov     w23, #1
    b       common_entry

    /* ── 4. C 실행 환경 만들기 ──────────────────────────────────── */
common_entry:
    /* ── 4a. 여기서 코어가 갈린다 ───────────────────────────────
     *
     * 예외 레벨 강하를 코어 넷이 모두 거친 뒤에 가른다. 처음에는 반대
     * 순서였다 - _start 에서 코어 번호를 보고 보조 코어를 즉시 재웠다.
     * 그러면 보조 코어는 들어온 EL(보통 EL2)에 그대로 남고 주 코어만
     * EL1 로 내려간다.
     *
     * 우리 펌웨어만 도는 동안은 아무 문제가 없다. 자는 코어가 어느 EL 에
     * 있든 상관없기 때문이다. 리눅스를 띄우는 순간 드러난다 - 커널이
     * 코어 넷을 다 깨운 직후 "CPUs started in inconsistent modes" 라고
     * 경고한다. 부팅은 되지만 KVM 이 꺼지고, 그 뒤로 예외 레벨을
     * 전제하는 모든 판단이 코어마다 달라진다. */
    cbnz    x22, secondary_park

    /* ── 4b. 펌웨어가 준 디바이스 트리 주소를 남긴다 ────────────
     *
     * start.elf 는 커널을 부를 때 x0 에 DTB 주소를 넣어준다. 그 트리는
     * 이미 이 보드에 맞게 고쳐져 있다 - 실제 메모리 크기, 시리얼 번호,
     * 클럭 주파수, config.txt 의 dtoverlay 까지 반영되어 있다. 카드에서
     * .dtb 파일을 읽어 쓰는 것보다 언제나 낫다. 우리가 읽는 파일은 공장
     * 출하 상태라서, 예를 들어 이 보드에 램이 얼마나 붙어 있는지조차
     * 적혀 있지 않다. */
    adrp    x1, boot_dtb_addr
    add     x1, x1, :lo12:boot_dtb_addr
    str     x21, [x1]

    /* 우리가 도는 예외 레벨. 예외 핸들러가 ESR 을 EL1 에서 읽을지
     * EL2 에서 읽을지 이 값으로 고른다 - 틀린 쪽을 읽으면 그 자리에서
     * 또 예외가 나고, 예외 핸들러 안의 예외만큼 다루기 어려운 것이 없다. */
    adrp    x1, boot_el
    add     x1, x1, :lo12:boot_el
    str     w23, [x1]

    /* 스택 포인터. 우리 이미지는 0x80000 에 실리고 위로 자라므로
     * 스택은 0x80000 에서 아래로 내려간다 (약 512KB 여유).
     * 저주소 0x00~0xFF 의 펌웨어 스핀테이블까지는 절대 못 닿는다. */
    movz    x0, #0x8, lsl #16       /* 0x00080000 */
    mov     sp, x0

    /* BSS 를 0 으로. 링커가 16바이트 정렬을 보장하므로 8바이트씩 쓴다. */
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
1:
    cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:
    /* ── 5. 예외 벡터를 먼저 건다 ───────────────────────────────
     *
     * kernel_main 안에서 걸지 않고 여기서 거는 이유: 그 사이에 나는
     * 예외를 놓치기 때문이다. UART 초기화 자체가 잘못된 주소를
     * 건드리는 버그였던 적이 있는데, 벡터가 없으면 그 순간 보드가
     * 조용해질 뿐이고 - 하필 아직 UART 가 없어서 아무 말도 못 하는
     * 구간이라 - 증상만 보면 원인을 짐작할 방법이 전혀 없다.
     *
     * 벡터를 먼저 걸어두면 최소한 CPU 가 우리 핸들러까지는 온다.
     * 거기서 UART 를 직접 깨워 사정을 말할 수 있다. */
    adrp    x0, vector_table
    add     x0, x0, :lo12:vector_table
    cmp     w23, #2
    b.ne    1f
    msr     vbar_el2, x0
    b       2f
1:  msr     vbar_el1, x0
2:  isb

    /* C 로 진입. 돌아오지 않는다. */
    bl      kernel_main

    /* 혹시 돌아오면 안전하게 정지 */
halt:
    wfe
    b       halt

.size   _start, . - _start
.type   _start, %function

/* 진입 시점의 x0. BSS 가 아니라 .data 에 두는 이유: BSS 는 el1_entry
 * 에서 0 으로 채워지는데, 그 시점은 우리가 이 값을 저장한 뒤다. */
.section ".data"
.align  3
.global boot_dtb_addr
boot_dtb_addr:
    .quad   0

/* 펌웨어가 실제로 도는 예외 레벨 (2 또는 1). */
.align  2
.global boot_el
boot_el:
    .word   0
