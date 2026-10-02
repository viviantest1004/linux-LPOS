# boot/efi/Makefile - build lpboot.efi, LP's graphical UEFI boot menu.
#
# clang and lld-link and nothing else: no gnu-efi, no EDK2. A UEFI
# application is a PE32+ executable with subsystem 10, which is what
# clang produces for the x86_64-unknown-windows target when it is told
# not to expect a C library (efi.h carries the few firmware tables the
# menu needs).
#
#   -ffreestanding -nostdlib   no libc; lpboot.c provides memset/memcpy
#   -fshort-wchar              wchar_t is 16 bits, so L"..." is a UEFI string
#   -mno-red-zone              firmware interrupt handlers run on our stack
#   -mno-stack-arg-probe       no __chkstk: there is no runtime to provide it
#   -fno-stack-protector       and no __security_cookie either
#
#   make -C boot/efi            -> boot/efi/lpboot.efi
#
# The image track installs the result as \EFI\LP\lpboot.efi (the NVRAM
# entry) and \EFI\BOOT\BOOTX64.EFI (the removable-media fallback).

CC      ?= clang
ifeq ($(origin CC),default)
  CC := clang
endif

CFLAGS := -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
          -mno-red-zone -mno-stack-arg-probe -fno-stack-protector \
          -fno-builtin -std=c11 -O2 -Wall -Wextra -Werror \
          -Wno-unused-const-variable
LDFLAGS := -target x86_64-unknown-windows -nostdlib -fuse-ld=lld-link \
           -Wl,-entry:efi_main -Wl,-subsystem:efi_application

HDRS := efi.h ../../recovery/ui/lp-ui.h ../../recovery/ui/lp-glyphs.h \
        ../../recovery/ui/lp-strings.h ../../userland/splash/logo.h \
        ../../desktop/branding/c/lp-logo-alpha.h

lpboot.efi: lpboot.c $(HDRS)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -c lpboot.c -o lpboot.o
	@echo "  LD      $@"
	@$(CC) $(LDFLAGS) lpboot.o -o $@
	@rm -f lpboot.o lpboot.lib
	@echo "  $$(stat -c%s $@) bytes  $@"

clean:
	rm -f lpboot.efi lpboot.o lpboot.lib

.PHONY: clean
