/* lp-reboot-recovery - talk to LP's boot menu from the running system.
 *
 *   lp-reboot-recovery              restart into LP Recovery (once)
 *   lp-reboot-recovery --no-reboot  only ask for it; the next restart goes there
 *   lp-reboot-recovery --cancel     forget that request
 *   lp-reboot-recovery --boot-ok    this boot came up: reset the boot counter
 *   lp-reboot-recovery --lang ko|en the boot menu's language
 *   lp-reboot-recovery --status     print the three variables
 *
 * The boot menu (boot/efi/lpboot.c) keeps its state in UEFI variables
 * under LP's GUID; recovery/lp-efivar.h is how this libc reads and
 * writes them.
 *
 * ── --boot-ok, and why the system has to say it ──
 *
 * The menu adds one to LPBootCount before it starts LP. Two or more
 * means the last two starts never got as far as saying they were fine,
 * and the menu then preselects LP Recovery - the way back from a system
 * that dies on the way up. That only works if a system that DID come up
 * says so. Nothing did, at first: every normal boot counted, and the
 * second restart after installing LP went, by itself, to Recovery. /etc/rc
 * now runs `lp-reboot-recovery --boot-ok` once the system is up.
 *
 * A count of 100 or more is not a count: it is the recovery system's
 * mark for "a reinstall started and did not finish", and only the
 * recovery system clears it. --boot-ok leaves it alone.
 *
 * The variable is written only when it is not already 0: firmware
 * variables live in flash, and a write per boot for nothing is wear.
 *
 * ── restarting ──
 *
 * The restart is init's (SIGUSR2, the same as `reboot`): it stops the
 * services, syncs and unmounts before it asks the kernel. Only root may
 * ask; Settings and the power menu go through the root helper, which
 * asks the administrator's password first.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"
#include "../../recovery/lp-efivar.h"

static const char *prog = "lp-reboot-recovery";

static void usage(int fd)
{
    dprintf(fd,
        "usage: %s [--no-reboot | --cancel | --boot-ok | --lang ko|en | --status]\n"
        "\n"
        "With no option: restart into LP Recovery, once. The boot menu\n"
        "offers the same choice at every start.\n"
        "\n"
        "  --no-reboot  the next restart goes to LP Recovery; not now\n"
        "  --cancel     the next restart is an ordinary one after all\n"
        "  --boot-ok    this start came up (run by /etc/rc)\n"
        "  --lang L     the boot menu's language: ko or en\n"
        "  --status     what the boot menu will do next\n", prog);
}

static long read_text(const char *name, char *out, size_t cap)
{
    long n = lp_efivar_read(name, out, cap - 1);
    if (n < 0) {
        out[0] = '\0';
        return n;
    }
    out[n] = '\0';
    /* The menu writes UCS-2-free ASCII; a trailing NUL or newline from
     * another writer is not part of the value. */
    while (n > 0 && (out[n - 1] == '\0' || out[n - 1] == '\n'))
        out[--n] = '\0';
    return n;
}

static int need_root(void)
{
    if (lp_getuid() != 0) {
        dprintf(STDERR_FILENO,
                "%s: only the administrator can change how the machine starts.\n"
                "  sudo %s ...\n", prog, prog);
        return 1;
    }
    return 0;
}

static int need_efi(void)
{
    if (!lp_efivars_ready()) {
        dprintf(STDERR_FILENO,
                "%s: this machine was not started by UEFI, so there is no boot\n"
                "  menu to talk to (LP Recovery is on UEFI PCs only).\n", prog);
        return 1;
    }
    return 0;
}

static int say_error(const char *what, long err)
{
    dprintf(STDERR_FILENO, "%s: could not %s: %s\n", prog, what, lp_strerror((int)-err));
    return 1;
}

static int do_status(void)
{
    if (!lp_efivars_ready()) {
        printf("not started by UEFI - no boot menu\n");
        return 0;
    }
    long c = lp_bootcount_get();
    char next[64], lang[16];
    read_text("LPBootNext", next, sizeof next);
    read_text("LPLang", lang, sizeof lang);
    if (c < 0)
        c = 0;
    printf("boot count:  %ld%s\n", c,
           c >= (long)LP_BOOT_REINSTALL_MARK ? "  (a reinstall did not finish)"
           : c >= 2 ? "  (the menu will suggest Recovery)" : "");
    printf("next start:  %s\n", strcmp(next, "recovery") == 0 ? "LP Recovery" : "LP");
    printf("menu:        %s\n", strcmp(lang, "ko") == 0 ? "Korean" : "English");
    return 0;
}

static int do_boot_ok(void)
{
    /* Quiet by design: rc runs it on every start, including on machines
     * with no UEFI at all (the Pi), where there is nothing to do. */
    if (lp_getuid() != 0 || !lp_efivars_ready())
        return 0;
    long c = lp_bootcount_get();
    if (c <= 0 || c >= (long)LP_BOOT_REINSTALL_MARK)
        return 0;
    long r = lp_bootcount_set(0);
    return r < 0 ? say_error("reset the boot counter", r) : 0;
}

int main(int argc, char **argv)
{
    bool reboot_now = true;

    if (argc > 1) {
        const char *a = argv[1];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(STDOUT_FILENO);
            return 0;
        }
        if (!strcmp(a, "--status"))
            return do_status();
        if (!strcmp(a, "--boot-ok"))
            return do_boot_ok();
        if (!strcmp(a, "--cancel")) {
            if (need_root() || need_efi())
                return 1;
            long r = lp_efivar_delete("LPBootNext");
            return r < 0 ? say_error("cancel the request", r) : 0;
        }
        if (!strcmp(a, "--lang")) {
            if (argc < 3 || (strcmp(argv[2], "ko") && strcmp(argv[2], "en"))) {
                usage(STDERR_FILENO);
                return 2;
            }
            if (need_root() || need_efi())
                return 1;
            long r = lp_efivar_write("LPLang", argv[2], strlen(argv[2]));
            return r < 0 ? say_error("set the menu's language", r) : 0;
        }
        if (!strcmp(a, "--no-reboot")) {
            reboot_now = false;
        } else {
            usage(STDERR_FILENO);
            return 2;
        }
    }

    if (need_root() || need_efi())
        return 1;
    long r = lp_efivar_write("LPBootNext", "recovery", 8);
    if (r < 0)
        return say_error("ask the boot menu for LP Recovery", r);
    if (!reboot_now) {
        printf("The next restart goes to LP Recovery.\n");
        return 0;
    }
    printf("Restarting into LP Recovery...\n");
    lp_sync();
    if (lp_kill(1, SIGUSR2) < 0) {
        dprintf(STDERR_FILENO, "%s: init did not take the restart; run `reboot`.\n", prog);
        return 1;
    }
    return 0;
}
