# recovery/Makefile - build lp-recovery, the recovery partition's menu.
#
# lp-recovery is several source files (the menu, the screen and input,
# the terminal the recovery shell runs in, the administrator check, the
# reinstall), which userland/Makefile's one-file-per-program rule cannot
# build, so it has this small Makefile of its own. It uses the same
# compiler flags as userland/Makefile and links the same libc, compiled
# here into build-$(ARCH)/ so that building recovery never races another
# `make` running in userland/ over the same object files.
#
#   make -C recovery               -> recovery/bin-amd64/lp-recovery
#   make -C recovery ARCH=arm64    -> recovery/bin/lp-recovery (builds; the
#                                     recovery partition is an amd64 thing)
#
# The glyph atlas (ui/lp-glyphs.h) is generated and checked in; rerun
# `python3.12 ui/mkglyphs.py` after changing ui/lp-strings.h or the
# terminal's character set (see mkglyphs.py for why it is checked in).

ifeq ($(origin CC),default)
  CC := clang
endif
ifeq ($(origin LD),default)
  LD := ld.lld
endif
STRIP ?= llvm-strip
AR    ?= llvm-ar

ARCH ?= amd64
ifeq ($(ARCH),amd64)
  TARGET   := x86_64-linux-gnu
  CPUFLAGS := -march=x86-64
  OBJDIR   := build-amd64
  BINDIR   := bin-amd64
else ifeq ($(ARCH),armv6)
  TARGET   := arm-linux-gnueabihf
  CPUFLAGS := -march=armv6kz -mfpu=vfp -mfloat-abi=hard -marm
  OBJDIR   := build-armv6
  BINDIR   := bin-armv6
else ifeq ($(ARCH),arm64)
  TARGET   := aarch64-linux-gnu
  CPUFLAGS := -mcpu=cortex-a53
  OBJDIR   := build
  BINDIR   := bin
else
  $(error ARCH must be arm64, armv6 or amd64, not "$(ARCH)")
endif

LIBC_DIR := ../userland/libc

# -O2 rather than userland's -Os: this program pushes 4K frames through
# per-pixel loops (gradient, glyph filtering, crossfades), and the size
# difference is a few kilobytes on a 6 GiB partition.
CFLAGS := --target=$(TARGET) $(CPUFLAGS) \
          -ffreestanding -nostdlibinc -fno-builtin \
          -fno-stack-protector -fno-pic -fno-pie \
          -ffunction-sections -fdata-sections \
          -std=c11 -O2 \
          -Wall -Wextra -Werror \
          -DLP_OS_NAME='"linux-LP"' -DLP_OS_TITLE='"linux-LP"' \
          -DLP_OS_MACHINE='"LP Recovery"' \
          -I$(LIBC_DIR)/include -I../thirdparty/bearssl/inc -I.
ASFLAGS := --target=$(TARGET) $(CPUFLAGS) -ffreestanding -nostdlibinc
LDFLAGS := -static --gc-sections -e _start

LIBC_SRCS := $(wildcard $(LIBC_DIR)/src/*.c)
LIBC_ASM  := $(wildcard $(LIBC_DIR)/src/*.S)
LIBC_OBJS := $(patsubst $(LIBC_DIR)/src/%.c,$(OBJDIR)/libc/%.o,$(LIBC_SRCS)) \
             $(patsubst $(LIBC_DIR)/src/%.S,$(OBJDIR)/libc/%.o,$(LIBC_ASM))

SRCS := lp-recovery.c screen.c system.c auth.c reinstall.c term.c osk.c
OBJS := $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))
# An archive, so the linker takes only the libc members lp-recovery uses
# (the TLS glue, for one, would otherwise drag in BearSSL).
LIBC_A := $(OBJDIR)/libc.a

.SECONDARY:
.PHONY: all clean

all: $(BINDIR)/lp-recovery

$(OBJDIR)/libc $(BINDIR):
	@mkdir -p $@

$(OBJDIR)/libc/%.o: $(LIBC_DIR)/src/%.c | $(OBJDIR)/libc
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJDIR)/libc/%.o: $(LIBC_DIR)/src/%.S | $(OBJDIR)/libc
	@echo "  AS      $<"
	@$(CC) $(ASFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.c | $(OBJDIR)/libc
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(LIBC_A): $(LIBC_OBJS)
	@echo "  AR      $@"
	@rm -f $@
	@$(AR) rcs $@ $(LIBC_OBJS)

$(BINDIR)/lp-recovery: $(OBJS) $(LIBC_A) | $(BINDIR)
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) $(OBJS) $(LIBC_A) -o $@
	@$(STRIP) $@
	@echo "  $$(stat -c%s $@) bytes  $@"

clean:
	rm -rf $(OBJDIR) $(BINDIR)

-include $(shell find $(OBJDIR) -name '*.d' 2>/dev/null)
