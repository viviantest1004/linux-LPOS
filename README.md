<p align="center">
  <img alt="LP" src="desktop/branding/logo/lp-tile.svg" width="96" height="96">
</p>

<p align="center">
  <b>An operating system built entirely with Claude Opus 5.5 (extra effort).</b><br>
  <a href="https://lpos.cholab.kr"><b>Download the installation image at lpos.cholab.kr</b></a>
</p>

<p align="center">
  <img alt="Version 1.411" src="https://img.shields.io/badge/version-1.411-f28c28">
  <img alt="Public beta" src="https://img.shields.io/badge/channel-public%20beta-0b2340">
  <img alt="x86_64 UEFI" src="https://img.shields.io/badge/platform-x86__64%20%C2%B7%20UEFI-0b2340">
  <img alt="Built with Claude Opus 5.5" src="https://img.shields.io/badge/built%20with-Claude%20Opus%205.5%20(extra%20effort)-d97757">
  <img alt="MIT License" src="https://img.shields.io/badge/license-MIT-0b2340">
</p>

# linux-LP

**A Linux distribution built from scratch.** Its own C library, init, shell,
boot loader and some 130 commands were all written for this project. It started
on the Raspberry Pi Zero 2 W and now runs as a desktop on ordinary PCs (x86-64).

> 한국어 → [README.ko.md](README.ko.md)

| | |
|---|---|
| Version | **1.411** (2026.10.02 build, public beta) |
| Desktop | A GTK desktop on wayfire (GPU) / sway (software) — dock, app grid, settings, files, task manager, calculator |
| Boot | Its own UEFI boot menu (`lpboot.efi`), a recovery partition and recovery mode |
| Install | Boot from USB and install to disk with the graphical installer |
| Packages | Debian 12 (bookworm) `apt` is available |
| License | MIT (third-party components keep their own licenses) |

---

## Built with Claude

LP OS was made **entirely with [Claude](https://www.anthropic.com/claude)
Opus 5.5 at extra effort**, working in [Claude Code](https://claude.com/claude-code):
the C library, init, shell and commands, the UEFI boot menu, recovery mode,
the desktop apps, the installer, the build scripts and the documentation.
The project owner set the direction and tested it on real hardware.

What LP runs on top of — the Linux kernel, the Debian base, the desktop
session, the applications, the themes, the icons and the fonts — was **not**
written by Claude or by this project. Those are third-party open-source
components, listed under [Third-party software](#third-party-software) and in
full in [THIRD_PARTY.md](THIRD_PARTY.md).

---

## Download the image

**The installation image can be downloaded from
[lpos.cholab.kr](https://lpos.cholab.kr).**

| File | Use |
|---|---|
| `linux-LP-desktop-20261002-45bb48bb.img.xz` | USB **16GB or larger**; the recovery partition carries the reinstall files. 2.13GB download, 9.08GB once unpacked |
| `linux-LP_desktop-8g.img.xz` | USB **8GB**. Not on the download site — build it from source with `LP_EDITION=8g` |

Minimum: a 64-bit PC, **2 cores / 4GB RAM / 16GB disk**, UEFI boot with
Secure Boot off. Legacy BIOS and 32-bit UEFI are not supported.
The device it is actually used on: Dell XPS 15 9550 (4K touchscreen).

Check the download against its SHA-256
([SHA256SUMS](https://lpos.cholab.kr/SHA256SUMS)):

```
0a50a416ba0eb7390ae620846de25bb861530438f83ad3cfb2cc2c4d4b7605f0  linux-LP-desktop-20261002-45bb48bb.img.xz
```

### Writing it to a USB drive

Unpack it first. The result is an `.img` file, and the `.img` is what goes on
the USB drive.

```bash
xz -d -k linux-LP-desktop-20261002-45bb48bb.img.xz   # macOS: brew install xz
```

**macOS**
```bash
diskutil list external                  # check that the USB drive is /dev/diskN
diskutil unmountDisk /dev/diskN
sudo dd if=linux-LP-desktop-20261002-45bb48bb.img of=/dev/rdiskN bs=4m
diskutil eject /dev/diskN
```
Giving it a different disk number erases that disk instead. Double-check the number.
You can also pick the `.img` in [balenaEtcher](https://etcher.balena.io/) and write it from there.

**Linux**
```bash
sudo dd if=linux-LP-desktop-20261002-45bb48bb.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Copying the file onto a USB drive does not make it bootable — it has to be
written as a disk image.

### Installing

1. Plug in the USB drive, power on, and press the boot-menu key at the
   manufacturer's logo (**F12** on a Dell) to pick the USB drive's UEFI entry.
2. Choose a language, then **Install LP** → account → time zone → disk to
   install to → confirm. You can also look around first with
   "Try without installing".
3. When the installation finishes, unplug the USB drive and restart.

If the disk does not show up on a Dell XPS or similar, switch the SATA mode to
**AHCI** in the BIOS (F2).

---

## Building from source

The desktop image is built on a Debian 12 host.
It needs root and about 15GB of disk.

```bash
# 1. The userland (our own libc, init, shell and commands)
make -C userland ARCH=amd64
(cd userland && LP_ARCH=amd64 LP_BINDIR=bin-amd64 LP_ROOTFS_DIR=rootfs-amd64 ./mkrootfs.sh)

# 2. The base package lp-base (.deb)
tools/mkdeb.sh amd64

# 3. The Debian base the desktop sits on (tools/desktop-packages.list)
sudo tools/apply-packages.sh

# 4. The image: sdcard/linux-LP_desktop.img (LP_EDITION=8g for the 8GB edition)
sudo tools/mkdesktop.sh
```

`kernel/build.sh` fetches the kernel version recorded in `kernel/linux.commit`
and builds it with the `kernel/lp-zero-amd64.config` configuration. More detail
is in [`GUIDE/`](GUIDE/) and [`docs/`](docs/).

## Folder layout

| Folder | Contents |
|---|---|
| `userland/` | Our own C library, init, shell and commands |
| `desktop/` | Desktop apps (dock, panel, settings, files, task manager, calculator …), the installer, the session |
| `boot/efi/` | The UEFI boot menu `lpboot.efi` |
| `recovery/` | Recovery mode |
| `kernel/` | Kernel configuration and build script |
| `tools/` | Scripts that build the images and packages |
| `tests/` | The self-test that runs on the device |
| `repo/` | The package repository the `pkg` command reads. To use it, run on the device: `pkg repo https://raw.githubusercontent.com/<owner>/<repo>/main/repo` |

The original Raspberry Pi edition (text mode, the whole system in RAM) is
described in [PI-EDITION.md](PI-EDITION.md).

---

## Third-party software

LP's own code is the part listed above. Everything below comes from other
open-source projects and is used as-is or lightly configured. The full list,
with versions and licenses, is in [THIRD_PARTY.md](THIRD_PARTY.md).

- **Kernel and base:** the Linux kernel 6.12 (Raspberry Pi kernel tree, with
  four small DRM patches in `kernel/patches/`), the Debian 12 "bookworm" base
  and the packages in [`tools/desktop-packages.list`](tools/desktop-packages.list),
  and hardware firmware (linux-firmware, Debian `firmware-*`, Raspberry Pi boot
  firmware).
- **Desktop session:** wayfire, sway, Xwayland, Waybar, fuzzel, foot, mako,
  PipeWire, BlueZ, Flatpak (apps from Flathub), GTK 3 / GTK 4.
- **Applications:** Firefox ESR, LibreOffice, GNOME apps (Console, Text Editor,
  Calculator, Clocks, Disks, System Monitor, Font Viewer, Image Viewer, Evince,
  File Roller, Baobab), gedit, gThumb, Drawing, mpv, Celluloid, Geany, GParted.
- **Korean input:** fcitx5 with fcitx5-hangul.
- **Themes and icons:** the LP GTK theme is LP's own stylesheet layered on
  GTK's built-in Adwaita dark theme; gnome-themes-extra is installed. The LP
  icon theme draws its own app icons and falls back to the **Papirus-Dark**
  icon theme, then Adwaita and hicolor.
- **Fonts:** Pretendard (interface, and the recovery screen's pre-rendered
  glyphs), D2Coding (terminal), Noto CJK, Noto Color Emoji, Nanum, DejaVu
  (the boot loader's bitmap console font is rendered from DejaVu Sans Mono).
- **Bundled or built from source:** BearSSL (in `thirdparty/bearssl/`), musl,
  Dropbear, wpa_supplicant, libnl, e2fsprogs (prebuilt `mke2fs`, `e2fsck`,
  `resize2fs` in `userland/prebuilt/`), CPython 3.12 with OpenSSL, ncurses,
  readline, SQLite, xz and bzip2, and MicroPython.

## License

The code in this repository is under the **MIT License** — see [`LICENSE`](LICENSE).
Anyone may use, modify, redistribute and use it commercially.
Just keep the copyright notice.

External software that is included here or goes into the image follows its own license:
- Linux kernel: GPL-2.0
- Debian packages: each package's own license. The source is available from debian.org.
- BearSSL: MIT
- Dropbear: MIT
- wpa_supplicant: BSD
- OpenSSL: Apache 2.0
- Themes, icons and fonts: see [THIRD_PARTY.md](THIRD_PARTY.md) (Papirus: GPL-3.0, fonts: SIL OFL 1.1, …)
