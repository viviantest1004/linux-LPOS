/* help - list every command on the system.
 *
 *   help            everything, grouped
 *   help <name>     what one command does, and how to run it
 *   help -a         plain list, one name per line (for scripts)
 *
 * This scans the directories in PATH rather than printing a fixed list,
 * so a program dropped into /data/bin shows up here without anyone
 * editing this file. Ones we know about get a description; the rest are
 * listed under "other" so they are at least visible.
 */
#include "types.h"
#include "osname.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

/* linux_dirent64 offsets (same as ls.c) */
#define DIRENT_RECLEN 16
#define DIRENT_TYPE   18
#define DIRENT_NAME   19
#define DT_DIR        4

#define MAX_CMDS   256
#define NAME_MAX   64

typedef struct {
    const char *name;
    const char *group;
    const char *what;
    const char *usage;
} entry_t;

/* Groups are printed in this order. */
static const char *GROUPS[] = {
    "shell", "files", "text", "time", "system", "network", "python", NULL
};

static const entry_t KNOWN[] = {
    /* shell builtins - these live in sh, not on disk */
    { "cd",       "shell",   "change directory",            "cd [dir]" },
    { "pwd",      "shell",   "print the current directory", "pwd" },
    { "echo",     "shell",   "print the arguments",         "echo [-n] [-e] ..." },
    { "env",      "shell",   "list environment variables",  "env" },
    { "exit",     "shell",   "leave the shell",             "exit [code]" },
    { "reboot",   "shell",   "restart the machine",         "reboot" },
    { "poweroff", "shell",   "shut the machine down",       "poweroff" },
    { "help",     "shell",   "this list",                   "help [command]" },
    { "test",     "shell",   "ask about a file or a string","test -f <path> && ..." },
    { "true",     "shell",   "succeed",                     "true" },
    { "false",    "shell",   "fail",                        "false" },
    { "if",       "shell",   "branch on a command's result", "if <cmd> ; then ... ; else ... ; fi" },
    { "while",    "shell",   "repeat while a command works","while <cmd> ; do ... ; done" },
    { "break",    "shell",   "leave a loop",                "break [n]" },
    { "continue", "shell",   "go to the next round of a loop", "continue [n]" },
    { "for",      "shell",   "repeat over a list",          "for x in a b c ; do ... ; done" },
    { "read",     "shell",   "read a line into a variable", "read [-p prompt] name..." },
    { "shift",    "shell",   "drop the first argument",     "shift [n]" },
    { "export",   "shell",   "assign; everything here is already exported", "export NAME=value" },

    { "ls",       "files",   "list a directory",            "ls [-lahtSrRd1] [path]..." },
    { "cp",       "files",   "copy files",                  "cp [-r] [-n] [-q] <src>... <dst>" },
    { "mv",       "files",   "move or rename",              "mv <src>... <dst>" },
    { "rm",       "files",   "delete files",                "rm [-r] [-f] <path>..." },
    { "mkdir",    "files",   "create directories",          "mkdir [-p] <path>..." },
    { "touch",    "files",   "create an empty file",        "touch <file>..." },
    { "mount",    "files",   "mount a filesystem",          "mount [-t type] <dev> <dir>" },
    { "umount",   "files",   "unmount a filesystem",        "umount <dir>" },
    { "dd",       "files",   "copy blocks, exactly as told",  "dd if=<file> of=<file> [bs=N] [count=N]" },
    { "expandfs", "files",   "grow /data to fill the card", "expandfs [disk part]" },
    { "disk",     "files",   "what storage is attached",    "disk [device]" },
    { "part",     "files",   "change the partition table",  "part <disk> [new|del|type|boot]" },
    { "datadisk", "files",   "choose which partition is /data", "datadisk [<part>|--format]" },
    { "storage",  "files",   "what survives a reboot, and adding to it",
                                                            "storage [adopt|forget|format] ..." },
    { "automount","files",   "mount drives as they are plugged in",
                                                            "automount [-d] [-l] [-u <name>]" },
    { "fsck",     "files",   "check and repair /data",      "fsck [-f] <device>..." },
    { "tar",      "files",   "make and open archives",      "tar -c|-t|-x <archive> ..." },
    { "find",     "files",   "walk a directory tree",       "find [path] [-name pat] [-type f|d]" },
    { "du",       "files",   "how much space it takes",     "du [-s] [-b] [path]..." },
    { "chmod",    "files",   "change what may be done",     "chmod 755|+x|-w <file>..." },
    { "chown",    "files",   "change who owns a file",      "chown [-R] <user>[:<group>] <file>..." },
    { "chgrp",    "files",   "change the group",            "chgrp [-R] <group> <file>..." },
    { "ln",       "files",   "another name for a file",     "ln [-s] <target> <name>" },
    { "basename", "files",  "the last part of a path",     "basename <path> [suffix]" },
    { "dirname",  "files",   "the path without its last part", "dirname <path>..." },
    { "stat",     "files",   "what a file is",              "stat <path>..." },
    { "chattr",   "files",   "flags root has to undo first","chattr +i|+a <path>..." },
    { "lsattr",   "files",   "show those flags",            "lsattr <path>..." },

    { "cat",      "text",    "print a file",                "cat [file]..." },
    { "edit",     "text",    "edit a file on screen",       "edit <file>" },
    { "more",     "text",    "read it a screen at a time",  "more [file]..." },
    { "grep",     "text",    "print the lines that match",  "grep [-invclqrEF] [-A n] [-B n] [-C n] <pattern> [file|dir]..." },
    { "head",     "text",    "the first lines",             "head [-n count] [file]..." },
    { "tail",     "text",    "the last lines",              "tail [-n count] [-f] [file]" },
    { "wc",       "text",    "count lines, words, characters", "wc [-l] [-w] [-c] [file]..." },
    { "sort",     "text",    "put lines in order",          "sort [-r] [-n] [-u] [file]..." },
    { "uniq",     "text",    "collapse repeated lines",     "uniq [-c] [-d] [file]" },
    { "cut",      "text",    "take columns out of lines",   "cut -f 1,3 [-d :|-w] [file]" },
    { "tee",      "text",    "write to a file and pass on", "... | tee [-a] <file>..." },
    { "awk",      "text",    "a program per line, over columns", "awk [-F sep] [-v x=1] '<program>' [file]..." },
    { "sed",      "text",    "edit a stream of text",       "sed [-nEi] [-e script] [-f file] '<script>' [file]..." },
    { "tr",       "text",    "replace, squeeze or delete characters", "tr [-cds] set1 [set2]" },
    { "rev",      "text",    "reverse each line",           "rev [file]..." },
    { "nl",       "text",    "number the lines",            "nl [-b a] [-w n] [-s sep] [file]..." },
    { "cmp",      "text",    "where two files start to differ", "cmp [-s] [-l] a b" },
    { "diff",     "text",    "what changed between two files", "diff [-uqiw] [-U n] a b   |   diff -r dir1 dir2" },

    { "seq",      "text",    "print a run of numbers",      "seq [-s sep] [-w] [first [step]] last" },
    { "printf",   "text",    "print with a format, from a script", "printf FORMAT [argument]..." },
    { "yes",      "text",    "print the same thing until stopped", "yes [text]..." },

    { "date",     "time",    "show or set the clock",       "date [-u|-e|-s TIME|-z ZONE]" },
    { "ntp",      "time",    "set the clock from the net",  "ntp [-r|-d] [server]" },

    { "top",      "system",  "what is running, and stop it","top [-1] [-n count]" },
    { "ps",       "system",  "what is running, once",       "ps [-l]" },
    { "df",       "system",  "how full each filesystem is", "df [-b]" },
    { "free",     "system",  "how much memory is left",     "free [-b]" },
    { "usage",    "system",  "memory and disk at a glance", "usage" },
    { "clear",    "system",  "wipe the screen",             "clear" },
    { "reset",    "system",  "put the terminal back together","reset" },
    { "run",      "system",  "run an ordinary Linux binary","run <program> [args...]" },
    { "dropprivs","system",  "run something as not-root",   "dropprivs 1000 <program> ..." },
    { "uname",    "system",  "what this system is",         "uname [-a] [-r] [-m]" },
    { "hostname", "system",  "what this machine calls itself","hostname [name]" },
    { "uptime",   "system",  "how long it has run, and load","uptime" },
    { "whoami",   "system",  "which user this is",          "whoami" },
    { "id",       "system",  "user and group, by number and name", "id [user]" },
    { "groups",   "system",  "which group",                 "groups [user]" },
    { "useradd",  "system",  "make a user",                 "useradd [-u uid] [-d home] <name>" },
    { "userdel",  "system",  "remove one",                  "userdel <name>" },
    { "su",       "system",  "run something as another user", "su <user> [-c <program>]" },
    { "sudo",     "system",  "run something as root",       "sudo <program> [args...]" },
    { "service",  "system",  "what init keeps alive",       "service [start|stop|restart|status <name>]" },
    { "sha256sum","system",  "the checksum of a file",      "sha256sum [-c SUMS] <file>..." },
    { "md5sum",   "system",  "the same, in MD5",            "md5sum [-c SUMS] <file>..." },
    { "sha1sum",  "system",  "the same, in SHA-1",          "sha1sum [-c SUMS] <file>..." },
    { "sha512sum","system",  "the same, in SHA-512",        "sha512sum [-c SUMS] <file>..." },
    { "cksum",    "system",  "the POSIX checksum and byte count", "cksum <file>..." },
    { "sum",      "system",  "the old BSD checksum",        "sum [-r|-s] <file>..." },
    { "defend",   "system",  "watch for what happens to a board on the internet", "defend [-d] | status | baseline | unban <addr>" },
    { "integrity","system",  "has anything persistent changed","integrity [-c] [-u] [-l]" },
    { "kill",     "system",  "stop a process, by pid or name", "kill [-9] <pid|name>..." },
    { "sleep",    "system",  "wait",                        "sleep <seconds>" },
    { "watchdog", "system",  "reboot the board if it hangs","watchdog [-t s] [-x]" },
    { "logd",     "system",  "collect logs to /data/log",   "(started by init)" },
    { "dmesg",    "system",  "the kernel's own log",        "dmesg [-n]" },
    { "info",     "system",  "everything about this machine",  "info [-s] [os|cpu|mem|disk|net|kernel|project]" },
    { "sysinfo",  "system",  "memory, CPU, disks, network", "sysinfo" },
    { "zram",     "system",  "compressed swap in RAM",      "zram on|off|status" },
    { "guard",    "system",  "the safety net (memory, heat, power, CPU)",
                                                             "guard [-d]" },
    { "bootcount","system",  "detect a reboot loop",        "bootcount" },
    { "beacon",   "system",  "report how the board is doing", "beacon [-d] [-n]" },
    { "which",    "system",  "where a command would come from", "which [-a] <name>..." },
    { "xargs",    "system",  "turn lines into arguments",   "... | xargs [-0trn N] [-I S] <command>" },
    { "calc",     "system",  "integer calculator",          "calc \"1 + 2 * 3\"" },
    { "apt",      "system",  "install packages from Debian",
                                                            "apt install|remove|search|run|shell <...>" },
    { "pkg",      "system",  "install and remove packages", "pkg list|add|remove|install" },
    { "update",   "system",  "replace the system, reversibly", "update [<file|url>|--rollback]" },
    { "splash",   "system",  "draw the boot screen",        "splash [device]" },

    { "dhcp",     "network", "get an address, and keep it", "dhcp [-d] <interface>" },
    { "ipconfig", "network", "a fixed address from a file", "ipconfig [-s] <file>" },
    { "net",      "network", "set it up, and say where it broke", "net [test|scan|wifi|dhcp|static|dns]" },
    { "ping",     "network", "is it there, and how far",    "ping [-c count] <host>" },
    { "ifconfig", "network", "look at or set an interface", "ifconfig [<if> [up|down|<address>]]" },
    { "route",    "network", "where packets go",            "route [add default gw <address>]" },
    { "nslookup", "network", "what address a name has",     "nslookup <name>..." },
    { "wget",     "network", "download a file",             "wget <url> [file]" },
    { "wpa_supplicant", "network", "join a WiFi network",   "wpa_supplicant -B -i wlan0 -c <conf>" },
    { "wpa_cli",  "network", "talk to wpa_supplicant",      "wpa_cli" },
    { "dropbear", "network", "the SSH server",              "(started by init)" },
    { "dropbearkey", "network", "make an SSH host key",     "dropbearkey -t ed25519 -f <file>" },
    { "authkey",  "network", "make and keep a way in over SSH", "authkey new | add [file] | -l" },
    { "netstat",  "network", "what is listening, and connected", "netstat [-latunp] [-r] [-i] [-s]" },
    { "httpd",    "network", "serve a directory over http",  "httpd [-p port] [-d dir] [-i addr] [-f]" },
    { "firewall", "network", "which ports are open",        "firewall [on|strict|off]" },

    { "python",   "python",  "CPython 3.12",                "python [file]" },
    { "python3",  "python",  "the same as python",          "python3 [file]" },
    { "micropython", "python", "MicroPython - small, fast", "micropython [file]" },
    /* ── added so that what is learned here is what works elsewhere ── */
    { "readlink", "files",   "what a symlink points at",    "readlink [-f] <path>..." },
    { "realpath", "files",   "the absolute path, links followed", "realpath [-e|-m] <path>..." },
    { "link",     "files",   "one more name for a file",    "link <file> <newname>" },
    { "unlink",   "files",   "remove one name",             "unlink <file>" },
    { "mkfifo",   "files",   "a named pipe",                "mkfifo [-m mode] <name>..." },
    { "mknod",    "files",   "a device node",               "mknod <name> b|c|p [major minor]" },
    { "truncate", "files",   "make a file exactly this big", "truncate -s SIZE <file>..." },
    { "shred",    "files",   "overwrite a file before removing it", "shred [-u] [-n N] <file>..." },
    { "install",  "files",   "copy, and set the mode in one step", "install [-m MODE] <src> <dst>" },
    { "pathchk",  "files",   "is this path usable",         "pathchk [-p] <path>..." },
    { "mktemp",   "files",   "a name nothing else has",     "mktemp [-d] [TEMPLATE]" },
    { "split",    "files",   "one file into several",       "split [-l N|-b SIZE] <file> [prefix]" },
    { "csplit",   "files",   "split where a pattern matches", "csplit <file> PATTERN..." },
    { "gzip",     "files",   "compress; gunzip and zcat undo it", "gzip [-d] [-c] [-k] <file>..." },
    { "tac",      "text",    "cat backwards, last line first", "tac [file]..." },
    { "fold",     "text",    "break long lines to fit",     "fold [-w N] [-s] [file]..." },
    { "fmt",      "text",    "reflow paragraphs",           "fmt [-w N] [file]..." },
    { "paste",    "text",    "lines side by side",          "paste [-d LIST] [-s] <file>..." },
    { "comm",     "text",    "what two sorted files share", "comm [-123] <file1> <file2>" },
    { "join",     "text",    "join two sorted files on a field", "join [-1 F] [-2 F] <a> <b>" },
    { "expand",   "text",    "tabs to spaces",              "expand [-t N] [file]..." },
    { "unexpand", "text",    "spaces to tabs",              "unexpand [-a] [file]..." },
    { "tsort",    "text",    "put dependencies in order",   "tsort [file]" },
    { "shuf",     "text",    "the same lines, shuffled",    "shuf [-n N] [-e ARG...] [file]" },
    { "expr",     "text",    "arithmetic and string tests", "expr EXPRESSION" },
    { "factor",   "text",    "the prime factors of a number", "factor [NUMBER]..." },
    { "numfmt",   "text",    "numbers to and from human sizes", "numfmt --to=iec [NUMBER]..." },
    { "base64",   "text",    "text that survives anything", "base64 [-d] [file]" },
    { "od",       "text",    "the bytes, in a base you can read", "od [-c] [-tx1] [file]" },
    { "xxd",      "text",    "a hex dump you can edit and reverse", "xxd [-r] [-p] [file]" },
    { "strings",  "text",    "the readable text inside a binary", "strings [-n N] <file>..." },
    { "column",   "text",    "line things up",              "column [-t] [-s SEP] [file]" },
    { "env",      "shell",   "run with a changed environment", "env [NAME=VALUE]... [command]" },
    { "printenv", "shell",   "one variable, or all of them", "printenv [NAME]..." },
    { "pwd",      "shell",   "where we are",                "pwd [-P]" },
    { "test",     "shell",   "the conditions a script asks", "test EXPR   or   [ EXPR ]" },
    { "true",     "shell",   "succeed, and do nothing else", "true" },
    { "false",    "shell",   "fail, and do nothing else",   "false" },
    { "timeout",  "shell",   "run it, and stop it if it takes too long", "timeout DURATION command..." },
    { "nohup",    "shell",   "keep running after the terminal goes", "nohup command..." },
    { "nice",     "shell",   "run at a lower priority",     "nice [-n N] command..." },
    { "watch",    "shell",   "run it over and over, show the latest", "watch [-n SECS] command..." },
    { "hwclock",  "time",    "the clock that runs while the power is off", "hwclock [-r|-w|-s]" },
    { "crontab",  "time",    "what to run, and when",       "crontab [-l|-e|-r] [file]" },
    { "pgrep",    "system",  "find processes by name",      "pgrep [-l] [-f] PATTERN" },
    { "pkill",    "system",  "signal them",                 "pkill [-SIGNAL] PATTERN" },
    { "killall",  "system",  "signal them, by exact name",  "killall [-SIGNAL] NAME..." },
    { "pmap",     "system",  "what a process has mapped",   "pmap [-x] PID" },
    { "sysctl",   "system",  "read and set kernel tunables", "sysctl [-a] [NAME[=VALUE]]" },
    { "lscpu",    "system",  "what the processor is",       "lscpu" },
    { "lsblk",    "system",  "the block devices",           "lsblk" },
    { "blkid",    "system",  "label, UUID and type of a filesystem", "blkid [device]" },
    { "nproc",    "system",  "how many processors",         "nproc" },
    { "arch",     "system",  "the machine name",            "arch" },
    { "hostid",   "system",  "this machine's 32-bit id",    "hostid" },
    { "getconf",  "system",  "system configuration values", "getconf [-a] [NAME]" },
    { "logname",  "system",  "the login name",              "logname" },
    { "users",    "system",  "who is logged in",            "users" },
    { "tty",      "system",  "the name of this terminal",   "tty [-s]" },
    { "sync",     "system",  "write everything out to disk", "sync" },
    { "passwd",   "system",  "change a password",           "passwd [user]" },
    { "voice",    "system",  "GNU's wording, or this system's own", "voice [gnu|lp]" },
    { "temp",     "system",  "how hot the board is",        "temp [-a] [-c] [-w [secs]]" },
    { "wifi",     "network", "why the wireless will not connect, and what works",
                                                 "wifi [fix [-k] | log]" },
    { "nc",       "network", "connect or listen on a socket", "nc [-l] [-p PORT] [host] [port]" },

    { NULL, NULL, NULL, NULL }
};

static const entry_t *lookup(const char *name)
{
    for (int i = 0; KNOWN[i].name; i++)
        if (strcmp(KNOWN[i].name, name) == 0)
            return &KNOWN[i];
    return NULL;
}

/* Names found on disk, so we can report ones we have no description for. */
static char found[MAX_CMDS][NAME_MAX];
static int  nfound = 0;

static bool already_found(const char *name)
{
    for (int i = 0; i < nfound; i++)
        if (strcmp(found[i], name) == 0)
            return true;
    return false;
}

static void scan_dir(const char *dir)
{
    long fd = lp_open(dir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;                     /* not every PATH entry exists */

    char buf[8192];
    for (;;) {
        long n = sys_getdents((int)fd, buf, sizeof(buf));
        if (n <= 0)
            break;

        for (long off = 0; off < n; ) {
            char       *rec  = buf + off;
            u16         len  = *(u16 *)(rec + DIRENT_RECLEN);
            u8          type = *(u8 *)(rec + DIRENT_TYPE);
            const char *name = rec + DIRENT_NAME;
            off += len;

            if (name[0] == '.' || type == DT_DIR)
                continue;
            if (nfound >= MAX_CMDS || already_found(name))
                continue;
            strlcpy(found[nfound++], name, NAME_MAX);
        }
    }
    lp_close((int)fd);
}

static void scan_path(void)
{
    const char *path = getenv("PATH");
    if (!path)
        path = "/bin:/data/bin:/sbin:/usr/bin:/usr/sbin";

    char dir[256];
    while (*path) {
        size_t i = 0;
        while (*path && *path != ':' && i < sizeof(dir) - 1)
            dir[i++] = *path++;
        dir[i] = '\0';
        if (i)
            scan_dir(dir);
        if (*path == ':')
            path++;
    }
}

/* Is this name a program we actually found, or a shell builtin? */
static bool available(const entry_t *e)
{
    if (strcmp(e->group, "shell") == 0)
        return true;                /* builtins are always there */
    return already_found(e->name);
}

/* Everything below goes through the pager rather than printf.
 *
 * This list is longer than the screen, and a screen on this board has no
 * scrollback - what scrolls off is gone. Printing it all at once means
 * the top half can never be read. Piped or redirected it prints straight
 * through, so "help | grep zram" and "help > list.txt" work as expected. */
static bool print_one(const entry_t *e)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "  %-14s %s", e->name, e->what);
    return page_line(buf);
}

static void print_all(void)
{
    page_begin();

    if (!page_line(LP_OS_NAME " OS commands")) goto done;

    for (int g = 0; GROUPS[g]; g++) {
        bool header = false;
        for (int i = 0; KNOWN[i].name; i++) {
            if (strcmp(KNOWN[i].group, GROUPS[g]) != 0)
                continue;
            if (!available(&KNOWN[i]))
                continue;           /* python may not be installed */
            if (!header) {
                if (!page_line("")) goto done;
                if (!page_line(GROUPS[g])) goto done;
                header = true;
            }
            if (!print_one(&KNOWN[i])) goto done;
        }
    }

    /* Anything on disk we have no description for. A new program shows
     * up here the moment it is copied in, which beats it being invisible. */
    bool other = false;
    for (int i = 0; i < nfound; i++) {
        if (lookup(found[i]))
            continue;
        if (!other) {
            if (!page_line("")) goto done;
            if (!page_line("other")) goto done;
            other = true;
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "  %s", found[i]);
        if (!page_line(buf)) goto done;
    }

    if (!page_line("")) goto done;
    if (!page_line("  help <command>   how to use one of them")) goto done;
    if (!page_line("  <command> -h     most of them explain themselves too")) goto done;
    if (!page_line("")) goto done;
    if (!page_line("The shell takes < > >> for redirection, | for pipes,")) goto done;
    if (!page_line("and && || ; between commands. if, while and for work,")) goto done;
    if (!page_line("with test to ask about files. Tab completes, the arrow")) goto done;
    if (!page_line("keys go back through what you have typed, * matches file")) goto done;
    if (!page_line("names, NAME=value sets a variable and & backgrounds.")) goto done;
    if (!page_line("")) goto done;
    if (!page_line("Files under /data and /root survive a reboot.")) goto done;
    if (!page_line("Everything else is in RAM and does not.")) goto done;

done:
    page_end();
}

int main(int argc, char **argv)
{
    scan_path();

    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        /* Plain list, for scripting or just piping into something. */
        for (int i = 0; KNOWN[i].name; i++)
            if (strcmp(KNOWN[i].group, "shell") == 0)
                printf("%s\n", KNOWN[i].name);
        for (int i = 0; i < nfound; i++)
            printf("%s\n", found[i]);
        return 0;
    }

    if (argc > 1) {
        const entry_t *e = lookup(argv[1]);
        if (!e) {
            if (already_found(argv[1])) {
                printf("%s\n", argv[1]);
                printf("  No description here. Try: %s -h\n", argv[1]);
                return 0;
            }
            dprintf(STDERR_FILENO,
                    "help: no command called '%s'\n"
                    "      run 'help' to see what there is\n", argv[1]);
            return 1;
        }
        printf("%s - %s\n\n", e->name, e->what);
        printf("  usage: %s\n", e->usage);
        if (!available(e))
            printf("\n  Not installed on this system.\n");
        return 0;
    }

    print_all();
    return 0;
}
