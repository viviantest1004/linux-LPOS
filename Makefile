# LP-zero - 라즈베리파이 제로 2 W 자작 펌웨어/OS
#
#   make               펌웨어 빌드
#   make userland      자체 libc + init + 셸 빌드
#   make initramfs     rootfs 조립 + initramfs.cpio.gz
#   make blobs         Broadcom GPU 펌웨어 다운로드 (최초 1회)
#   make sdcard        부팅 가능한 SD카드 이미지 생성
#   make all-in-one    blobs + firmware + sdcard 한 번에
#   make qemu          QEMU 에서 펌웨어 실행
#   make clean         정리

.PHONY: all firmware userland initramfs kernel kernel-test blobs verify-blobs \
        sdcard sdcard-linux image all-in-one qemu qemu-log qemu-shot disasm syms \
        sysroot thirdparty python sdk dist clean distclean help

all: firmware

# ── 빌드 ────────────────────────────────────────────────────────
firmware:
	@$(MAKE) --no-print-directory -C firmware

# 외부 libc 없이 crt0 부터 직접 만든 유저랜드
userland:
	@$(MAKE) --no-print-directory -C userland

# 리눅스가 램디스크로 푸는 cpio 아카이브
initramfs: userland
	@cd userland && ./mkrootfs.sh --cpio

# 최소 리눅스 커널. 유저랜드를 이미지에 내장한다.
# 커널 소스 경로는 LINUX_SRC 로 바꿀 수 있다.
kernel: initramfs
	@./kernel/build.sh

# 빌드한 커널을 QEMU 에서 부팅시켜 셸까지 올라오는지 확인
kernel-test:
	@./kernel/test-qemu.sh log

disasm:
	@$(MAKE) --no-print-directory -C firmware disasm

syms:
	@$(MAKE) --no-print-directory -C firmware syms

# ── Broadcom 블롭 ───────────────────────────────────────────────
blobs:
	@./tools/fetch-blobs.sh

verify-blobs:
	@./tools/fetch-blobs.sh --verify

# ── SD 이미지 ───────────────────────────────────────────────────
sdcard: firmware
	@./tools/mksdcard.sh

# 우리가 빌드한 리눅스 커널을 부팅하는 SD 이미지
sdcard-linux: kernel
	@./tools/mksdcard.sh --linux

# 짧은 이름. 매일 도는 것은 이쪽이다.
image: sdcard-linux

# 배포용 두 개 (dist/)
dist: kernel
	@./tools/mkdist.sh

all-in-one: blobs firmware sdcard

# ── 한 번만 도는 빌드 ───────────────────────────────────────────
# 이 셋은 이미지가 아니라 이미지에 들어갈 재료를 만든다. 오래 걸리고,
# 그것들이 바뀔 때만 다시 돌리면 된다. 순서가 중요하다:
# sysroot -> thirdparty -> python.
sysroot:
	@./tools/build-sysroot.sh

thirdparty:
	@./tools/build-thirdparty.sh

python:
	@./tools/build-python.sh

# 이 시스템용 C 프로그램을 지을 수 있는 최소 SDK
sdk:
	@./tools/build-sdk.sh

# ── QEMU 에뮬레이션 (실기 없이 테스트) ──────────────────────────
# 주의: SD 이미지가 아니라 커널 이미지를 직접 올린다.
# QEMU 는 VideoCore GPU 를 흉내내지 않아 Broadcom 블롭이 돌지 않는다.
qemu: firmware
	@./tools/run-qemu.sh interactive

qemu-log: firmware
	@./tools/run-qemu.sh log

qemu-shot: firmware
	@./tools/run-qemu.sh shot

# ── 정리 ────────────────────────────────────────────────────────
clean:
	@$(MAKE) --no-print-directory -C firmware clean
	@$(MAKE) --no-print-directory -C userland clean
	@rm -rf qemu-out sdcard userland/rootfs userland/initramfs.cpio.gz
	@echo "  qemu-out/ sdcard/ rootfs/ initramfs 제거"

# 다운로드받은 블롭까지 전부 제거
distclean: clean
	@rm -rf blobs
	@echo "  blobs/ 제거"

help:
	@echo "LP-zero 빌드 타겟:"
	@echo "  make firmware      펌웨어 이미지 빌드"
	@echo "  make userland      자체 libc + init + 셸 빌드"
	@echo "  make initramfs     rootfs 조립 + initramfs.cpio.gz"
	@echo "  make kernel        최소 리눅스 커널 빌드 (유저랜드 내장)"
	@echo "  make kernel-test   커널을 QEMU 에서 부팅 검증"
	@echo "  make blobs         Broadcom GPU 펌웨어 다운로드"
	@echo "  make verify-blobs  받은 블롭 체크섬 검증"
	@echo "  make sdcard        SD 이미지 (베어메탈 펌웨어 부팅)"
	@echo "  make sdcard-linux  SD 이미지 (우리 리눅스 커널 부팅)"
	@echo "  make image         sdcard-linux 와 같다 (짧은 이름)"
	@echo "  make dist          배포용 .img.xz 와 .zip"
	@echo ""
	@echo "  한 번만 도는 것 (순서대로):"
	@echo "  make sysroot       CPython 이 링크할 라이브러리들"
	@echo "  make thirdparty    dropbear, wpa_supplicant"
	@echo "  make python        CPython 3.12 + pip + glibc"
	@echo "  make sdk           이 시스템용 C 프로그램을 짓는 SDK"
	@echo "  make all-in-one    blobs + firmware + sdcard"
	@echo "  make qemu          QEMU 에서 실행 (대화형)"
	@echo "  make qemu-log      QEMU 실행 후 시리얼 로그"
	@echo "  make qemu-shot     QEMU 부팅 화면 캡처"
	@echo "  make disasm        펌웨어 디스어셈블"
	@echo "  make syms          심볼 테이블(주소순)"
	@echo "  make clean         빌드 산출물 제거"
	@echo "  make distclean     블롭까지 제거"
