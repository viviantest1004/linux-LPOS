# 에뮬레이터에서 테스트하기 (QEMU / UTM)

실기 없이도 ARM 쪽 코드는 전부 검증할 수 있다.

## 핵심: SD 이미지는 에뮬레이터에서 부팅되지 않는다

**QEMU 는 VideoCore GPU 를 흉내내지 않는다.** 그래서
`sdcard/lp-zero.img` 를 통째로 물려도 부팅되지 않는다 —
`bootcode.bin` 과 `start.elf` 를 실행할 GPU 자체가 없기 때문이다.

대신 QEMU 는 GPU 가 하던 일(커널을 `0x80000` 에 올리고 ARM 을 깨우기)을
자기가 대신 해준다. 그래서 `-kernel kernel8.img` 로 직접 올려야 한다.

| | 실기 | QEMU |
|---|---|---|
| 부팅 시작 | GPU 부트ROM | QEMU 내장 스텁 |
| Broadcom 블롭 | 실행됨 | **실행 안 됨** |
| 넣는 것 | `sdcard/lp-zero.img` (SD카드) | `firmware/kernel8.img` (`-kernel`) |

ARM 쪽 코드는 양쪽에서 동일하게 돈다. 우리가 쓴 코드는 전부 ARM 쪽이므로
에뮬레이터 검증에 의미가 있다.

## 머신 선택

QEMU 에 Zero 2 W 전용 머신은 없다. 가장 가까운 것은 **`raspi3ap`**
(Raspberry Pi 3 Model A+) 이다.

| | Zero 2 W | raspi3ap | raspi3b |
|---|---|---|---|
| SoC | BCM2710A1 | BCM2837 | BCM2837 |
| 코어 | Cortex-A53 ×4 | 동일 | 동일 |
| RAM | 512MB | **512MB** | 1GB |
| 페리페럴 베이스 | `0x3F000000` | 동일 | 동일 |

메모리 크기까지 같아서 `raspi3ap` 이 가장 가깝다. 보드 리비전 코드만
Pi 3A+ 로 보고된다 (우리 코드가 그대로 해석해서 표시한다).

## 데스크탑에서

```bash
apt install qemu-system-arm      # 또는 brew install qemu

make qemu          # 대화형 - 시리얼이 터미널에 붙는다 (종료: Ctrl-A 다음 X)
make qemu-log      # 12초 돌리고 시리얼 로그를 뿌린다
make qemu-shot     # 부팅 화면을 qemu-out/*.ppm 으로 캡처
```

직접 실행하려면:
```bash
qemu-system-aarch64 -M raspi3ap -kernel firmware/kernel8.img \
    -serial mon:stdio -display none
```

### 함정: `-serial stdio` 가 파이프에서 조용하다

QEMU 의 stdio 캐릭터 장치는 stdout 이 TTY 가 아니면 출력을 내보내지
않는다. `qemu ... | head` 처럼 파이프로 넘기면 **아무것도 안 나온다**.
펌웨어가 멀쩡한데도 죽은 것처럼 보인다.

CI 나 스크립트에서는 파일로 받아야 확실하다:
```bash
qemu-system-aarch64 -M raspi3ap -kernel firmware/kernel8.img \
    -serial file:serial.log -display none
```
`make qemu-log` 가 이 방식을 쓴다.

## 윈도우

**된다.** QEMU 는 공식 윈도우 빌드가 있고 `virt` 머신을 그대로 지원한다.
아이폰 UTM 이 막힌 이유(raspi 머신 미지원)가 여기서는 해당 없다.

### 방법 1 — QEMU 만 설치

1. https://qemu.weilnetz.de/w64/ 에서 설치
2. 설치 폴더(보통 `C:\Program Files\qemu`)를 PATH 에 추가
3. PowerShell 에서:

```powershell
qemu-system-aarch64.exe -M virt -cpu cortex-a53 -m 512 `
  -kernel Image `
  -append "console=ttyAMA0 rootwait" `
  -serial mon:stdio -display none
```

종료는 `Ctrl-A` 다음 `X`.

### VirtualBox 만으로는 안 된다

**VirtualBox 는 에뮬레이터가 아니라 가상화기다.** 호스트 CPU 를 게스트에
그대로 빌려주는 방식이라 **호스트와 같은 아키텍처만** 돌릴 수 있다.
x86 PC 에서는 x86 게스트만 뜬다. 우리 커널은 aarch64 라 불가능하다.

QEMU 는 ARM 명령어를 x86 으로 번역해서 실행한다. 느린 이유이자 되는 이유다.

| | 하는 일 | ARM 코드 |
|---|---|---|
| VirtualBox | CPU 를 그대로 빌려줌 | ❌ |
| QEMU | 명령어를 번역 | ✅ |

**다만 VirtualBox 안에 리눅스를 깔고 그 안에서 QEMU 를 돌리면 된다.**
그러면 실행뿐 아니라 빌드까지 전부 가능하다.

```
Windows
  └─ VirtualBox
       └─ Ubuntu 게스트          <- 여기서 빌드
            └─ QEMU (aarch64 에뮬레이션)
                 └─ 우리 커널
```

VM 권장값: CPU 4코어 이상, RAM 8GB, 디스크 40GB
(커널 소스 2GB + 빌드 산출물).

### 방법 2 — WSL2 (권장)

```bash
wsl --install          # PowerShell 관리자 권한으로

# WSL 안에서
sudo apt install qemu-system-arm clang lld llvm \
                 gcc-aarch64-linux-gnu mtools dosfstools cpio bc flex bison
git clone <저장소>
make kernel-test
```

WSL2 를 권하는 이유는 실행만이 아니라 **프로젝트 전체를 직접 빌드**할 수
있기 때문이다. 커널 설정 수정, 셸에 명령 추가, SD 이미지 생성까지 전부
가능하다.

| | 아이폰 UTM | VirtualBox 단독 | 윈도우 QEMU | VBox+Ubuntu | WSL2 |
|---|---|---|---|---|---|
| 커널 부팅 테스트 | ❌ | ❌ | ✅ | ✅ | ✅ |
| 직접 빌드 | ❌ | ❌ | ❌ | ✅ | ✅ |
| SD 이미지 생성 | ❌ | ❌ | ❌ | ✅ | ✅ |
| 설치 부담 | — | — | 작음 | 큼 | 작음 |

이미 VirtualBox 에 리눅스가 있으면 그대로 쓰면 된다. 새로 준비한다면
WSL2 가 설치도 빠르고 디스크도 덜 먹는다.

`Image` 는 빌드 산출물이라 저장소에 없다. WSL2 로 직접 빌드하거나
릴리스에서 받아야 한다.

## UTM SE (아이폰/아이패드) — 사실상 안 된다

UTM SE 는 QEMU 를 iOS 로 포팅한 것이라 원리상 될 것 같지만,
**라즈베리파이 머신 타입이 iOS UTM 에서 동작하지 않는다는 보고가 있다.**

UTM 이슈 #6546 (UTM 4.5.3 / iOS 17, 작성 시점 기준 미해결):

> "All other architectures worked, but AARCH64 and ARM32,
>  I found that only QEMU Virtual Machine worked"

즉 iOS UTM 의 ARM64 에서는 generic `virt` 머신만 쓸 수 있고
`raspi*` 계열은 못 쓴다는 뜻이다.

**이게 왜 치명적인가**

우리 펌웨어는 BCM2710 하드웨어 주소에 하드코딩되어 있다:

| | 우리 펌웨어 (BCM2710) | QEMU `virt` 머신 |
|---|---|---|
| 페리페럴 베이스 | `0x3F000000` | 해당 없음 |
| PL011 UART | `0x3F201000` | `0x09000000` |
| 메일박스 | `0x3F00B880` | 없음 (VideoCore 자체가 없다) |

`virt` 에서는 주소가 전부 다르고 메일박스라는 개념 자체가 없다.
따라서 **`raspi*` 를 못 쓰면 우리 펌웨어는 아이폰에서 돌릴 수 없다.**
`virt` 용으로 따로 포팅하면 돌긴 하겠지만, 그건 더 이상 라즈베리파이
펌웨어가 아니다.

**결론: 데스크탑 QEMU 를 쓴다.** `make qemu` 한 줄이고, 스크린샷 캡처와
디스어셈블 로그, gdb 연결까지 전부 된다.

## 실기와 다른 점

에뮬레이터에서 통과해도 실기에서 다를 수 있는 것들:

| 항목 | QEMU | 실기 |
|---|---|---|
| UART 클럭 | 3MHz | 48MHz (`init_uart_clock`) |
| 화면 해상도 | 640×480 고정 | 모니터 EDID 에 따름 |
| 온도 | 25.0°C 고정 | 실제 센서 |
| 보드 시리얼 | 0 | 실제 값 |
| SD/EMMC | 부분 구현 | 실물 |
| 타이밍 | 실시간과 무관 | 실제 |

보레이트를 하드코딩하지 않고 메일박스로 UART 클럭을 조회하도록 만든
이유가 이것이다 — 3MHz 와 48MHz 양쪽에서 그대로 동작한다.
