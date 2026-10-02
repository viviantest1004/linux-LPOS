# linux-LP — the Raspberry Pi edition

> This is the original document for the **Raspberry Pi edition** of linux-LP
> (text mode, the whole system in RAM). The pre-built Raspberry Pi images and
> the `dev` branch it mentions are not part of this repository. For the
> **desktop edition** and its download at [lpos.cholab.kr](https://lpos.cholab.kr),
> see [README.md](README.md).

---

**A Linux distribution written from nothing.** Its own C library, its own
init, its own shell, its own bootloader, and 130 commands. No busybox,
no glibc underneath, no userland from anywhere else. The whole system is
one kernel image file that unpacks into RAM at boot.

It began on a **Raspberry Pi Zero 2 W**, which has 512MB of RAM and loses
half of it to Ubuntu before you have done anything. Nothing in it turned
out to be specific to that board, so it now runs on **arm64 virtual
machines and ordinary amd64 PCs** as well.

| | |
|---|---|
| Whole system | **one** kernel + userland file, 13–22MB |
| RAM left after boot | about **480MB** on a 512MB board |
| Boot time | roughly 10 seconds from power to a prompt |
| Commands | **130**, every one written here |
| SSH | **built in**, public key only — `authkey new` makes one and prints the command to connect |
| Text tools | `sed` `awk` `grep` `diff`, over one shared regular expression engine |
| Packages | its own `pkg`, and `apt` — real Debian packages |
| Serving | `httpd`, `netstat`, and services of your own that init supervises |
| Python | CPython 3.12 + pip. manylinux wheels install |
| Firewall | on by default, and a port opens from the machine |
| Licence | MIT |

Four things come from elsewhere: the Broadcom blobs the Pi's GPU boot ROM
insists on, and three cryptographic implementations (dropbear,
wpa_supplicant, OpenSSL). **Cryptography is the one thing you do not
write yourself.**

---

## Who wrote this

**All of it was written by [Claude Code](https://claude.com/claude-code)**,
Anthropic's coding agent, working from the repository owner's direction:
the kernel configuration, the C library, init, the shell, every command,
the bare-metal bootloader, the build system, the self-test, and this
document.

The design and the debugging are **on the record** — each commit explains
what was wrong and why the fix is shaped the way it is, and the comments
say what was tried and what broke. At the same time, **no human wrote or
reviewed any of it.** No independent engineering review, no production
use, no track record.

---

## Before you rely on this

None of this is "so do not use it". It is all "so do not be surprised".

- **A hobby operating system, not a supported product.** No security
  team, no CVE process, nobody on call. For anything that has to keep
  working, use Raspberry Pi OS Lite or Debian.
- **Real Raspberry Pi hardware is not verified.** Everything was tested
  in QEMU. WiFi association, the thermal sensors, the hardware watchdog
  and the EMMC driver against an actual card are all unconfirmed.
- **The security work has not been audited** by anybody outside this
  repository. Signature checking on updates, the firewall, the DNS and
  NTP spoofing defences are all implemented and the reasoning is in the
  comments — which is not the same thing.
- **The C library is ours and it is not complete.** It has what the
  commands need. Compiling something else against it can meet a function
  that is not there. Ordinary Linux binaries run anyway, on the glibc
  beside Python.
- **`/data` is the only thing that survives a reboot,** and it is one
  partition on one card. Cards die.
- **`dd` to the wrong device destroys that device.** It is the one
  mistake here a reboot cannot undo.
- **No GUI, and it does not run containers.** The DRM drivers are in the
  kernel but X11 or Wayland is yours to install; cgroups and namespaces
  are not enabled for container use.
- **The rough edges are usually the quiet kind.** Found and fixed in one
  recent pass: `echo x | grep x` had answered "command not found" for as
  long as the shell existed, because a pipeline forked and exec'd every
  stage and echo is a builtin; `cmd ; echo $?` was always 0, because a
  line was parsed and `$?` expanded before any of it ran; `127.0.0.1` did
  not work at all, because nothing ever brought the loopback interface
  up; and apt could never have worked, because the Debian base image
  ships `/etc/resolv.conf` as a symlink into `/run` and writing over it
  failed unnoticed. Each has a check in the self-test now. Expect more of
  the same.

**What is verified**, on real QEMU boots of both architectures: the
self-test passes with nothing failing — booting, storage, SSH, Python,
external glibc binaries, graphics, security, error paths, redirection,
logging, time, every new command, and the shell's own syntax. Separately:
a fork bomb is cleared in a single sweep with init, SSH and the log still
running; memory exhaustion kills the process that caused it and nothing
else; and `apt` fetches, installs and runs a Debian package.

---

## What is on which branch

| Branch | What is in it |
|---|---|
| **`main`** | Everything. Full source, `GUIDE/`, both READMEs, the pre-built images in `dist/`, the download page. **This is the branch you want.** |
| **`dev`** | The development branch, identical in content to `main` at any settled moment. What it is for is the commit history — each message explains what was wrong and why the fix looks the way it does. |

Work goes to `main` only after it builds for both architectures and
passes `tests/selftest.sh` with nothing failing on each. More in
[`GUIDE/BRANCHES.txt`](GUIDE/BRANCHES.txt).

---

## Which image do I want

| Image | Where it runs |
|---|---|
| `dist/linux-LP_arm64_Zero2W.img.xz` | **Raspberry Pi Zero 2 W hardware and arm64 VMs alike.** One image does both: an uncompressed kernel for the Pi's GPU and an EFI executable for UEFI |
| `dist/linux-LP_armv6_ZeroW.img.xz` | **Raspberry Pi Zero / Zero W hardware.** ARM1176, ARMv6, 32-bit: a different instruction set from the Zero 2 W, so a separate image |
| `dist/linux-LP_amd64.img.xz` | **Ordinary PCs and desktop VMs.** VMware, VirtualBox, QEMU/KVM, Hyper-V |
| `dist/test_a_123_LPzero2W_linux-utm.zip` | **arm64 VMs only.** Unzip and double-click; UTM opens it |

Check what you downloaded — a truncated image fails in confusing ways
halfway through boot.

```bash
sha256sum -c dist/SHA256SUMS.txt
```

---

## Getting in over SSH

The SSH server is there from the start and comes up on its own.
**Password authentication is not compiled in at all** — on a board facing
the internet a password is a matter of time, so it was not offered.

That used to mean a freshly burned card let nobody in, and the only way
to add a key was a card reader. Now:

```
authkey new
```

makes a keypair on the machine, authorises the public half, leaves the
private half where you can fetch it, and prints the exact command to
connect with the machine's own address filled in.

To use a key you already have, the card's first partition is FAT32 and
opens on any computer:

```bash
cat ~/.ssh/id_ed25519.pub >> /media/BOOT/authorized_keys
```

> While `/boot/authorized_keys` exists it is the source of truth, and the
> copy on the machine is overwritten at boot. Delete it from `/boot` to
> manage keys on the machine instead. It works this way so that even if
> `/data` is destroyed, pulling the card puts a key back.

There is one user, root. When it does not work: `authkey -l`,
`service status dropbear`, `firewall status`, `netstat -l`, `logd`.

---

## Using it

### Burning a card

```bash
xz -d < linux-LP_arm64_Zero2W.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

**Check `/dev/sdX`.** On the first boot `/data` grows to fill the card.

### In a virtual machine

Decompress, attach as a disk, set **UEFI boot** — the MBR bootstrap is
deliberately empty. `truncate -s 16G` the image for a bigger disk; it is
sparse, and `/data` grows into it. Full QEMU command lines are in
[`GUIDE/USAGE.txt`](GUIDE/USAGE.txt).

---

## What you get once it boots

### Text and scripting

`sed` `awk` `grep` `diff` `sort` `uniq` `cut` `tr` `printf` `xargs`
`head` `tail` `wc` `nl` `rev` `cmp` `seq` `find` `tee` `cat` `more`
`edit`

One regular expression engine serves grep, sed and awk, so a pattern
means the same thing in all three. `grep` follows POSIX — `+` and `?` are
ordinary characters, `-E` gives the extended syntax, `-F` searches for a
plain string.

`edit` is a real editor: search and replace, undo and redo, go to line,
line numbers, select and paste, auto-indent, and it writes a temporary
and renames so a failed save leaves the original whole.

### The shell

Pipes, redirection including `2>` and `2>&1`, `&&` and `||`, `if`,
`while`, `for`, `break`, `continue`, functions, `$(...)` and backticks,
variables, globs, background jobs, `read`, `shift`, `export`.

A profile is read at login — `/etc/profile`, then `~/.profile`. `/root`
is on the data partition, so `~/.profile` survives a reboot. That is
where your settings go.

### Running a server

```
service add httpd -d /data/www -p 8080
firewall allow 8080
```

`httpd` serves a directory over HTTP: ranges so a big file resumes, 304
answers, a capped connection count and a read timeout on every socket.
Path traversal is rejected before and after percent-decoding, and it
drops privileges after binding.

`service add` means init supervises it — restarted when it dies, still
there after a reboot, and `guard` will not kill it under memory pressure.
That was not possible before: `/etc/services` lives in the kernel image,
so a program of your own could never be supervised, which is most of what
a server needs.

Opening a port no longer needs a card reader: `firewall allow 8080`,
`firewall deny 8080`, `firewall ports`.

`netstat` says what is listening and connected; `info` says everything
about the machine in one place, which is also what to paste into a bug
report.

### Debian packages

```
apt install ripgrep
apt run rg --version
```

Real Debian apt, into a separate tree under `/data/debian`. The first run
downloads 95MB and unpacks 440MB, which is why it is not in the image.
Nothing installed there can break this system's own commands — they are
not in there — and `rm -rf /data/debian` undoes all of it.

### Drives

Plug a USB drive in and it mounts itself under `/media`. To keep one in
the same place across reboots, `storage adopt /dev/sdb photos`. Adopted
drives are found by filesystem label rather than device name, so they
come back through a different port. `disk`, `part`, `datadisk`,
`expandfs`, `fsck` and `dd` handle the rest.

---

## Why it does not fall over

### The root filesystem is in RAM

The entire system is a cpio inside the kernel image, unpacked at boot.
**Nothing on disk is part of the running system.** Pulling the power
leaves nothing to corrupt; delete the wrong thing and a reboot puts it
back; an update is one file replaced.

`/bin`, `/lib`, `/usr`, `/opt` and friends are overlays — reads see the
image, writes land on `/data` — so `cp mytool /bin/` survives a reboot.

### `guard`

Watches memory, temperature, voltage, CPU and disk.

- **A fork bomb** is stopped with SIGSTOP on the whole process group
  first, because a stopped process cannot fork, and then swept — one
  pass, measured at 2493 processes, where before it killed nearly ten
  thousand a pass and never caught up.
- **Memory exhaustion**: the largest process goes, never init, the shell,
  SSH, the watchdog, or anything init supervises.
- **A runaway CPU**: a warning at 10 seconds, a priority drop at 30. It
  does not kill — you may have meant it.
- **Overheating, undervoltage, a full disk**: reported, and logs go
  first.

init watches guard in turn; kill it and it is back within a second.

### `temp`

How hot the board is, in one line. `temp -a` for every sensor, `temp -w`
to keep watching, `temp -c` for the bare number.

Printed next to the temperature is the GPU firmware's **undervoltage
record**, which is why this is a command of its own: a thin cable does
not crash the board, it corrupts the SD card three weeks later, and this
is the only place that fact is kept. A board that is cool and being
throttled is being throttled for the power, not the heat.

### There is always a way back

A hardware watchdog restarts a wedged board. Five boots that fail to last
five minutes skip `/data/rc.local`. A new system that cannot survive five
minutes is rolled back. SSH keys exist in three places, one of them the
FAT partition any PC can write. `fsck` repairs `/data` on the machine.
`poweroff` unmounts before cutting power.

---

## Security

- **No password authentication.** dropbear is compiled without it.
- **Signature checking on updates.** RSA-2048 / PKCS#1 v1.5 / SHA-256,
  with the public key inside the kernel image so pulling the card cannot
  change it. **There is no override.**
- **Firewall on by default,** rules pushed over netlink in one
  transaction, and now openable from the machine itself.
- **DNS and NTP spoofing defences.** Random transaction IDs, a connected
  socket, the question section checked; nonce, mode and stratum verified.
- **`/data` is nosuid,nodev,** as are external drives.
- **`defend`** watches what actually happens to a small internet-facing
  board: repeated SSH failures from one address, setuid and
  world-writable files where they do not belong, listeners that were not
  there before, and changes to the keys that can log in. **It is not an
  antivirus** — a signature database is tens of megabytes and needs daily
  updates, and this whole system is 13–22MB.
- **Login records** in `/data/log/auth`, and `integrity` hashes what
  survives a reboot.

None of this has been audited by anyone. See
[Before you rely on this](#before-you-rely-on-this).

---

## Every command

`help` prints this on the machine, grouped. `help <name>` explains one.
`info` describes the machine itself.

---

## Building it yourself

```bash
sudo apt install clang lld llvm gcc-aarch64-linux-gnu \
     mtools dosfstools e2fsprogs xz-utils zip cpio bc \
     bsdextrautils qemu-system-arm qemu-system-x86 python3

./tools/fetch-kernel.sh && ./tools/fetch-blobs.sh
make && make kernel && make sdcard-linux      # arm64
./tools/mkdist.sh                             # all three releases
./tests/selftest.sh                           # on the machine
```

The checksums and the download page are generated from the actual files.
`mkrootfs` refuses to build an image containing a binary older than its
source — that shipped once, and the self-test then failed on code that
was already correct.

There is no `arm64/` or `amd64/` directory: both use the same sources,
and exactly three things differ — the syscall numbers, the entry point in
`crt0.S`, and the kernel configuration. Copies invite fixing one side and
not the other, which has happened here twice.

Full details, in English and Korean, in
[`GUIDE/USAGE.txt`](GUIDE/USAGE.txt).

---

## Licence

MIT — see [`LICENSE`](LICENSE). The bundled third-party software keeps
its own: dropbear (MIT), wpa_supplicant (BSD), OpenSSL (Apache 2.0), the
Broadcom GPU firmware (proprietary, redistribution permitted), and the
Linux kernel (GPL-2.0).
