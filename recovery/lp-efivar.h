/* lp-efivar.h - read and write LP's boot-menu variables through efivarfs.
 *
 * The boot menu (boot/efi/lpboot.c) keeps its state in three UEFI
 * variables under LP's vendor GUID, and two programs on the Linux side
 * change them: the recovery menu (recovery/lp-recovery.c) and
 * lp-reboot-recovery (userland/lp-reboot-recovery). This is the one copy
 * of how, on our libc, header-only so both can include it.
 *
 *   LPBootCount  u32   normal boots started and not yet confirmed; 0 after
 *                      a good boot, >= 100 while a reinstall is unfinished
 *   LPBootNext   text  "recovery": the next boot goes to LP Recovery, once
 *   LPLang       text  "ko" or "en": the boot menu's language
 *
 * efivarfs has two rules that make a naive write fail:
 *   - a file's contents are four bytes of attributes followed by the
 *     data, and the kernel wants both in ONE write();
 *   - every variable it does not know is created immutable (so a stray
 *     rm -rf cannot brick a machine by deleting firmware variables), and
 *     an immutable file cannot be rewritten or removed. The flag has to
 *     come off (FS_IOC_SETFLAGS) before either, and only for our own
 *     variable.
 */
#ifndef LP_EFIVAR_H
#define LP_EFIVAR_H

#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define LP_EFI_GUID    "8accd20f-82a3-4c56-b45f-f3cd7ab7a4b0"
#define LP_EFIVARS     "/sys/firmware/efi/efivars"
#define LP_EFI_ATTRS   7u      /* non-volatile, boot service, runtime */
#define LP_BOOT_REINSTALL_MARK 100u

#define LP_FS_IOC_GETFLAGS LP_IOR('f', 1, long)
#define LP_FS_IOC_SETFLAGS LP_IOW('f', 2, long)
#define LP_FS_IMMUTABLE_FL 0x10

/* efivarfs mounted where it belongs; false on a machine not booted by
 * UEFI (nothing to do there). */
static inline bool lp_efivars_ready(void)
{
    if (!lp_is_dir("/sys/firmware/efi"))
        return false;
    /* A mounted efivarfs is never empty (it has at least the firmware's
     * own variables); an empty directory means it is not mounted yet. */
    long fd = lp_open(LP_EFIVARS, O_RDONLY | O_DIRECTORY, 0);
    if (fd >= 0) {
        char buf[512];
        long n = sys_getdents((int)fd, buf, sizeof buf);
        lp_close((int)fd);
        if (n > 48)             /* more than "." and ".." */
            return true;
    }
    lp_mount("efivarfs", LP_EFIVARS, "efivarfs", 0, NULL);
    fd = lp_open(LP_EFIVARS, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return false;
    char buf[512];
    long n = sys_getdents((int)fd, buf, sizeof buf);
    lp_close((int)fd);
    return n > 48;
}

static inline void lp_efivar_path(const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s-%s", LP_EFIVARS, name, LP_EFI_GUID);
}

/* The variable's data (attributes stripped); bytes read, or -errno. */
static inline long lp_efivar_read(const char *name, void *buf, size_t cap)
{
    char path[160];
    u8 raw[260];
    lp_efivar_path(name, path, sizeof path);
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;
    long n = lp_read((int)fd, raw, sizeof raw);
    lp_close((int)fd);
    if (n < 4)
        return n < 0 ? n : -22;
    n -= 4;
    if ((size_t)n > cap)
        n = (long)cap;
    memcpy(buf, raw + 4, (size_t)n);
    return n;
}

static inline void lp_efivar_mutable(const char *path)
{
    long fd = lp_open(path, O_RDONLY, 0);
    if (fd < 0)
        return;
    long flags = 0;
    if (lp_ioctl((int)fd, LP_FS_IOC_GETFLAGS, &flags) == 0 && (flags & LP_FS_IMMUTABLE_FL)) {
        flags &= ~(long)LP_FS_IMMUTABLE_FL;
        lp_ioctl((int)fd, LP_FS_IOC_SETFLAGS, &flags);
    }
    lp_close((int)fd);
}

/* Create or replace. 0, or -errno. */
static inline long lp_efivar_write(const char *name, const void *data, size_t n)
{
    char path[160];
    u8 raw[260];
    if (n > sizeof raw - 4)
        return -22;
    lp_efivar_path(name, path, sizeof path);
    if (lp_exists(path))
        lp_efivar_mutable(path);
    u32 attrs = LP_EFI_ATTRS;
    memcpy(raw, &attrs, 4);
    memcpy(raw + 4, data, n);
    long fd = lp_open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
        return fd;
    long w = lp_write((int)fd, raw, n + 4);
    lp_close((int)fd);
    return w == (long)(n + 4) ? 0 : (w < 0 ? w : -5);
}

static inline long lp_efivar_delete(const char *name)
{
    char path[160];
    lp_efivar_path(name, path, sizeof path);
    if (!lp_exists(path))
        return 0;
    lp_efivar_mutable(path);
    return lp_unlink(path);
}

static inline long lp_bootcount_get(void)
{
    u32 v = 0;
    long n = lp_efivar_read("LPBootCount", &v, sizeof v);
    return n == 4 ? (long)v : (n < 0 ? n : 0);
}

static inline long lp_bootcount_set(u32 v)
{
    return lp_efivar_write("LPBootCount", &v, sizeof v);
}

#endif
