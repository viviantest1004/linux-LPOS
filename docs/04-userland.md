# 유저랜드 — 자체 libc · init · 셸

외부 libc 를 전혀 링크하지 않는다. `_start` 부터 `printf` 까지 전부 우리 코드다.

```
userland/
├── libc/
│   ├── include/  types.h syscall.h unistd.h string.h stdio.h stdlib.h
│   └── src/      crt0.S unistd.c string.c stdio.c malloc.c
├── init/         PID 1
├── sh/           셸
├── cat/  ls/     유틸리티
├── cp/  mv/  rm/  mkdir/
├── ntp/          시각 동기화 (DNS 리졸버 포함)
└── mkrootfs.sh   rootfs 조립 + initramfs(cpio) 생성
```

현재 크기:

| | 바이트 |
|---|---|
| `init` | 4,352 |
| `sh` | 8,576 |
| `cat` | 3,512 |
| `ls` | 3,864 |
| **initramfs.cpio.gz** | **7,693** |

## AArch64 시스템콜의 함정

x86 을 기준으로 알던 시스템콜이 AArch64 에는 **아예 없다**. asm-generic
표를 쓰기 때문이다:

| 익숙한 것 | AArch64 에서는 |
|---|---|
| `open` | `openat(AT_FDCWD, ...)` |
| `stat` | `newfstatat` |
| `fork` | `clone(SIGCHLD, 0, 0, 0, 0)` |
| `pipe` | `pipe2(fds, 0)` |
| `dup2` | `dup3(old, new, 0)` — 단 old==new 면 EINVAL 이라 따로 처리 |

호출 규약은 번호를 `x8`, 인자를 `x0`~`x5`, `svc #0`. 반환은 `x0` 이고
**오류는 음수 `-errno`** 로 온다 (glibc 처럼 -1 + errno 전역이 아니다).
그래서 우리 래퍼는 errno 전역 없이 음수를 그대로 노출한다.

## 빌드와 테스트

```bash
cd userland
make                    # bin/{init,sh,cat,ls}
./mkrootfs.sh --cpio    # rootfs/ 와 initramfs.cpio.gz
```

x86 호스트에서 aarch64 바이너리를 돌리려면 QEMU 유저모드가 필요하다:

```bash
apt install qemu-user qemu-user-static
qemu-aarch64 bin/sh                 # 단독 실행
```

`init` 이 셸을 `execve` 하는 것까지 검증하려면 binfmt_misc 등록이 필요하다.
QEMU 유저모드는 `execve` 를 호스트 커널로 넘기는데, 호스트가 aarch64 를
읽을 줄 알아야 하기 때문이다.

```bash
mount -t binfmt_misc none /proc/sys/fs/binfmt_misc
cat > /tmp/reg <<'EOF'
:qemu-a64:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\xb7\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-aarch64-static:OCF
EOF
cat /tmp/reg > /proc/sys/fs/binfmt_misc/register

chroot userland/rootfs /bin/init      # 전체 통합 테스트
```

### ⚠️ 등록 문자열의 백슬래시를 셸이 해석하게 두면 안 된다

커널은 `\x7f` 같은 **리터럴 텍스트**를 받아 자기가 unquote 한다.
`printf ':qemu:M::\x7fELF\x02...'` 처럼 셸이 미리 실제 바이트로 바꾸면
문자열이 **NUL 바이트에서 잘린다.** 그러면 매직이

```
magic 7f454c46020101        <- 7바이트만 남음
mask  ffffffffffffff
```

가 되어 **모든 64비트 리틀엔디언 ELF 와 일치**한다. x86 바이너리까지
aarch64 에뮬레이터로 넘어가면서 시스템에서 아무것도 실행할 수 없게 된다
(`/bin/bash` 조차 `ENOEXEC`).

그래서 위처럼 **따옴표 친 heredoc**(`<<'EOF'`)을 써서 백슬래시를 보존한다.
등록 후에는 반드시 확인한다:

```bash
cat /proc/sys/fs/binfmt_misc/qemu-a64 | grep magic
# 7f454c460201010000000000000000000200b700  (40자여야 정상)
```

짧으면 즉시 해제한다. 이때 `echo` 와 리다이렉션은 bash 내장이라
`exec` 이 깨진 상태에서도 동작한다:

```bash
echo -1 > /proc/sys/fs/binfmt_misc/qemu-a64
```

또 인터프리터는 **정적 링크판**(`qemu-aarch64-static`)이어야 한다.
동적 링크판은 chroot 안에서 자기 공유 라이브러리를 찾지 못한다.

## 파이썬을 /data 에 두는 이유

initramfs 는 커널 이미지 안에 박혀 있다. 거기에 넣은 것은 그대로 부팅
이미지 크기가 되고, 부팅할 때마다 전부 RAM 으로 풀린다. 512MB 짜리
보드에서 25MB 를 상주시킬 이유가 없다.

`/data` 는 첫 부팅에 카드 끝까지 늘어난다(`expandfs`). 크기가 문제되지
않는 자리다. PATH 에 `/data/bin` 이 들어 있어 이름만으로 실행된다.

| | 크기 | 시작 | 쓸 곳 |
|---|---|---|---|
| `calc` | 6.6KB | 즉시 | 사칙연산, 16진수·2진수 변환 |
| `micropython` | 1.6MB | 즉시 | 짧은 스크립트, 네트워크 |
| `python` (CPython) | 26MB | 체감됨 | 표준 라이브러리가 필요할 때 |

## ⚠️ RTC 가 없으면 HTTPS 가 안 된다

Pi Zero 2 W 에는 배터리로 도는 시계가 없다. 전원을 넣으면 커널 시계가
**1970년 1월 1일**에서 시작한다.

그 상태로 HTTPS 를 쓰면 이렇게 된다:

```
The certificate validity starts in the future
```

서버 인증서의 `notBefore` 가 "지금"보다 미래이기 때문이다. 인증서 자체는
멀쩡한데 시계가 틀려서 전부 거부된다. 이 증상은 인증서 문제로 보이기
때문에 CA 번들을 의심하며 한참 헤매기 쉽다.

그래서 `ntp` 를 만들었다:

```
ntp                 기본 서버에서 받아 시계를 맞추고 /data/.clock 에 저장
ntp <서버|IP>       지정한 서버에서
ntp -r              네트워크 없이, 저장해둔 시각으로 되돌린다
```

`/etc/rc` 는 두 번 부른다. `/data` 를 마운트한 직후 `ntp -r` 로 지난번
시각을 올려두고(네트워크가 없어도 1970년은 면한다), DHCP 가 끝난 뒤
`ntp` 로 진짜 시각을 맞춘다. 시계는 절대 뒤로 가지 않는다.

호스트 이름 해석은 직접 한다. 우리 libc 에는 리졸버가 없으므로
`/etc/resolv.conf`(dhcp 가 쓴다)의 네임서버에 A 레코드를 물어본다.

## 부트 파티션으로 설정하기

부트 파티션은 FAT32 라 윈도우·맥에서도 보인다. 리눅스 없이 기기를
설정할 수 있는 유일한 통로다. `/etc/rc` 가 부팅할 때 읽어간다.

```
authorized_keys       SSH 공개키 - 이게 없으면 아무도 못 들어온다
wpa_supplicant.conf   무선 공유기 정보
```

`cp` 에 `-n`(있으면 덮지 않음)과 `-q`(원본이 없어도 조용히)를 넣은 것은
이것 때문이다. 우리 셸에는 `if` 도 `test` 도 없어서 "있으면 건너뛴다"를
달리 표현할 방법이 없다. 같은 이유로 `sh` 에 `-q` 가 있다
(`/data/rc.local` 처럼 있을 수도 없을 수도 있는 스크립트용).

## 아직 없는 것

셸: 잡 컨트롤, 변수 확장(`$VAR`), 와일드카드(`*`), 서브셸,
여러 줄 문자열(`python -c` 에 줄바꿈이 든 코드를 못 넘긴다 - 파일로 저장해서
실행해야 한다).
libc: 부동소수점 printf, 시그널 핸들러, 스레드, `atoll`.
편집기: 없다. `cat > 파일` 로 만들고 Ctrl-D 로 끝낸다.

필요해질 때 붙인다. 지금은 셸이 도는 데 필요한 만큼만 있다.
