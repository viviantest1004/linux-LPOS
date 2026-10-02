# 부팅 사슬 — 전원이 들어온 뒤 무슨 일이 벌어지나

라즈베리파이의 가장 특이한 점: **ARM 이 먼저 부팅하지 않는다.**
VideoCore GPU 가 주인이고 ARM 은 나중에 깨어나는 보조 프로세서다.

```
전원 ON
  │
  ▼
[1] GPU 부트ROM                          ← BCM2710 실리콘 내부. 변경 불가
  │   · SD카드 첫 FAT 파티션을 찾는다
  │   · bootcode.bin 을 읽어 GPU L2 캐시에 올린다
  │     (이 시점엔 SDRAM 이 아직 안 켜져 있다)
  ▼
[2] bootcode.bin                         ← Broadcom 클로즈드 블롭 (52KB)
  │   · SDRAM 컨트롤러 초기화
  │   · start.elf 를 SDRAM 으로 로드
  ▼
[3] start.elf + fixup.dat                ← Broadcom 클로즈드 블롭 (2.9MB)
  │   · config.txt 파싱
  │   · gpu_mem 값에 따라 ARM/GPU 메모리 분할 결정 (fixup.dat 이 재배치)
  │   · arm_64bit=1 이면 kernel8.img 를 찾는다
  │   · kernel8.img 를 0x80000 에 raw 로 적재
  │   · armstub 을 거쳐 ARM 코어 4개를 릴리즈 → 0x80000 으로 점프
  ▼
[4] kernel8.img = boot.S 의 _start       ← ★ 여기부터 전부 우리 코드 ★
  │   · MPIDR_EL1 로 코어 번호 확인 → 0번만 진행, 1~3은 WFE 파킹
  │   · CurrentEL 확인 → EL3/EL2 → EL1 강하
  │   · SCTLR_EL1 초기화 (MMU/캐시 끔), CPACR_EL1 로 FP 허용
  │   · SP = 0x80000 (아래로 성장), BSS 를 0 으로
  ▼
[5] kernel_main()  (main.c)
      · uart_init()    PL011 을 GPIO14/15 에 붙이고 115200 8N1
      · 메일박스로 GPU 에 보드/메모리/클럭/온도 조회
      · 대화형 시리얼 모니터 진입
```

## 왜 [2][3] 은 직접 못 만드나

GPU 부트ROM 이 실리콘에 하드코딩되어 있고, 그것이 요구하는 파일 형식과
서명 방식이 Broadcom 비공개다. `librerpi/rpi-open-firmware` 같은 오픈
대체품 시도가 있지만 BCM2710 에서 안정적으로 동작하지 않는다.

**대신 [4] 부터는 100% 우리 코드다.** 이 저장소의 모든 `.c`/`.S` 가
그것이다.

## 로드 주소가 0x80000 인 이유

`arm_64bit=1` 일 때 start.elf 의 기본 커널 로드 주소다.
`linker.ld` 의 `. = 0x80000;` 과 `config.txt` 의 `kernel_address=0x80000`
이 셋이 전부 일치해야 한다. 하나라도 어긋나면 아무 출력 없이 멈춘다.

우리 이미지는 **위로** 자라고 스택은 0x80000 에서 **아래로** 자라므로
서로 충돌하지 않는다. 0x0~0x80000 의 512KB 가 스택 공간이다.

## 진입 예외 레벨

`arm_64bit=1` 에서 기본 armstub 을 거치면 보통 **EL2** 로 들어온다.
펌웨어 버전에 따라 EL3 이나 EL1 일 수도 있어서 `boot.S` 는 세 경우를
모두 처리한다. 최종적으로 항상 EL1h 에서 C 코드가 실행된다.

리눅스를 나중에 올릴 때도 커널은 EL2 진입을 선호하므로(KVM 을 위해),
Phase 3 의 부트로더에서는 EL2 를 유지한 채 넘길 예정이다.
