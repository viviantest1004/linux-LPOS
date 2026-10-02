/* preinit - get off the RAM root and onto a real one.
 *
 * This is the /init of a very small initramfs, and it has exactly one
 * job: find the root filesystem, mount it, and hand over to the real
 * init on it. Nothing else belongs here. Everything this program does
 * happens before there is a system to report a failure to, so every
 * failure it can have says what it was and what to do about it, on the
 * console, before stopping.
 *
 * ── Why this exists ──
 *
 * The Raspberry Pi images have no root filesystem at all: the whole
 * system is a cpio inside the kernel image, unpacked into RAM at boot,
 * and nothing on disk is part of the running system. That is what makes
 * a board survive having its power pulled, and it is the right answer
 * for a board in a cupboard.
 *
 * It is the wrong answer for a machine somebody sits in front of. A
 * desktop wants /etc to still be there tomorrow, packages installed into
 * /usr rather than an overlay, and a root that can hold more than the
 * RAM it is unpacked into. So the amd64 system boots this instead: a
 * kernel with a small initramfs that mounts a real partition and
 * switches to it - from the USB stick it was written to, and from the
 * NVMe disk it was installed on.
 *
 * ── How the handover works ──
 *
 * An initramfs cannot be unmounted - it is rootfs, the one mount the
 * kernel will not let go of. The way every Linux system does this is to
 * move the new root over the old one and chroot into it:
 *
 *     mount the real root at /newroot
 *     move /dev /proc /sys across, so they are not lost
 *     chdir /newroot
 *     mount --move . /
 *     chroot .
 *     exec /sbin/init
 *
 * The initramfs is then unreachable and its memory is reclaimed as the
 * files in it are freed. This program does not delete them first: it is
 * a few tens of KB and the /dev, /proc and /sys directories it made are
 * empty, so there is nothing worth the code to reclaim.
 *
 * ── Finding the root: which one, when there are two ──
 *
 * Every copy of this system has a root partition that looks the same:
 * ext4, labelled LP-ROOT, GPT type "Linux root (x86-64)". There are two
 * copies on the owner's machine the moment the installer finishes - the
 * NVMe it installed to and the USB stick it installed from, which is
 * usually still plugged in at the first reboot. The old rule, "the first
 * partition labelled LPROOT", then boots whichever the kernel happened
 * to enumerate first. Booting the stick's root under the NVMe's kernel
 * works, looks right and is wrong in every way that matters: settings
 * made that evening are on the stick.
 *
 * So the question is not "where is a root" but "where is the root that
 * belongs to the kernel that is running", and it is answered in order:
 *
 *   1. root=PARTUUID=<uuid>, the last root= on the command line. The
 *      installer writes the firmware boot entry for the installed disk
 *      with this as its load options; the kernel appends load options
 *      after its built-in command line, which is why the LAST root= is
 *      the one that counts (the kernel's own rule, too). A PARTUUID is
 *      unique per install - the installer makes fresh ones - so this is
 *      exact. If it is not there, nothing else is tried: guessing is
 *      how the wrong root gets booted.
 *
 *   2. root=/dev/<name>: taken at its word, as before.
 *
 *   2b. root=PARTLABEL=<name>: the GPT partition name. This is how the
 *      boot menu starts the recovery system (root=PARTLABEL=LP-RECOVERY
 *      lp.mode=recovery), and a name is by design the same on every LP
 *      disk - the USB stick carries one too. So the partition with that
 *      name on the disk the firmware booted wins; failing that, the only
 *      one with the name; two or more and nothing to choose by is a
 *      stop, not a guess.
 *
 *   3. Otherwise, the disk the firmware loaded this kernel from. The
 *      firmware records which boot entry it started (BootCurrent) and
 *      that entry's device path names the disk: by the EFI partition's
 *      own GUID for an entry the installer made, and by PCI slot, USB
 *      port or NVMe namespace for the entry the firmware makes itself
 *      when it boots \EFI\BOOT\BOOTX64.EFI from removable media or from
 *      a disk whose entry was lost. The root is the LP root partition on
 *      that same disk. This is what makes the stick boot the stick and
 *      the NVMe's fallback path boot the NVMe, with no command line to
 *      carry the answer.
 *
 *   4. If the boot disk cannot be worked out, a single partition with
 *      the label (root=LABEL=x, or LP-ROOT) is used, as this program
 *      always did. If there are several, it stops and says so rather
 *      than pick one.
 *
 * The root's label was LPROOT until the disk layout gained a recovery
 * partition and every LP partition was named LP-<something>. Either
 * spelling is accepted wherever the default label is looked for, so a
 * disk made before the rename still boots under a kernel made after it
 * (and the other way round) - the label is only ever the last resort.
 *
 * A label is still preferred over a device name for the same reason
 * `storage` uses one: sda2 becomes sdb2 the moment another disk is
 * plugged in, and a root that moves is a machine that does not boot.
 *
 * ── Checking it first ──
 *
 * Between mounting the root read-only and handing over, the root's own
 * e2fsck -p and fsck.fat -a check the root and the EFI partition beside
 * it (check_filesystems). It is the last moment nothing is writing to
 * either, and the one that makes a power cut cost a few seconds of work
 * rather than a filesystem nobody checked.
 *
 * GPT is read here rather than in libc's disk.h, which handles MBR only
 * on purpose (the Pi's firmware reads an MBR). This program is the one
 * place that must understand the amd64 disks, and it is also the one
 * place that must be small.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "disk.h"

#define NEWROOT   "/newroot"
#define DEF_LABEL "LP-ROOT"
#define OLD_LABEL "LPROOT"      /* the same partition, before the rename */
#define EFIVARS   "/sys/firmware/efi/efivars"
#define EFI_GLOBAL "8be4df61-93ca-11d2-aa0d-00e098032b8c"

/* How long to keep looking. USB mass storage on a cold xHCI takes the
 * longest of anything here - a second or two, occasionally more - and a
 * boot disk that is still arriving must be waited for, not replaced by
 * whichever disk arrived first. */
#define ROUNDS    100
#define ROUND_MS  100

/* GPT type "Linux root (x86-64)" from the Discoverable Partitions
 * Specification, in on-disk byte order. mkdisk.sh and lp-install both
 * give the root partition this type, which is what lets the root on a
 * disk be found without trusting a label every copy shares. */
static const u8 ROOT_X86_64[16] = {
    0xe3, 0xbc, 0x68, 0x4f, 0xcd, 0xe8, 0xb1, 0x4d,
    0x96, 0xe7, 0xfb, 0xca, 0xf9, 0x84, 0xb7, 0x09
};

#define MAXP (DISK_MAX * 16)

typedef struct {
    char path[48];      /* /dev/nvme0n1p2 */
    int  disk;          /* index into g_disks */
    int  index;         /* partition number, as the kernel numbers it */
    bool gpt;
    u8   type[16];      /* GPT partition type */
    u8   uuid[16];      /* GPT unique partition GUID - the PARTUUID */
    u8   mbr_type;
    u64  start;         /* first logical block */
    char name[40];      /* GPT partition name - the PARTLABEL - as ASCII */
    char fs[8];
    char label[24];
} part_t;

static blk_t  g_disks[DISK_MAX];
static u32    g_mbr_sig[DISK_MAX];
static int    g_ndisks;
static part_t g_parts[MAXP];
static int    g_nparts;

/* "quiet" on the kernel command line. The desktop boots with it so that
 * nothing but the firmware's logo and then the splash is ever on the
 * screen: a line of text between the two is a flash of console that
 * makes a smooth start look like a machine that stumbled. So progress
 * is not said when it is set - only failure is, and failure always is,
 * because a machine that stops at a black screen with no reason given
 * is the worst way for this program to go wrong. */
static bool g_quiet;

/* Said on the console, where there is no logger yet. */
static void say(const char *msg)
{
    if (!g_quiet)
        dprintf(STDERR_FILENO, "preinit: %s\n", msg);
}

static void sayf2(const char *a, const char *b)
{
    if (!g_quiet)
        dprintf(STDERR_FILENO, "preinit: %s%s\n", a, b);
}

/* Nothing can be done, and there is no init to fall back to. Say why in
 * full, then stop somewhere a person can read it - panicking the kernel
 * would scroll the reason away. */
static void give_up(const char *why, const char *detail)
{
    dprintf(STDERR_FILENO, "\n");
    dprintf(STDERR_FILENO, "preinit: ** %s\n", why);
    if (detail && *detail)
        dprintf(STDERR_FILENO, "preinit:    %s\n", detail);
    dprintf(STDERR_FILENO,
        "preinit:\n"
        "preinit:    This kernel needs a root filesystem on disk: an ext4\n"
        "preinit:    partition labelled %s on the disk it was loaded\n"
        "preinit:    from, or the one root= on the kernel command line\n"
        "preinit:    names (root=PARTUUID=..., root=LABEL=..., /dev/...).\n"
        "preinit:\n"
        "preinit:    If two LP disks are plugged in and this cannot tell\n"
        "preinit:    them apart, unplugging the one you did not mean to\n"
        "preinit:    boot is enough.\n"
        "preinit:\n"
        "preinit:    Nothing has been written to any disk. It is safe to\n"
        "preinit:    switch the power off.\n", DEF_LABEL);
    for (;;)
        lp_sleep_ms(60000);
}

/* ── little pieces ───────────────────────────────────────────────── */

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u64 le64(const u8 *p)
{
    return (u64)le32(p) | ((u64)le32(p + 4) << 32);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char HEX[] = "0123456789abcdef";

/* A GUID as text, the way blkid and the kernel print a PARTUUID. The
 * first three fields are stored little-endian and the last two as
 * bytes, which is the one thing about GUIDs everybody gets wrong once. */
static void guid_str(const u8 *g, char out[37])
{
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6,
                                   8, 9, 10, 11, 12, 13, 14, 15 };
    int o = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[o++] = '-';
        out[o++] = HEX[g[order[i]] >> 4];
        out[o++] = HEX[g[order[i]] & 15];
    }
    out[o] = '\0';
}

static bool same_text_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y)
            return false;
    }
    return *a == *b;
}

/* The standard reflected CRC-32. GPT checksums its header with it, and a
 * header that fails the check is not read: random bytes that happen to
 * start with "EFI PART" should not become a partition table. */
static u32 crc32(const u8 *p, size_t n)
{
    u32 c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static long read_at(const char *dev, u64 off, void *buf, size_t n)
{
    long fd = lp_open(dev, O_RDONLY, 0);
    if (fd < 0)
        return -1;
    long got = -1;
    if (lp_lseek((int)fd, (off_t)off, SEEK_SET) == (s64)off)
        got = lp_read((int)fd, buf, n);
    lp_close((int)fd);
    return got;
}

static u32 logical_block(const char *name)
{
    char p[96], v[16];
    snprintf(p, sizeof p, "/sys/block/%s/queue/logical_block_size", name);
    if (proc_read(p, v, sizeof v) <= 0)
        return 512;
    long b = strtol(v, 0, 10);
    return (b == 512 || b == 1024 || b == 2048 || b == 4096) ? (u32)b : 512;
}

/* ── root= ───────────────────────────────────────────────────────── */

/* Is `word` one of the words on the kernel command line? */
static bool cmdline_has(const char *word)
{
    char buf[2048];
    if (proc_read("/proc/cmdline", buf, sizeof buf) <= 0)
        return false;
    size_t wl = strlen(word);
    for (char *p = buf; *p; p++) {
        if (p != buf && p[-1] != ' ' && p[-1] != '\t')
            continue;
        if (strncmp(p, word, wl) == 0 &&
            (p[wl] == '\0' || p[wl] == ' ' || p[wl] == '\t' || p[wl] == '\n'))
            return true;
    }
    return false;
}

/* The LAST root=... in /proc/cmdline, or an empty string.
 *
 * The kernel command line here is the built-in one followed by whatever
 * the boot entry passed, so a root= in the boot entry has to win over
 * one compiled in. "root=" but not "rootwait=" or "rootflags=" - match
 * at a word boundary, or a machine with rootwait on the line would try
 * to mount a filesystem called "wait". */
static void root_from_cmdline(char *out, size_t n)
{
    out[0] = '\0';

    char buf[2048];
    if (proc_read("/proc/cmdline", buf, sizeof buf) <= 0)
        return;

    for (char *p = buf; *p; p++) {
        if (p != buf && p[-1] != ' ' && p[-1] != '\t')
            continue;
        if (strncmp(p, "root=", 5) != 0)
            continue;
        p += 5;
        size_t i = 0;
        while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' &&
               i < n - 1) {
            out[i] = p[i];
            i++;
        }
        out[i] = '\0';
    }
}

/* ── what is on the disks ────────────────────────────────────────── */

static void add_part(int disk, int index, const char *dname)
{
    if (g_nparts >= MAXP)
        return;
    part_t *p = &g_parts[g_nparts++];
    memset(p, 0, sizeof *p);
    p->disk = disk;
    p->index = index;
    /* The kernel names a partition on mmcblk and nvme with a "p" in
     * front of the number, and everything else without one. */
    size_t l = strlen(dname);
    bool needs_p = l && dname[l - 1] >= '0' && dname[l - 1] <= '9';
    snprintf(p->path, sizeof p->path, "/dev/%s%s%d", dname,
             needs_p ? "p" : "", index);
}

/* Read one GPT header and its entries. `lba` is where the header is. */
static bool read_gpt_at(int d, u32 bs, u64 lba)
{
    const char *dev = g_disks[d].path;
    u8 hdr[512];
    if (read_at(dev, lba * bs, hdr, sizeof hdr) != (long)sizeof hdr)
        return false;
    if (memcmp(hdr, "EFI PART", 8) != 0)
        return false;

    u32 hsize = le32(hdr + 12);
    if (hsize < 92 || hsize > sizeof hdr)
        return false;
    u32 want = le32(hdr + 16);
    memset(hdr + 16, 0, 4);
    if (crc32(hdr, hsize) != want)
        return false;

    u64 elba  = le64(hdr + 72);
    u32 count = le32(hdr + 80);
    u32 esize = le32(hdr + 84);
    if (esize < 128 || esize > 512 || count == 0 || count > 256)
        return false;

    static u8 ents[256 * 128];
    size_t need = (size_t)count * esize;
    if (need > sizeof ents)
        need = sizeof ents - sizeof ents % esize;
    if (read_at(dev, elba * bs, ents, need) != (long)need)
        return false;
    if (crc32(ents, (size_t)count * esize) != le32(hdr + 88) &&
        need == (size_t)count * esize)
        return false;

    const char *dname = g_disks[d].name;
    for (u32 i = 0; i * esize < need; i++) {
        const u8 *e = ents + (size_t)i * esize;
        static const u8 zero[16];
        if (memcmp(e, zero, 16) == 0)
            continue;
        add_part(d, (int)i + 1, dname);
        part_t *p = &g_parts[g_nparts - 1];
        p->gpt = true;
        memcpy(p->type, e, 16);
        memcpy(p->uuid, e + 16, 16);
        p->start = le64(e + 32);
        /* The name is 36 UTF-16LE code units. Ours are ASCII; anything
         * else becomes '?', which can then match nothing we look for. */
        int o = 0;
        for (int k = 0; k < 36 && o < (int)sizeof p->name - 1; k++) {
            u16 cu = (u16)(e[56 + 2 * k] | (e[57 + 2 * k] << 8));
            if (cu == 0)
                break;
            p->name[o++] = (cu >= 0x20 && cu < 0x7f) ? (char)cu : '?';
        }
        p->name[o] = '\0';
    }
    return true;
}

/* Every partition on every disk the kernel has found so far, MBR or
 * GPT, with the filesystem and label on each. Called once per round:
 * disks keep arriving for the first second or two. */
static void scan(void)
{
    g_ndisks = disk_list(g_disks, DISK_MAX);
    g_nparts = 0;

    for (int d = 0; d < g_ndisks; d++) {
        u8 mbr[DISK_SECTOR];
        g_mbr_sig[d] = 0;
        if (!disk_read_mbr(g_disks[d].path, mbr) || !mbr_valid(mbr))
            continue;
        g_mbr_sig[d] = le32(mbr + 440);

        u8 t1; u32 s1, c1; bool b1;
        mbr_get(mbr, 1, &t1, &s1, &c1, &b1);
        if (t1 == PART_TYPE_GPT) {
            u32 bs = logical_block(g_disks[d].name);
            if (!read_gpt_at(d, bs, 1)) {
                /* The backup header lives in the last block. A disk
                 * whose primary table was damaged still boots. */
                u64 last = g_disks[d].bytes / bs - 1;
                read_gpt_at(d, bs, last);
            }
            continue;
        }

        blk_t mp[DISK_PARTS];
        int np = disk_parts(g_disks[d].path, mp, DISK_PARTS);
        for (int k = 0; k < np; k++) {
            add_part(d, mp[k].index, g_disks[d].name);
            g_parts[g_nparts - 1].mbr_type = mp[k].type;
            g_parts[g_nparts - 1].start = mp[k].start;
        }
    }

    for (int i = 0; i < g_nparts; i++)
        disk_identify(g_parts[i].path, g_parts[i].fs, sizeof g_parts[i].fs,
                      g_parts[i].label, sizeof g_parts[i].label);
}

/* Could this partition be a root at all? ext4, and not an EFI or swap
 * partition that merely happens to say so. */
static bool is_ext4(const part_t *p)
{
    return strcmp(p->fs, "ext4") == 0;
}

/* The kernel's PARTUUID for this partition: the GUID on GPT, and
 * SSSSSSSS-PP (disk signature, partition number) on MBR. */
static bool partuuid_is(const part_t *p, const char *want)
{
    char s[40];
    if (p->gpt) {
        guid_str(p->uuid, s);
    } else {
        u32 sig = g_mbr_sig[p->disk];
        for (int i = 0; i < 8; i++)
            s[i] = HEX[(sig >> (28 - 4 * i)) & 15];
        s[8] = '-';
        s[9] = HEX[(p->index >> 4) & 15];
        s[10] = HEX[p->index & 15];
        s[11] = '\0';
    }
    return same_text_ci(s, want);
}

/* ── which disk did the firmware load us from ────────────────────── */

/* What the running boot entry says about where it points. */
typedef struct {
    bool has_guid;          /* an HD() node with a GPT partition GUID */
    u8   esp_guid[16];
    bool has_mbr;           /* an HD() node on an MBR disk */
    u32  mbr_sig;
    int  npci;
    u8   pci[8][2];         /* (device, function), outermost first */
    int  nusb;
    u8   usb[8];            /* port numbers, 1-based like Linux's */
    u32  nvme_ns;
    char entry[12];         /* "Boot0003", for the log line */
} hint_t;

/* An EFI variable, without the four attribute bytes efivarfs puts in
 * front of it. Returns the payload length, or -1. */
static long efivar(const char *name, u8 *buf, size_t n)
{
    char path[160];
    snprintf(path, sizeof path, "%s/%s-%s", EFIVARS, name, EFI_GLOBAL);
    u8 raw[1024];
    long got = read_at(path, 0, raw, sizeof raw);
    if (got < 4)
        return -1;
    got -= 4;
    if ((size_t)got > n)
        got = (long)n;
    memcpy(buf, raw + 4, (size_t)got);
    return got;
}

static bool read_hint(hint_t *h)
{
    memset(h, 0, sizeof *h);
    if (!lp_is_dir("/sys/firmware/efi"))
        return false;                   /* not booted through UEFI */

    lp_mount("efivarfs", EFIVARS, "efivarfs", MS_RDONLY, NULL);

    u8 cur[2];
    bool ok = false;
    if (efivar("BootCurrent", cur, sizeof cur) == 2) {
        unsigned num = (unsigned)cur[0] | ((unsigned)cur[1] << 8);
        char name[12] = "Boot";
        for (int i = 0; i < 4; i++)
            name[4 + i] = "0123456789ABCDEF"[(num >> (12 - 4 * i)) & 15];
        name[8] = '\0';
        strlcpy(h->entry, name, sizeof h->entry);

        u8 opt[1000];
        long n = efivar(name, opt, sizeof opt);
        if (n > 6) {
            /* EFI_LOAD_OPTION: attributes, the device path's length,
             * a UCS-2 description, then the device path itself. */
            u16 plen = (u16)(opt[4] | (opt[5] << 8));
            long off = 6;
            while (off + 1 < n && (opt[off] || opt[off + 1]))
                off += 2;
            off += 2;
            long end = off + plen;
            if (end > n)
                end = n;
            while (off + 4 <= end) {
                u8  type = opt[off], sub = opt[off + 1];
                u16 len  = (u16)(opt[off + 2] | (opt[off + 3] << 8));
                const u8 *d = opt + off;
                if (len < 4 || off + len > end)
                    break;
                if (type == 0x7f)
                    break;                          /* end of path */
                if (type == 1 && sub == 1 && len >= 6 && h->npci < 8) {
                    h->pci[h->npci][0] = d[5];      /* device */
                    h->pci[h->npci][1] = d[4];      /* function */
                    h->npci++;
                } else if (type == 3 && sub == 5 && len >= 6 && h->nusb < 8) {
                    /* UEFI counts hub ports from 0, Linux from 1. */
                    h->usb[h->nusb++] = (u8)(d[4] + 1);
                } else if (type == 3 && sub == 0x17 && len >= 8) {
                    h->nvme_ns = le32(d + 4);
                } else if (type == 4 && sub == 1 && len >= 42) {
                    if (d[41] == 2) {
                        h->has_guid = true;
                        memcpy(h->esp_guid, d + 24, 16);
                    } else if (d[41] == 1) {
                        h->has_mbr = true;
                        h->mbr_sig = le32(d + 24);
                    }
                }
                off += len;
            }
            ok = h->has_guid || h->has_mbr || h->npci > 0;
        }
    }
    lp_umount(EFIVARS, 0);
    return ok;
}

/* The chain of PCI functions and USB ports from the root complex down to
 * this disk, read out of where it sits in sysfs:
 *
 *   ../devices/pci0000:00/0000:00:14.0/usb1/1-4/1-4:1.0/host0/...
 *
 * gives PCI (0x14,0) and USB port 4. */
static void disk_chain(const char *name, u8 pci[8][2], int *npci,
                       u8 usb[8], int *nusb)
{
    char path[64], link[256];
    *npci = 0;
    *nusb = 0;
    snprintf(path, sizeof path, "/sys/block/%s", name);
    long n = lp_readlink(path, link, sizeof link - 1);
    if (n <= 0)
        return;
    link[n] = '\0';

    char *c = link;
    while (c && *c) {
        char *slash = strchr(c, '/');
        if (slash)
            *slash = '\0';
        size_t l = strlen(c);
        /* dddd:bb:dd.f */
        if (l == 12 && c[4] == ':' && c[7] == ':' && c[10] == '.' &&
            *npci < 8) {
            int hi = hexval(c[8]), lo = hexval(c[9]), fn = hexval(c[11]);
            if (hi >= 0 && lo >= 0 && fn >= 0) {
                pci[*npci][0] = (u8)(hi * 16 + lo);
                pci[*npci][1] = (u8)fn;
                (*npci)++;
            }
        } else if (l >= 3 && !strchr(c, ':') && strchr(c, '-') &&
                   c[0] >= '0' && c[0] <= '9') {
            /* A USB device: bus-port[.port...]. The deepest one wins,
             * so start the list again each time. */
            *nusb = 0;
            char *q = strchr(c, '-') + 1;
            while (*q && *nusb < 8) {
                usb[(*nusb)++] = (u8)strtol(q, &q, 10);
                if (*q == '.')
                    q++;
                else
                    break;
            }
        }
        c = slash ? slash + 1 : NULL;
    }
}

/* The disk the boot entry points at, or -1. */
static int boot_disk(const hint_t *h)
{
    /* An entry that names the EFI partition by GUID (every entry the
     * installer writes) says exactly which disk it is. */
    if (h->has_guid) {
        for (int i = 0; i < g_nparts; i++)
            if (g_parts[i].gpt && memcmp(g_parts[i].uuid, h->esp_guid, 16) == 0)
                return g_parts[i].disk;
        return -1;
    }
    if (h->has_mbr) {
        for (int d = 0; d < g_ndisks; d++)
            if (g_mbr_sig[d] && g_mbr_sig[d] == h->mbr_sig)
                return d;
        return -1;
    }

    /* The firmware's own entries name a device, not a partition: the
     * PCI function it hangs off and, below that, a USB port or an NVMe
     * namespace. Match the PCI chain first; if two disks share it (two
     * sticks on one controller), the USB port or namespace decides. */
    int found = -1, count = 0, exact = -1, nexact = 0;
    for (int d = 0; d < g_ndisks; d++) {
        u8 pci[8][2], usb[8];
        int npci, nusb;
        disk_chain(g_disks[d].name, pci, &npci, usb, &nusb);
        if (npci != h->npci || npci == 0)
            continue;
        if (memcmp(pci, h->pci, (size_t)npci * 2) != 0)
            continue;
        found = d;
        count++;

        bool better = false;
        if (h->nusb && nusb == h->nusb && memcmp(usb, h->usb, (size_t)nusb) == 0)
            better = true;
        if (h->nvme_ns) {
            const char *nm = g_disks[d].name, *ns = strrchr(nm, 'n');
            if (strncmp(nm, "nvme", 4) == 0 && ns &&
                strtol(ns + 1, 0, 10) == (long)h->nvme_ns)
                better = true;
        }
        if (better) {
            exact = d;
            nexact++;
        }
    }
    if (count == 1)
        return found;
    if (nexact == 1)
        return exact;
    return -1;
}

/* Does this partition carry the label being looked for? The default
 * label answers to both of its spellings (see the top of this file). */
static bool label_is(const part_t *p, const char *label)
{
    if (strcmp(p->label, label) == 0)
        return true;
    if (strcmp(label, DEF_LABEL) == 0 || strcmp(label, OLD_LABEL) == 0)
        return strcmp(p->label, DEF_LABEL) == 0 ||
               strcmp(p->label, OLD_LABEL) == 0;
    return false;
}

/* The LP root on one disk: the partition typed "Linux root (x86-64)",
 * or failing that the one carrying the label - MBR disks have no type
 * GUID to go by. */
static int root_on_disk(int d, const char *label)
{
    for (int i = 0; i < g_nparts; i++)
        if (g_parts[i].disk == d && g_parts[i].gpt && is_ext4(&g_parts[i]) &&
            memcmp(g_parts[i].type, ROOT_X86_64, 16) == 0)
            return i;
    for (int i = 0; i < g_nparts; i++)
        if (g_parts[i].disk == d && is_ext4(&g_parts[i]) &&
            label_is(&g_parts[i], label))
            return i;
    return -1;
}

/* ── mounting ────────────────────────────────────────────────────── */

/* Try to mount `dev` as the root. Read-only first is deliberate: ext4
 * replays its journal at mount time either way, and a root mounted
 * read-write before anything has checked it is a root that a half
 * finished write can make worse. /etc/rc remounts it read-write once it
 * has looked. */
static bool try_mount(const char *dev)
{
    return lp_mount(dev, NEWROOT, "ext4", MS_RDONLY, NULL) == 0;
}

/* ── checking before handing over ────────────────────────────────── */

/* GPT type "EFI system partition", on-disk byte order. */
static const u8 ESP_TYPE[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

/* Run a program from the new root, chrooted into it, and return its
 * exit status (-1 if it could not be run at all). Under "quiet" its
 * output goes nowhere: e2fsck says "clean, 51234/196608 files" on every
 * good boot, and that line is exactly the flash of console text the
 * quiet boot exists to avoid. What matters is the exit status, and that
 * is what the caller acts on - loudly, when it is bad. */
static int run_in_root(char *const argv[])
{
    if (lp_access(argv[0], F_OK) != 0)
        return -1;
    pid_t pid = lp_fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (lp_chroot(NEWROOT) < 0 || lp_chdir("/") < 0)
            lp_exit(127);
        if (g_quiet) {
            long fd = lp_open("/dev/null", O_RDWR, 0);
            if (fd >= 0) {
                lp_dup2((int)fd, 1);
                lp_dup2((int)fd, 2);
            }
        }
        char *env[] = { "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C", NULL };
        /* argv[0] is the path as seen from outside; inside the chroot
         * it is the same path without the NEWROOT prefix. */
        lp_execve(argv[0] + sizeof NEWROOT - 1, argv, env);
        lp_exit(127);
    }
    int st = 0;
    if (lp_waitpid(pid, &st, 0) != pid || !LP_WIFEXITED(st))
        return -1;
    return LP_WEXITSTATUS(st);
}

/* Check the root and the EFI partition before anything writes to them.
 *
 * This is the one moment it can be done properly: the root is mounted,
 * but read-only, and nothing has opened a file on it for writing yet.
 * /etc/rc remounts it read-write a few lines into its run, and from then
 * on e2fsck may only look. (rc's own fsck line checks partition 2 of the
 * boards' cards - on this disk layout that is LP-RECOVERY, not the root.)
 *
 * The checkers are the root's own - Debian's e2fsck and fsck.fat, run in
 * a chroot - because the initramfs holds one program and is compiled
 * into the kernel; carrying a second copy of e2fsprogs there would mean
 * a kernel rebuild for every e2fsprogs update.
 *
 * A power cut is the case this is for. ext4's journal has already been
 * replayed by the mount; -p (preen) then repairs whatever the journal
 * could not, without asking, and does nothing but read the superblock
 * when the filesystem is clean - milliseconds on a normal boot. */
static void check_filesystems(const char *root_dev)
{
    scan();
    int r = -1;
    for (int i = 0; i < g_nparts; i++)
        if (strcmp(g_parts[i].path, root_dev) == 0)
            r = i;

    char e2[] = NEWROOT "/usr/sbin/e2fsck";
    char *e2argv[] = { e2, "-p", (char *)root_dev, NULL };
    int st = run_in_root(e2argv);
    if (st >= 0 && (st & 4)) {
        dprintf(STDERR_FILENO,
            "preinit: ** the root filesystem (%s) has damage e2fsck -p could\n"
            "preinit:    not repair on its own. Starting anyway; restart and\n"
            "preinit:    choose \"LP Recovery\" -> \"Check and repair disks\".\n",
            root_dev);
    } else if (st >= 0 && (st & 2)) {
        /* Repairs made to a mounted root - even a read-only one - leave
         * the kernel's cached view of it stale. e2fsck's own advice, and
         * the only safe one, is to start again. */
        dprintf(STDERR_FILENO, "preinit: the root filesystem was repaired;"
                               " restarting\n");
        lp_sleep_ms(1500);
        lp_reboot(LINUX_REBOOT_CMD_RESTART);
    }

    /* The EFI partition on the same disk: FAT has no journal, and it
     * holds the kernel. -a repairs without asking, -w writes each fix at
     * once. It is not mounted yet (rc mounts it read-only later). */
    if (r < 0)
        return;
    char fat[] = NEWROOT "/usr/sbin/fsck.fat";
    for (int i = 0; i < g_nparts; i++) {
        if (g_parts[i].disk != g_parts[r].disk || !g_parts[i].gpt ||
            memcmp(g_parts[i].type, ESP_TYPE, 16) != 0)
            continue;
        char *fargv[] = { fat, "-a", "-w", g_parts[i].path, NULL };
        run_in_root(fargv);
        break;
    }
}

/* Say what IS there. "no root found" with nothing else is the least
 * useful message a computer can print. */
static void list_partitions(void)
{
    dprintf(STDERR_FILENO, "preinit: the partitions that are here:\n");
    for (int i = 0; i < g_nparts; i++) {
        char u[40] = "";
        if (g_parts[i].gpt)
            guid_str(g_parts[i].uuid, u);
        dprintf(STDERR_FILENO, "preinit:   %-16s %-5s %-12s %-12s %s\n",
                g_parts[i].path, g_parts[i].fs[0] ? g_parts[i].fs : "?",
                g_parts[i].label[0] ? g_parts[i].label : "(no label)",
                g_parts[i].name[0] ? g_parts[i].name : "-", u);
    }
}

int main(void)
{
    /* The kernel gives an initramfs an empty /dev, so devtmpfs has to be
     * mounted before any disk can be opened by name. /proc is needed to
     * read the command line, /sys to enumerate the disks. */
    lp_mkdir("/proc", 0555);
    lp_mkdir("/sys",  0555);
    lp_mkdir("/dev",  0755);
    lp_mkdir(NEWROOT, 0755);

    lp_mount("proc",     "/proc", "proc",     0, NULL);
    lp_mount("sysfs",    "/sys",  "sysfs",    0, NULL);
    lp_mount("devtmpfs", "/dev",  "devtmpfs", 0, NULL);

    g_quiet = cmdline_has("quiet");

    char want[128];
    root_from_cmdline(want, sizeof want);

    bool mounted = false;
    char chosen[64] = "";

    /* 1. root=/dev/something - take it at its word. */
    if (strncmp(want, "/dev/", 5) == 0) {
        sayf2("root= names ", want);
        for (int wait = 0; wait < ROUNDS && !mounted; wait++) {
            if (try_mount(want)) {
                mounted = true;
                strlcpy(chosen, want, sizeof chosen);
            } else {
                lp_sleep_ms(ROUND_MS);
            }
        }
        if (!mounted)
            give_up("the root named on the kernel command line is not there",
                    want);
    }

    /* 2. root=PARTUUID=x - exact, and never second-guessed. */
    if (!mounted && strncmp(want, "PARTUUID=", 9) == 0) {
        const char *uuid = want + 9;
        sayf2("looking for the root with PARTUUID ", uuid);
        for (int round = 0; round < ROUNDS && !mounted; round++) {
            scan();
            for (int i = 0; i < g_nparts && !mounted; i++) {
                if (!partuuid_is(&g_parts[i], uuid))
                    continue;
                if (try_mount(g_parts[i].path)) {
                    mounted = true;
                    strlcpy(chosen, g_parts[i].path, sizeof chosen);
                }
            }
            if (!mounted) {
                if (round == 20)
                    say("still looking - a USB or NVMe disk can take a"
                        " few seconds to appear");
                lp_sleep_ms(ROUND_MS);
            }
        }
        if (!mounted) {
            list_partitions();
            give_up("the partition root=PARTUUID= names is not here",
                    uuid);
        }
    }

    /* 2b. root=PARTLABEL=x - a name every LP disk may share, so the
     * boot disk's partition first, then a unique one. */
    if (!mounted && strncmp(want, "PARTLABEL=", 10) == 0) {
        const char *name = want + 10;
        sayf2("looking for the partition named ", name);
        hint_t hint;
        bool have_hint = read_hint(&hint);
        for (int round = 0; round < ROUNDS && !mounted; round++) {
            scan();
            int d = have_hint ? boot_disk(&hint) : -1;
            int pick = -1, n = 0;
            for (int i = 0; i < g_nparts; i++) {
                if (!g_parts[i].gpt || strcmp(g_parts[i].name, name) != 0)
                    continue;
                if (d >= 0 && g_parts[i].disk == d) {
                    pick = i;           /* on the boot disk: settled */
                    n = 1;
                    break;
                }
                if (pick < 0)
                    pick = i;
                n++;
            }
            /* With a hint, wait for the boot disk rather than take the
             * first disk that happens to carry the name - unless the
             * wait is nearly over and there is exactly one. */
            bool settle = n == 1 && (d >= 0 || !have_hint ||
                                     round >= ROUNDS / 2);
            if (settle && try_mount(g_parts[pick].path)) {
                mounted = true;
                strlcpy(chosen, g_parts[pick].path, sizeof chosen);
                break;
            }
            if (n > 1 && d < 0 && (!have_hint || round >= ROUNDS / 2)) {
                list_partitions();
                give_up("more than one disk has a partition with this name,"
                        " and nothing says which one to use", name);
            }
            if (round == 20)
                say("still looking - a USB or NVMe disk can take a"
                    " few seconds to appear");
            lp_sleep_ms(ROUND_MS);
        }
        if (!mounted) {
            list_partitions();
            give_up("the partition root=PARTLABEL= names is not here", name);
        }
    }

    /* 3 and 4. The disk the firmware booted, then a unique label. */
    const char *label = DEF_LABEL;
    if (!mounted && strncmp(want, "LABEL=", 6) == 0)
        label = want + 6;

    if (!mounted) {
        hint_t hint;
        bool have_hint = read_hint(&hint);
        if (have_hint)
            sayf2("the firmware started ", hint.entry);

        for (int round = 0; round < ROUNDS && !mounted; round++) {
            scan();

            if (have_hint) {
                int d = boot_disk(&hint);
                int r = d >= 0 ? root_on_disk(d, label) : -1;
                if (r >= 0 && try_mount(g_parts[r].path)) {
                    mounted = true;
                    strlcpy(chosen, g_parts[r].path, sizeof chosen);
                    sayf2("booted from ", g_disks[d].path);
                    break;
                }
                /* The boot disk may still be on its way. Keep looking
                 * for it, rather than settling for a root elsewhere. */
            } else {
                /* No firmware to ask: the old rule, one label. */
                int first = -1, n = 0;
                for (int i = 0; i < g_nparts; i++)
                    if (is_ext4(&g_parts[i]) &&
                        label_is(&g_parts[i], label)) {
                        if (first < 0) first = i;
                        n++;
                    }
                if (n == 1 && try_mount(g_parts[first].path)) {
                    mounted = true;
                    strlcpy(chosen, g_parts[first].path, sizeof chosen);
                    break;
                }
                if (n > 1)
                    break;
            }
            if (round == 20)
                say("still looking - a USB or NVMe disk can take a"
                    " few seconds to appear");
            lp_sleep_ms(ROUND_MS);
        }

        if (!mounted) {
            /* The firmware's answer led nowhere - an entry this program
             * cannot read, or a kernel on one disk and its root on
             * another. One partition with the label is still an answer;
             * two is a question only a person can settle. */
            scan();
            int first = -1, n = 0;
            for (int i = 0; i < g_nparts; i++)
                if (is_ext4(&g_parts[i]) && label_is(&g_parts[i], label)) {
                    if (first < 0) first = i;
                    n++;
                }
            if (n == 1 && try_mount(g_parts[first].path)) {
                mounted = true;
                strlcpy(chosen, g_parts[first].path, sizeof chosen);
                if (have_hint)
                    say("could not tell which disk the firmware booted;"
                        " using the only root there is");
            } else if (n > 1) {
                list_partitions();
                give_up("more than one disk holds an LP root, and nothing"
                        " says which one this kernel belongs to",
                        "add root=PARTUUID=<uuid> to the boot entry, or"
                        " unplug the other disk");
            }
        }
    }

    if (!mounted) {
        if (g_nparts == 0)
            give_up("no disk with a partition on it was found at all",
                    "the kernel may be missing the driver for this"
                    " machine's disk controller");
        list_partitions();
        give_up("none of them is the root", "none carries the expected label");
    }

    sayf2("root is ", chosen);

    /* Take the kernel filesystems across. Mounting them again on the
     * other side would work too, but moving them keeps anything already
     * open on them - and it is one syscall instead of three plus the
     * unmounts. */
    lp_mkdir(NEWROOT "/proc", 0555);
    lp_mkdir(NEWROOT "/sys",  0555);
    lp_mkdir(NEWROOT "/dev",  0755);
    lp_mount("/proc", NEWROOT "/proc", NULL, MS_MOVE, NULL);
    lp_mount("/sys",  NEWROOT "/sys",  NULL, MS_MOVE, NULL);
    lp_mount("/dev",  NEWROOT "/dev",  NULL, MS_MOVE, NULL);

    /* With /dev and /proc now inside the new root, its own checkers can
     * run there. The recovery system checks its own disks, from its
     * menu, and is left alone here. */
    if (!cmdline_has("lp.mode=recovery"))
        check_filesystems(chosen);

    /* The handover. After the chroot there is no way back, so anything
     * that could fail has already been done. */
    if (lp_chdir(NEWROOT) < 0)
        give_up("cannot enter the root that was just mounted", NEWROOT);
    if (lp_mount(".", "/", NULL, MS_MOVE, NULL) != 0)
        give_up("cannot move the new root over the old one",
                "the kernel refused MS_MOVE - is this really an initramfs?");
    if (lp_chroot(".") < 0)
        give_up("cannot chroot into the new root", NULL);
    if (lp_chdir("/") < 0)
        give_up("cannot enter / after the chroot", NULL);

    /* And hand over. init keeps pid 1, which is the whole point of
     * exec rather than fork. */
    static const char *INITS[] = {
        "/sbin/init", "/bin/init", "/init", "/usr/sbin/init", NULL
    };
    extern char **environ;
    for (int i = 0; INITS[i]; i++) {
        char *argv[] = { (char *)INITS[i], NULL };
        lp_execve(INITS[i], argv, environ);
    }

    give_up("the root filesystem has no init on it",
            "looked for /sbin/init, /bin/init, /init and /usr/sbin/init");
    return 1;                    /* not reached */
}
