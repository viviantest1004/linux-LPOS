# 커널 — 최소 리눅스 만들기

## 결과

| 차수 | 한 일 | Image | 부팅 |
|---|---|---|---|
| 1 | `CONFIG_MODULES=n` 만 | 37.7 MB | 1.84초 |
| 2 | 서브시스템 루트 옵션 차단 | 20.2 MB | 1.56초 |
| 3 | 측정으로 찾은 잔여 항목 | 17.4 MB | 0.92초 |
| 4 | `select` 선택자 차단 | **15.7 MB** | **0.83초** |

매 차수마다 QEMU 에서 init(PID 1)과 셸 프롬프트까지 확인했다.

### 15.7MB 가 왜 더 안 줄어드나

남아 있는 큰 덩어리는 대부분 **원격 접속 요구사항 때문에 필요한 것들**이다.

| | 크기 | 뺄 수 있나 |
|---|---|---|
| `drivers/net` (brcmfmac) | 2.40 MB | WiFi 드라이버 |
| `net/ipv4` | 1.43 MB | TCP/IP - SSH 에 필요 |
| `arch/arm64` | 1.38 MB | 아키텍처 코어 |
| `net/core` | 1.33 MB | 네트워크 코어 |
| `net/mac80211` | 1.00 MB | 802.11 스택 |
| `fs/ext4` | 0.92 MB | 데이터 파티션 |
| `net/wireless` (cfg80211) | 0.64 MB | WiFi 설정 계층 |
| `drivers/mmc` | 0.56 MB | SD 카드 |

합치면 약 10MB 다. **WiFi + TCP/IP + ext4 를 요구하는 한 이 아래로는
내려가기 어렵다.** 완전 헤드리스 어플라이언스(네트워크 없음)라면 3~5MB
까지 가능하지만, 그러면 SSH 접속을 포기해야 한다.

참고로 Raspberry Pi OS 는 커널 본체 ~9MB 에 모듈 ~100MB 가 별도다.
우리는 15.7MB 하나로 끝이고 `/lib/modules` 가 없다.

## 출발점

공식 `bcm2711_defconfig` 에서 깎는다. `tinyconfig` 에서 쌓아올리는 방법도
있지만, 부팅에 꼭 필요한 옵션을 하나씩 찾아내느라 시간이 훨씬 많이 든다.
검증된 defconfig 에서 빼는 쪽이 확실하다.

라즈베리파이 공식 문서 기준 `bcm2711_defconfig` 가 **Pi 3 / 3+ / CM3 /
CM3+ / Zero 2 W / 4 / 400 / CM4** 를 모두 덮는 arm64 defconfig 다.
(`bcmrpi3_defconfig` 는 6.12.20 에서 삭제됐다.)

```bash
make blobs      # 최초 1회
make kernel     # 유저랜드 빌드 -> rootfs 조립 -> 커널 빌드
make kernel-test
```

커널 소스 위치는 `LINUX_SRC` 환경변수로 바꾼다. 기본값은 저장소 옆의
`.build/linux` 이고, 작업 디렉터리 전체를 옮기려면 `LPZERO_WORK` 를
쓴다 (`LPZERO_WORK=/mnt/big/lpzero make kernel`).

## 유저랜드를 커널에 내장한다

`CONFIG_INITRAMFS_SOURCE` 에 `userland/rootfs` 디렉터리를 지정하면 커널
빌드가 그 내용을 cpio 로 만들어 이미지 안에 넣는다.

**결과적으로 `Image` 파일 하나만 있으면 셸까지 부팅된다.** 별도의
initramfs 파일도, 루트 파티션도 필요 없다.

## ⚠️ `CONFIG_MODULES=n` 만으로는 커널이 줄지 않는다

가장 크게 헛디딘 부분이라 기록해둔다.

"모듈 1,916개를 없애면 작아지겠지" 하고 `CONFIG_MODULES=n` 만 넣고
빌드했더니 결과가 이랬다:

| | `=y` | `=m` | Image |
|---|---|---|---|
| `bcm2711_defconfig` 원본 | 1,895 | 1,916 | (모듈 별도 ~100MB) |
| `CONFIG_MODULES=n` 만 추가 | **2,426** | 0 | **37.7 MB** |

**늘었다.** 이유는 `olddefconfig` 의 동작에 있다. 모듈 지원이 꺼지면
tristate 옵션에 `m` 값이 유효하지 않게 되고, `olddefconfig` 는 그 옵션을
**기본값으로 되돌린다.** 그런데 많은 드라이버의 기본값이 `y` 다.
그래서 모듈이던 것들이 사라지는 대신 **커널 안으로 들어왔다.**

1차 빌드의 부팅 로그가 증거다:

```
libceph: loaded (mon/osd proto 15/24)
batman_adv: B.A.T.M.A.N. advanced 2024.2 loaded
openvswitch: Open vSwitch switching datapath
NET: Registered PF_VSOCK protocol family
mpls_gso: MPLS GSO support
```

Ceph 분산 파일시스템, 메시 네트워크 라우팅, 가상 스위치가 우리
라즈베리파이에 들어가 있었다.

**해결: 서브시스템 루트 옵션을 직접 꺼야 한다.**
`CONFIG_USB_SUPPORT=n`, `CONFIG_ETHERNET=n`, `CONFIG_DRM=n`,
벤더별 `CONFIG_WLAN_VENDOR_*=n` 같은 것들이다.

## ⚠️ `=n` 을 적어도 안 꺼지는 경우가 있다

kconfig 의 `select` 는 **사용자 설정을 무시하고 심볼을 강제로 켠다.**
그래서 조각에 `CONFIG_X=n` 을 적어도 다른 켜진 옵션이 `select X` 하고
있으면 `y` 로 남는다. `# CONFIG_X is not set` 형태로 써도 마찬가지다.

3차 빌드에서 조각 134개 중 109개만 반영되고 10개가 `=y` 로 남았다.
선택자를 찾는 방법:

```bash
grep -rn --include='Kconfig*' 'select SCSI\b' /path/to/linux
```

실제로 찾아낸 것들:

| 안 꺼지던 것 | 실제 원인 (이걸 꺼야 함) |
|---|---|
| `CEPH_LIB` | `BLK_DEV_RBD` (Ceph 블록 장치) |
| `ZSTD_COMPRESS`, `LZ4_COMPRESS` | `CRYPTO_ZSTD`, `CRYPTO_LZ4`, `CRYPTO_LZO`, `CRYPTO_DEFLATE` |
| `CGROUPS` | `CHECKPOINT_RESTORE`, `SCHED_AUTOGROUP` |
| `SCSI` | **`ATA_OVER_ETH`, `ATALK`** |

마지막 것이 인상적이다. **ATA over Ethernet 과 AppleTalk** 이 켜져 있어서
SCSI 계층 전체를 끌고 오고 있었다. 범용 배포판 defconfig 가 어디까지
담고 있는지 보여주는 예다.

그래서 `build.sh` 는 병합 후 **조각의 모든 항목을 대조하고 반영되지 않은
것을 전부 보고한다.** 조용히 무시되면 왜 이미지가 안 줄어드는지 알 수 없다.

## 무엇을 끄나

Zero 2 W 의 하드웨어가 정해져 있다는 점이 근거다.

| 끈 것 | 근거 |
|---|---|
| `CONFIG_USB_SUPPORT` | **WiFi 가 SDIO 로 붙는다.** USB 가 아예 필요 없다 |
| `CONFIG_ETHERNET`, `CONFIG_PHYLIB` | 이더넷 포트가 없는 보드다 |
| `CONFIG_WLAN_VENDOR_*` (브로드컴 외) | 칩이 CYW43438 하나로 정해져 있다 |
| `CONFIG_DRM`, `CONFIG_FB` | 1단계는 시리얼 콘솔만. 프레임버퍼는 나중에 |
| `CONFIG_I2C`, `CONFIG_SPI`, `CONFIG_MTD` | 지금 쓰는 주변장치가 없다 |
| `CONFIG_SOUND`, `CONFIG_MEDIA_SUPPORT` | 오디오·카메라 미사용 |
| `CONFIG_IPV6`, `CONFIG_NETFILTER` | IPv4 만 쓴다 |
| `CONFIG_IIO`, `CONFIG_HWMON`, `CONFIG_W1` | 센서 미사용 |
| `CONFIG_FTRACE`, `CONFIG_KPROBES`, 디버그 정보 | 이미지 크기를 크게 먹는다 |
| `CONFIG_PCI` | **Zero 2 W 에 PCI 버스가 없다** (defconfig 가 Pi 4 의 PCIe 때문에 켜둔 것) |
| `CONFIG_SCSI`, `CONFIG_BCACHEFS_FS` | 저장장치가 SD 하나뿐이고 파일시스템은 ext4 만 쓴다 |
| `CONFIG_SECURITY`(LSM), `CONFIG_AUDIT` | 사용자가 하나뿐이라 강제접근제어를 쓸 일이 없다 |
| `CONFIG_CGROUPS`, `CONFIG_NAMESPACES` | 컨테이너를 쓰지 않는다 |

남기는 것은 `docs/00-roadmap.md` 의 저장 구조 절에 정리되어 있다.

## QEMU 테스트는 `virt` 머신으로

`raspi3ap` 이 하드웨어에는 더 가깝지만 **커널 테스트에는 쓸 수 없다.**

QEMU 의 raspi 머신은 디바이스 트리를 만들어주지 않는다. 실기에서는
`start.elf` 가 DTB 를 읽어 메모리 크기·시리얼 번호·활성화할 노드를
패치한 뒤 커널에 넘기는데, QEMU 에는 그 과정이 없다. 공식 DTB 를
`-dtb` 로 직접 줘도 패치되지 않은 상태라 콘솔조차 올라오지 않는다
(실제로 시도했고, 커널은 MMU 설정까지 진행했지만 출력이 한 글자도 없었다).

`virt` 는 QEMU 가 DTB 를 직접 생성한다. CPU 를 `cortex-a53`, 메모리를
512MB 로 맞추면 Zero 2 W 와 같은 조건이 된다.

```bash
qemu-system-aarch64 -M virt -cpu cortex-a53 -m 512 \
    -kernel kernel/out/Image \
    -append "earlycon console=ttyAMA0 rootwait" \
    -serial mon:stdio -display none
```

BCM 고유 주변장치(메일박스, VideoCore, SDIO WiFi)는 여기서 검증되지
않는다. 그건 실기에서 확인해야 한다. 하지만 **커널이 부팅하고
initramfs 를 풀고 우리 init 이 PID 1 로 뜨고 셸이 도는지**는 확인할 수
있고, 그게 이 단계에서 알고 싶은 전부다.

### 실행 중인 스크립트를 수정하지 말 것

4차 빌드에서 겪은 일이다. 빌드가 도는 동안 `build.sh` 를 편집했더니
빌드는 정상적으로 끝났는데 스크립트가 엉뚱한 곳에서 죽었다.

bash 는 스크립트를 한 번에 읽지 않고 **실행하면서 필요한 만큼씩 읽는다.**
파일이 중간에 바뀌면 다음에 읽을 바이트 위치가 그대로라 내용이 어긋난다.
빌드 중에는 스크립트를 건드리지 않는 편이 낫다.

성공하면 이렇게 나온다:

```
[    1.842085] Run /init as init process

  LP-zero OS
  init (pid 1)

/ $
```
