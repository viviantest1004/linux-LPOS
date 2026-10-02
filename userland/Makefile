# userland/Makefile - 자체 libc 위에 올린 init 과 셸.
#
# 외부 libc 를 전혀 링크하지 않는다. crt0 부터 printf 까지 전부 우리 코드다.
# 정적 링크이므로 동적 링커(ld-linux-aarch64.so)도 필요 없다 -
# initramfs 에 바이너리만 넣으면 그대로 돈다.

ifeq ($(origin CC),default)
  CC := clang
endif
ifeq ($(origin LD),default)
  LD := ld.lld
endif
STRIP   ?= llvm-strip
OBJDUMP ?= llvm-objdump
AR      ?= llvm-ar

# ARCH picks the machine. clang is a cross compiler for everything it
# supports, so this is a target triple and a CPU name and nothing else -
# no second toolchain to install.
#
#   make            -> arm64, the Pi Zero 2 W and arm64 VMs
#   make ARCH=amd64 -> x86-64, an ordinary desktop or VM
#   make ARCH=armv6 -> 32-bit ARM, the Pi Zero W and Pi 1
ARCH ?= arm64

ifeq ($(ARCH),amd64)
  TARGET   := x86_64-linux-gnu
  CPUFLAGS := -march=x86-64
  OBJDIR   := build-amd64
  BINDIR   := bin-amd64
  # The same system on an ordinary desktop, and it says so rather than
  # claiming to be a Raspberry Pi.
  OSNAME   ?= linux-LP
  HOSTNAME ?= linux-lp
  OSTITLE  ?= linux-LP
  OSMACHINE ?= amd64 PC or virtual machine  -  built from scratch
else ifeq ($(ARCH),armv6)
  # The Pi Zero W is an ARM1176: ARMv6, one core, 32 bits, and no NEON.
  # armv6kz is what the chip actually is - the k adds the SMP-ish
  # instructions Linux wants and the z adds TrustZone, both of which the
  # kernel expects to be there on a BCM2835.
  TARGET   := arm-linux-gnueabihf
  CPUFLAGS := -march=armv6kz -mfpu=vfp -mfloat-abi=hard -marm
  OBJDIR   := build-armv6
  BINDIR   := bin-armv6
  OSNAME   ?= LP-zero
  HOSTNAME ?= lpzero
  OSTITLE  ?= test_a_123_LPzeroW_img
  OSMACHINE ?= Raspberry Pi Zero W  -  built from scratch
else ifeq ($(ARCH),arm64)
  TARGET   := aarch64-linux-gnu
  CPUFLAGS := -mcpu=cortex-a53
  OBJDIR   := build
  BINDIR   := bin
  OSNAME   ?= LP-zero
  HOSTNAME ?= lpzero
  OSTITLE  ?= test_a_123_LPzero2W_img
  OSMACHINE ?= Raspberry Pi Zero 2 W  -  built from scratch
else
  $(error ARCH must be arm64, armv6 or amd64, not "$(ARCH)")
endif

LIBC_DIR := libc

# ── BearSSL ──────────────────────────────────────────────────────
#
# TLS. The only cryptography in this system that is not ours, and it is
# not ours on purpose: a wrong bignum or a leaky comparison in a TLS
# stack fails silently and stays failed, which is the one kind of bug
# this project has no way to notice.
#
# It is compiled in rather than linked from a shared library, and the
# sources are in the repository rather than downloaded by a script,
# because `pkg` is what downloads things - a package manager that needs
# a package manager to build is not one.
#
# Every binary links against the archive, and the linker takes only the
# members it needs, so `ls` does not carry a TLS stack. Four programs
# actually pull it in: pkg, wget, net and update.
BEARSSL_DIR  := ../thirdparty/bearssl

# What the four defines turn off, and what we do instead:
#
#   BR_USE_URANDOM     BearSSL would open /dev/urandom itself. On this
#   BR_USE_WIN32_RAND  system that means a file descriptor and a path
#                      that may not exist yet - /dev is ours to set up
#                      and the SSH host key generator has already had to
#                      care about when. We seed the engine explicitly
#                      instead, from getrandom(2), and fail loudly if we
#                      cannot: a TLS client with a predictable random
#                      stream is worse than one that refuses to connect.
#
#   BR_USE_UNIX_TIME   BearSSL would call time(3), which we do not have.
#   BR_USE_WIN32_TIME  We hand the validator the clock explicitly, which
#                      also lets us say "your clock is wrong" instead of
#                      "certificate expired" - on a board with no RTC
#                      that is the difference between a fixable problem
#                      and a mystery.
BEARSSL_DEFS := -DBR_USE_URANDOM=0 -DBR_USE_WIN32_RAND=0 \
                -DBR_USE_UNIX_TIME=0 -DBR_USE_WIN32_TIME=0

#  -ffreestanding   표준 라이브러리 가정 없음
#  -nostdlibinc     시스템 헤더 배제 (stdarg.h 등 컴파일러 내장은 유지)
#  -fno-builtin     우리 memset 이 memset 호출로 바뀌는 무한재귀 방지
#  -static          동적 링커 없이 단독 실행
CFLAGS := --target=$(TARGET) $(CPUFLAGS) \
          -ffreestanding -nostdlibinc -fno-builtin \
          -fno-stack-protector -fno-pic -fno-pie \
          -ffunction-sections -fdata-sections \
          -std=c11 -Os \
          -Wall -Wextra -Werror \
          -DLP_OS_NAME='"$(OSNAME)"' \
          -DLP_OS_TITLE='"$(OSTITLE)"' \
          -DLP_OS_MACHINE='"$(OSMACHINE)"' \
          -I$(LIBC_DIR)/include -I$(BEARSSL_DIR)/inc

# Where `info` says the source is, and where `pkg` gets packages when
# nobody has chosen. Empty unless given, so a copy of this source names
# no repository until it is told its own:
#   make ARCH=amd64 LP_PROJECT_URL=https://github.com/<owner>/<repo> \
#        LP_PKG_REPO=https://raw.githubusercontent.com/<owner>/<repo>/main/repo
LP_PROJECT_URL ?=
LP_PKG_REPO    ?=
CFLAGS += -DLP_PROJECT_URL='"$(LP_PROJECT_URL)"' -DLP_PKG_REPO='"$(LP_PKG_REPO)"'

ASFLAGS := --target=$(TARGET) $(CPUFLAGS) -ffreestanding -nostdlibinc

LDFLAGS := -static --gc-sections -e _start

LIBC_SRCS := $(wildcard $(LIBC_DIR)/src/*.c)
LIBC_ASM  := $(wildcard $(LIBC_DIR)/src/*.S)
LIBC_OBJS := $(patsubst $(LIBC_DIR)/src/%.c,$(OBJDIR)/libc/%.o,$(LIBC_SRCS)) \
             $(patsubst $(LIBC_DIR)/src/%.S,$(OBJDIR)/libc/%.o,$(LIBC_ASM))

BEARSSL_SRCS := $(shell find $(BEARSSL_DIR)/src -name '*.c' | sort)
BEARSSL_OBJS := $(patsubst $(BEARSSL_DIR)/src/%.c,$(OBJDIR)/bearssl/%.o,$(BEARSSL_SRCS))
BEARSSL_LIB  := $(OBJDIR)/libbearssl.a

PROGS := init sh cat ls cp mv rm touch edit help more dhcp ntp date mount zram guard bootcount expandfs mkdir sleep kill top watchdog logd sysinfo calc clear wc head tail free df grep ps find du chmod splash pkg dmesg usage ping nslookup wget ifconfig route ln uname hostname uptime whoami sort uniq cut tee sha256sum reset stat fsck authkey integrity tar run ipconfig chattr dropprivs firewall disk part datadisk net beacon update chown id useradd su service pidof persist poweroff automount storage apt which basename dirname seq rev yes nl cmp tr printf xargs sed awk echo diff netstat info httpd dd defend lp-power ec2 cron voice preinit readlink realpath env printenv base64 mktemp tac fold paste comm split expr truncate nproc sync tty pwd link unlink mkfifo shuf factor od strings timeout nohup nice pgrep watch sysctl lscpu lsblk true false test expand unexpand tsort xxd cksum column arch hostid logname users getconf pmap install pathchk mknod sum gzip passwd crontab killall hwclock nc join fmt csplit numfmt shred blkid temp wifi lp-tune cal login lp-diskd lp-motd lp-net lp-privd sudo visudo wpa lp-reboot-recovery
BINS  := $(addprefix $(BINDIR)/,$(PROGS))

# libc 오브젝트는 중간 파일이 아니다. 이것이 없으면 make 가 패턴 규칙
# 사슬 끝에서 build/libc/*.o 를 지워버리고, 다음 빌드 때 다시 짓는다.
.SECONDARY:

.PHONY: all clean size test

all: $(BINS) size

$(OBJDIR)/libc $(OBJDIR)/prog $(BINDIR):
	@mkdir -p $@

$(OBJDIR)/libc/%.o: $(LIBC_DIR)/src/%.c | $(OBJDIR)/libc
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/libc/%.o: $(LIBC_DIR)/src/%.S | $(OBJDIR)/libc
	@echo "  AS      $<"
	@$(CC) $(ASFLAGS) -c $< -o $@

# BearSSL's own tree, compiled with our flags and its own switches.
#
# -Wno-macro-redefined is the whole of the concession: stddef.h defines
# NULL and so does our types.h, identically. Everything else builds
# under -Wall -Wextra -Werror unchanged, which is not luck - it is why
# this library was picked.
$(OBJDIR)/bearssl/%.o: $(BEARSSL_DIR)/src/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) $(BEARSSL_DEFS) -I$(BEARSSL_DIR)/src \
	       -Wno-macro-redefined -c $< -o $@

# An archive, not a list of objects. The linker pulls in only the
# members something references, so a program that never opens an https
# URL does not grow by a byte.
$(BEARSSL_LIB): $(BEARSSL_OBJS)
	@echo "  AR      $@"
	@rm -f $@
	@$(AR) rcs $@ $(BEARSSL_OBJS)

$(OBJDIR)/prog/%.o: %/%.c | $(OBJDIR)/prog
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# 각 프로그램은 자기 소스 하나 + libc 전체
.SECONDEXPANSION:
$(BINDIR)/%: $$*/$$*.c $(LIBC_OBJS) $(BEARSSL_LIB) | $(BINDIR) $(OBJDIR)/prog
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $(OBJDIR)/prog/$*.o
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) $(OBJDIR)/prog/$*.o $(LIBC_OBJS) $(BEARSSL_LIB) -o $@
	@$(STRIP) $@

size: $(BINS)
	@echo ""
	@for b in $(BINS); do \
	    printf "  %-12s %8s bytes\n" "$$b" "$$(stat -c%s $$b)"; \
	done
	@echo ""

clean:
	@rm -rf $(OBJDIR) $(BINDIR)
	@echo "  정리 완료"

-include $(shell find $(OBJDIR) -name '*.d' 2>/dev/null)
