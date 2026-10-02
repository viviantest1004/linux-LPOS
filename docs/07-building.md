# Building it, and building things for it

Four separate builds live in this repository, and they are separate on
purpose. The system image is one thing; the parts that have to speak to
the outside world - Python, its packages, other people's binaries - are
another, and they carry their own baggage on the data partition where
deleting it puts things back.

| What | Script | How often |
|---|---|---|
| The sysroot CPython links against | `tools/build-sysroot.sh` | once |
| dropbear and wpa_supplicant | `tools/build-thirdparty.sh` | once, or on an update |
| CPython, pip and glibc | `tools/build-python.sh` | once |
| The userland, the kernel, the image | `make image` | every change |

## A fixed environment

`Dockerfile` pins the whole toolchain. Every package in it is there for
a reason written next to it - two of them (`bsdextrautils` for
`hexdump`, `libelf-dev`) are the kind whose absence ends a seven-minute
kernel build with an error that says nothing about what is missing.

    docker build -t lpzero .
    docker run --rm -it -v "$PWD":/src -w /src lpzero

Nothing is kept inside the container: the source and the output are both
on the host directory mounted at `/src`.

> Not yet verified by an actual `docker build` - there is no Docker
> daemon in the environment this was written in. Every package name in it
> was checked against the Ubuntu 24.04 archive, which is the same base
> the image uses.

## The order

    ./tools/build-sysroot.sh        # OpenSSL, ncurses, readline, sqlite,
                                    # xz, bzip2, zlib - static, for CPython.
                                    # Also Mozilla's root certificates.
    ./tools/build-thirdparty.sh     # dropbear, wpa_supplicant, libnl
    ./tools/build-python.sh         # CPython 3.12, pip, and staged glibc
    ./tools/build-fsck.sh           # e2fsck for the boot partition
    make image                      # userland -> kernel -> SD image

The first three take a while and only need doing again when something
they build changes. `make image` is the loop you actually live in.

## The two pieces of somebody else's code

dropbear and wpa_supplicant are the only third-party code in the system
image. They are there for one reason: writing your own cryptography is
the single kind of mistake in a project like this that is both easy to
make and silent when made. Everything else - the kernel configuration,
the libc, the shell, every command - is ours.

Their build configuration lives in the repository, not in the build
tree:

    thirdparty/dropbear-localoptions.h   password authentication off,
                                         Ed25519 only, no forwarding
    thirdparty/wpa_supplicant.config     WPA2-PSK and nothing else

That is deliberate. Those two files encode security decisions, and a
build tree is disposable. If they lived only there, someone rebuilding
dropbear from a fresh clone would get password authentication back
without ever being told it had been off.

`build-thirdparty.sh` checks: after linking, it looks for the password
authentication strings in the binary and refuses the build if they are
there. A missed configuration file produces no error of its own - just
an SSH server that accepts passwords.

### Updating them

    ./tools/build-thirdparty.sh --verify    # what checksums are recorded
    # edit DROPBEAR_VER / WPA_VER at the top of the script
    rm tools/thirdparty.sha256              # the old sums are for the old files
    ./tools/build-thirdparty.sh --force

The new checksums are recorded on the first run, so commit
`tools/thirdparty.sha256` with the version bump. Read the release notes
before bumping dropbear: it is the thing standing between the internet
and a root shell.

## Writing a program for this system

A binary from an ordinary aarch64 cross compiler will not run here. There
is no dynamic linker anywhere in the image, no shared libraries, and the
libc is `userland/libc` rather than glibc. The kernel looks for the
binary's interpreter, finds nothing, and refuses it.

That is not an accident - it is also what keeps other people's binaries
out - but it means your own program needs the SDK.

    ./tools/build-sdk.sh
    sdk/bin/lp-gcc -o myprog myprog.c

`sdk/README.md` has the rest. `sdk/bin/lp-gcc` is twenty lines of shell
around clang, and every flag on it is explained where it appears.

## Making a package out of it

    mkdir -p stage/bin && cp myprog stage/bin/
    ./tools/mkpkg.sh myprog 1.0 stage

That writes `repo/myprog-1.0.tar` and rewrites `repo/index`. Serve the
directory over HTTPS and the machine can install from it:

    pkg repo https://your.server/lpzero
    pkg update && pkg install myprog

Over plain HTTP it works too, and `pkg` says out loud that it does not
mean much: whoever can rewrite the index can rewrite the packages, and
the checksums will agree with each other perfectly.

A package is an uncompressed tar with relative paths, unpacked under
`/data`. `bin/foo` in the archive becomes `/data/bin/foo`, which is on
`PATH` already. `mkpkg.sh` refuses to publish an archive containing an
absolute path or `..`, and `pkg` refuses to install one - the check is
in both places because the archive can have come from anywhere.

## What is not there

There are no package signatures. `pkg` proves that what arrived is what
the index said would arrive, and HTTPS proves the index came from the
server it claims to. Neither proves the server was not compromised. For
something that matters, fetch it yourself, check it, and use `pkg add`,
which never touches the network at all.
