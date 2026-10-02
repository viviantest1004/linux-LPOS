/* run - run a program built for an ordinary Linux.
 *
 *   run <program> [arguments...]
 *
 * Every program in this system is statically linked against our own
 * libc, so there is no dynamic loader in the image and a binary built
 * anywhere else fails at execve: the kernel looks for the interpreter
 * named inside it and finds nothing.
 *
 * glibc is on /data, because CPython needs it. This hands a program
 * straight to that loader, which is what /lib/ld-linux-aarch64.so.1
 * would do if it existed:
 *
 *   run ./ripgrep --version
 *
 * /etc/rc makes those /lib symlinks when /data/glibc is present, so
 * most binaries run without this. It is still here for the cases where
 * they do not: a binary that wants libraries somewhere else, or a
 * system where the symlinks were deliberately left out.
 *
 *   run -l /data/lib/mine ./program     look for libraries there too
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define LIBDIR "/data/glibc"

/* ── The loader has a different name on every architecture ────────
 *
 * This was one string, "/data/glibc/ld-linux-aarch64.so.1", written
 * when arm64 was the only target. On the amd64 image it named a file
 * that is not there - the loader is ld-linux-x86-64.so.2 - so `run`
 * could not start anything at all, and said so in a way that sounded
 * like the program was at fault rather than this command.
 *
 * Chosen at compile time, because the binary is built once per
 * architecture and there is nothing to decide at runtime. The search
 * below is for the case this file has not been taught about yet: a new
 * architecture should degrade to looking rather than to failing. */
#if defined(__x86_64__)
#  define LOADER LIBDIR "/ld-linux-x86-64.so.2"
#elif defined(__aarch64__)
#  define LOADER LIBDIR "/ld-linux-aarch64.so.1"
#else
#  define LOADER ""
#endif

/* Whatever loader is actually sitting in a directory.
 *
 * ld-linux-*.so.* is the naming every glibc port uses, so this finds
 * one without knowing the architecture. Used when the compiled-in name
 * is missing - a /data/glibc copied from another machine, say - because
 * "there is a loader here and it is not the one I expected" is a
 * situation worth surviving. */
static bool find_loader(const char *dir, char *out, size_t n)
{
    long fd = lp_open(dir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return false;

    char buf[4096];
    bool found = false;
    long got;

    while (!found && (got = sys_getdents((int)fd, buf, sizeof buf)) > 0) {
        for (long off = 0; off < got; ) {
            /* struct linux_dirent64: ino(8) off(8) reclen(2) type(1) name */
            unsigned short reclen = *(unsigned short *)(buf + off + 16);
            const char *name = buf + off + 19;

            if (strncmp(name, "ld-linux-", 9) == 0 && strstr(name, ".so")) {
                snprintf(out, n, "%s/%s", dir, name);
                found = true;
                break;
            }
            if (reclen == 0)
                break;
            off += reclen;
        }
    }
    lp_close((int)fd);
    return found;
}

int main(int argc, char **argv)
{
    const char *libpath = LIBDIR;
    int first = 1;

    if (argc > 2 && strcmp(argv[1], "-l") == 0) {
        libpath = argv[2];
        first = 3;
    }

    /* The loader this build expects, or whatever one is actually there. */
    static char loader[256];
    strlcpy(loader, LOADER, sizeof loader);
    if (!loader[0] || !lp_exists(loader)) {
        if (!find_loader(libpath, loader, sizeof loader))
            loader[0] = '\0';
    }

    if (argc <= first || strcmp(argv[1], "-h") == 0) {
        printf("usage: run [-l libdir] <program> [arguments...]\n\n");
        printf("Runs a program built for an ordinary Linux, using the\n");
        printf("glibc on /data. Programs built for this system do not\n");
        printf("need it - they have no loader at all.\n\n");
        printf("  loader   %s\n", loader[0] ? loader : "(none found)");
        printf("  libs     %s\n", libpath);
        return argc <= first ? 2 : 0;
    }

    if (!loader[0]) {
        dprintf(STDERR_FILENO,
                "run: no dynamic loader in %s\n"
                "     glibc lives on the data partition and arrives with\n"
                "     CPython. An image without python has no loader, and\n"
                "     programs built elsewhere cannot run on it.\n", libpath);
        return 1;
    }

    /* ld.so takes the program and its arguments after its own. */
    static char *args[64];
    int n = 0;

    args[n++] = loader;
    args[n++] = (char *)"--library-path";
    args[n++] = (char *)libpath;

    for (int i = first; i < argc && n < 63; i++)
        args[n++] = argv[i];
    args[n] = NULL;

    extern char **environ;
    lp_execve(loader, args, environ);

    dprintf(STDERR_FILENO, "run: cannot start %s\n", loader);
    return 127;
}
