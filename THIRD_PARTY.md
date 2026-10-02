# Third-party software, themes and fonts

linux-LP's own code — the C library, init, shell, commands, boot menu, recovery
mode, desktop apps, installer, build scripts, the LP GTK stylesheet, the LP icon
theme, the wallpapers and the logo — is under the MIT License ([`LICENSE`](LICENSE)).

Everything on this page was made by other projects. It is used as-is or lightly
configured, and each item keeps its own license. On an installed system, the
license of every Debian package is in `/usr/share/doc/<package>/copyright`, and
the source of every Debian package is available from
[sources.debian.org](https://sources.debian.org).

## Kernel and firmware

| Component | How LP uses it | License |
|---|---|---|
| [Linux kernel](https://github.com/raspberrypi/linux) 6.12.107 | Raspberry Pi kernel tree, branch `rpi-6.12.y`, commit in [`kernel/linux.commit`](kernel/linux.commit). LP adds a configuration and four small DRM patches ([`kernel/patches/`](kernel/patches/)) | GPL-2.0 |
| [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git) and Debian `firmware-*` packages | Graphics, Wi-Fi, Bluetooth and sound firmware for PCs | Various, redistributable |
| [Intel processor microcode](https://github.com/intel/Intel-Linux-Processor-Microcode-Data-Files) | CPU microcode for PCs | Intel, redistributable |
| [broadcom-bt-firmware](https://github.com/winterheart/broadcom-bt-firmware) | Bluetooth firmware for Broadcom cards | Broadcom, redistributable |
| [wireless-regdb](https://git.kernel.org/pub/scm/linux/kernel/git/sforshee/wireless-regdb.git) | Wi-Fi regulatory database | ISC |
| [Raspberry Pi boot firmware](https://github.com/raspberrypi/firmware) (`bootcode.bin`, `start.elf`, `fixup.dat`) and [Raspberry Pi Wi-Fi firmware](https://github.com/RPi-Distro/firmware-nonfree) | Raspberry Pi editions only | Broadcom, redistribution permitted |

## Base system

| Component | How LP uses it | License |
|---|---|---|
| [Debian 12 "bookworm"](https://www.debian.org) | The base the desktop sits on: glibc, GTK, Mesa, drivers and the `apt` package manager. Every package LP asks for by name is in [`tools/desktop-packages.list`](tools/desktop-packages.list) | Each package's own license |

## Desktop session

Installed from Debian.

| Component | Role |
|---|---|
| [wayfire](https://wayfire.org) | Compositor with GPU acceleration |
| [sway](https://swaywm.org), swayidle, swaylock | Compositor for software rendering |
| Xwayland | Running X11 applications |
| [Waybar](https://github.com/Alexays/Waybar) | Bar |
| [fuzzel](https://codeberg.org/dnkl/fuzzel) | Launcher |
| [foot](https://codeberg.org/dnkl/foot), GNOME Console | Terminals |
| [mako](https://github.com/emersion/mako) | Notifications |
| wl-clipboard, wtype, grim, slurp, wlr-randr, wlsunset | Clipboard, input, screenshots, display settings, night light |
| [GTK 3 / GTK 4](https://www.gtk.org), libgtk-layer-shell | Toolkit the LP desktop apps are written in |
| [PipeWire](https://pipewire.org), WirePlumber, pavucontrol | Sound |
| [BlueZ](https://www.bluez.org) | Bluetooth |
| udisks2, gvfs, seatd, PolicyKit, UPower, D-Bus, udev | Disks, seats, permissions, power, system bus |
| [Flatpak](https://flatpak.org) with [Flathub](https://flathub.org) | Extra applications in the Software app |
| xdg-desktop-portal (-gtk, -wlr) | Desktop portals |
| [fcitx5](https://fcitx-im.org) and fcitx5-hangul | Korean input |

## Applications

Installed from Debian.

[Firefox ESR](https://www.mozilla.org/firefox/enterprise/),
[LibreOffice](https://www.libreoffice.org) (Writer, Calc, Impress),
gedit, GNOME Text Editor, Eye of GNOME, Evince, GNOME Calculator, GNOME Clocks,
File Roller, Baobab, GNOME Disks, GNOME Font Viewer, GNOME System Monitor,
gThumb, Drawing, mpv, Celluloid, FFmpeg, GStreamer, Geany, GParted,
and the command-line tools listed in [`tools/desktop-packages.list`](tools/desktop-packages.list)
(curl, git, vim, nano, tmux, htop, rsync and others).

## Themes and icons

| Component | How LP uses it | License |
|---|---|---|
| GTK's built-in **Adwaita** theme (Default / dark) | The LP GTK theme ([`desktop/theme/`](desktop/theme/)) is LP's own stylesheet imported on top of it | LGPL-2.1-or-later (part of GTK) |
| [gnome-themes-extra](https://gitlab.gnome.org/GNOME/gnome-themes-extra) | Installed (Adwaita for GTK 2, HighContrast) | LGPL-2.1-or-later |
| [**Papirus icon theme**](https://github.com/PapirusDevelopmentTeam/papirus-icon-theme) (Papirus-Dark) | The LP icon theme ([`desktop/icons/LP/`](desktop/icons/LP/)) draws its own app icons and inherits every other icon from Papirus-Dark | GPL-3.0 |
| [Adwaita icon theme](https://gitlab.gnome.org/GNOME/adwaita-icon-theme) | Second fallback icon theme | LGPL-3.0 / CC BY-SA 3.0 |
| [hicolor icon theme](https://www.freedesktop.org/wiki/Software/icon-theme/) | Last fallback icon theme | GPL-2.0 |

## Fonts

| Font | How LP uses it | License |
|---|---|---|
| [Pretendard](https://github.com/orioncactus/pretendard) 1.3.9 | Interface font; also the source of the recovery screen's pre-rendered glyphs ([`recovery/ui/lp-glyphs.h`](recovery/ui/lp-glyphs.h)). Downloaded by [`tools/fetch-fonts.sh`](tools/fetch-fonts.sh) | SIL OFL 1.1 |
| [D2Coding](https://github.com/naver/d2codingfont) 1.3.2 | Terminal font. Downloaded by [`tools/fetch-fonts.sh`](tools/fetch-fonts.sh) | SIL OFL 1.1 |
| [Noto Sans / Serif CJK](https://github.com/notofonts/noto-cjk) | Korean, Chinese and Japanese fallback | SIL OFL 1.1 |
| [Noto Color Emoji](https://github.com/googlefonts/noto-emoji) | Emoji | SIL OFL 1.1 |
| [Nanum, Nanum Coding](https://hangeul.naver.com/font) | Korean text in documents | SIL OFL 1.1 |
| [DejaVu](https://dejavu-fonts.github.io) | Latin fallback; the boot loader's 8×16 console font ([`firmware/src/font_8x16.c`](firmware/src/font_8x16.c)) is rendered from DejaVu Sans Mono | Bitstream Vera license (free) |

## Bundled in this repository or built from source

| Component | Version | How LP uses it | License |
|---|---|---|---|
| [BearSSL](https://bearssl.org) | — | TLS and cryptography in LP's C library (`wget`, `pkg`, Wi-Fi handshake, recovery); source in [`thirdparty/bearssl/`](thirdparty/bearssl/) | MIT |
| [musl](https://musl.libc.org) | 1.2.5 | C library for Dropbear and wpa_supplicant on the ARMv6 (Pi Zero W) build only | MIT |
| [Dropbear](https://matt.ucc.asn.au/dropbear/dropbear.html) | 2024.86 | SSH server | MIT |
| [wpa_supplicant](https://w1.fi/wpa_supplicant/) | 2.11 | Wi-Fi authentication | BSD-3-Clause |
| [libnl](https://github.com/thom311/libnl) | 3.11.0 | Netlink library for wpa_supplicant | LGPL-2.1 |
| [e2fsprogs](https://e2fsprogs.sourceforge.net) | 1.47.0 | Prebuilt `mke2fs`, `e2fsck`, `resize2fs` in [`userland/prebuilt/`](userland/prebuilt/) | GPL-2.0 |
| [CPython](https://www.python.org) | 3.12.3 | Python on the Raspberry Pi editions | PSF-2.0 |
| [OpenSSL](https://www.openssl.org) | 3.5.1 | Python's `ssl` module | Apache-2.0 |
| [ncurses](https://invisible-island.net/ncurses/) | 6.5 | Python's `curses` module | MIT-style |
| [GNU Readline](https://tiswww.case.edu/php/chet/readline/rltop.html) | 8.2 | Python's interactive prompt | GPL-3.0 |
| [SQLite](https://www.sqlite.org) | 3.50.1 | Python's `sqlite3` module | Public domain |
| [xz](https://tukaani.org/xz/) | 5.6.3 | Python's `lzma` module | 0BSD |
| [bzip2](https://sourceware.org/bzip2/) | 1.0.8 | Python's `bz2` module | BSD-style |
| [MicroPython](https://micropython.org) | 1.24.1 | Small Python for the Raspberry Pi editions | MIT |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | MicroPython's submodule | HTTPS in MicroPython | Apache-2.0 |

## Tools mentioned in the instructions

[balenaEtcher](https://etcher.balena.io/) is suggested for writing the image to a
USB drive. It is not part of LP.
