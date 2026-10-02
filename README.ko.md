# linux-LP

**처음부터 직접 만든 리눅스 배포판입니다.** C 라이브러리, init, 셸, 부트로더,
명령어 130여 개를 모두 직접 작성했습니다. 라즈베리파이 제로 2 W에서 시작해
지금은 일반 PC(x86-64)용 데스크탑까지 돌아갑니다.

**Claude Opus 5.5(extra effort)로 전부 만든 OS입니다.** 리눅스 커널, 데비안 기반,
데스크탑 세션·앱·테마·아이콘·글꼴은 외부 오픈소스입니다 → [THIRD_PARTY.md](THIRD_PARTY.md)

> English → [README.md](README.md)

| | |
|---|---|
| 버전 | **1.411** |
| 데스크탑 | wayfire(GPU) / sway(소프트웨어) 위의 GTK 데스크탑, 독·앱 그리드·설정·파일·작업관리자·계산기 |
| 부팅 | 자체 UEFI 부트 메뉴(`lpboot.efi`), 복구 파티션과 복구 모드 |
| 설치 | USB로 켜서 그래픽 설치 프로그램으로 디스크에 설치 |
| 패키지 | 데비안 12(bookworm) `apt` 사용 가능 |
| 라이선스 | MIT (외부 구성요소는 각자의 라이선스) |

---

## 이미지 받기

설치 이미지는 **[lpos.cholab.kr](https://lpos.cholab.kr)** 에서 받을 수 있습니다.

| 파일 | 용도 |
|---|---|
| `linux-LP-desktop-20261002-45bb48bb.img.xz` | USB **16GB 이상**, 복구 파티션에 재설치 파일 포함 |
| `linux-LP_desktop-8g.img.xz` | USB **8GB**. 다운로드 사이트에는 없고, 소스에서 `LP_EDITION=8g` 로 빌드 |

최소 사양: 64비트 PC, **2코어 / 램 4GB / 디스크 16GB**, UEFI 부팅.
실제로 쓰고 있는 기기: Dell XPS 15 9550 (4K 터치스크린).

### USB에 굽기

먼저 압축을 풉니다. 결과물은 `.img` 파일이고, USB에는 이 `.img`를 씁니다.

```bash
xz -d -k linux-LP-desktop-20261002-45bb48bb.img.xz   # macOS: brew install xz
```

**macOS**
```bash
diskutil list external                  # USB가 /dev/diskN 인지 확인
diskutil unmountDisk /dev/diskN
sudo dd if=linux-LP-desktop-20261002-45bb48bb.img of=/dev/rdiskN bs=4m
diskutil eject /dev/diskN
```
다른 디스크 번호를 넣으면 그 디스크가 지워집니다. 번호를 꼭 확인하세요.
[balenaEtcher](https://etcher.balena.io/)로 `.img`를 골라 구워도 됩니다.

**Linux**
```bash
sudo dd if=linux-LP-desktop-20261002-45bb48bb.img of=/dev/sdX bs=4M conv=fsync status=progress
```

### 설치하기

1. USB를 꽂고 켠 뒤, 제조사 로고에서 부팅 메뉴 키(Dell은 **F12**)를 눌러 USB를 고릅니다.
2. 언어를 고르고 **LP 설치** → 계정 → 시간대 → 설치할 디스크 → 확인.
   "설치하지 않고 써 보기"로 먼저 둘러볼 수도 있습니다.
3. 설치가 끝나면 USB를 뽑고 다시 시작합니다.

Dell XPS 등에서 디스크가 안 보이면 BIOS(F2)에서 SATA 모드를 **AHCI**로 바꾸세요.

---

## 소스에서 빌드하기

데스크탑 이미지는 데비안 12 호스트에서 만듭니다.
루트 권한과 디스크 약 15GB가 필요합니다.

```bash
# 1. 유저랜드(자체 libc·init·셸·명령어)
make -C userland ARCH=amd64
(cd userland && LP_ARCH=amd64 LP_BINDIR=bin-amd64 LP_ROOTFS_DIR=rootfs-amd64 ./mkrootfs.sh)

# 2. 기본 패키지 lp-base (.deb)
tools/mkdeb.sh amd64

# 3. 데스크탑이 올라갈 데비안 베이스 (tools/desktop-packages.list)
sudo tools/apply-packages.sh

# 4. 이미지: sdcard/linux-LP_desktop.img (8GB판은 LP_EDITION=8g)
sudo tools/mkdesktop.sh
```

커널은 `kernel/build.sh`가 `kernel/linux.commit`에 적힌 버전을 받아
`kernel/lp-zero-amd64.config` 설정으로 빌드합니다. 자세한 내용은 [`GUIDE/`](GUIDE/)와
[`docs/`](docs/)에 있습니다.

## 폴더 구성

| 폴더 | 내용 |
|---|---|
| `userland/` | 자체 C 라이브러리, init, 셸, 명령어 |
| `desktop/` | 데스크탑 앱(독, 패널, 설정, 파일, 작업관리자, 계산기 …), 설치 프로그램, 세션 |
| `boot/efi/` | UEFI 부트 메뉴 `lpboot.efi` |
| `recovery/` | 복구 모드 |
| `kernel/` | 커널 설정과 빌드 스크립트 |
| `tools/` | 이미지·패키지를 만드는 스크립트 |
| `tests/` | 기기 안에서 도는 셀프테스트 |
| `repo/` | `pkg` 명령으로 받는 패키지 저장소. 쓰려면 기기에서 `pkg repo https://raw.githubusercontent.com/<아이디>/<레포>/main/repo` |

처음 라즈베리파이판(텍스트 모드, 시스템 전체가 RAM에서 도는 판)은 [PI-EDITION.md](PI-EDITION.md)(영어)에 설명되어 있습니다.

## 라이선스

이 저장소의 코드는 **MIT 라이선스**입니다. 자세한 내용은 [`LICENSE`](LICENSE)를 보세요.
누구나 자유롭게 쓰고, 고치고, 다시 배포하고, 상업적으로 써도 됩니다.
저작권 표시만 남겨 주세요.

포함되거나 이미지에 들어가는 외부 소프트웨어는 각자의 라이선스를 따릅니다.
- 리눅스 커널: GPL-2.0
- 데비안 패키지: 각 패키지의 라이선스. 소스는 debian.org에서 받을 수 있습니다.
- BearSSL: MIT
- dropbear: MIT
- wpa_supplicant: BSD
- OpenSSL: Apache 2.0
- 테마·아이콘·글꼴: [THIRD_PARTY.md](THIRD_PARTY.md) (Papirus: GPL-3.0, 글꼴: SIL OFL 1.1 등)

