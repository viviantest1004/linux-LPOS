/* comm - what two sorted files have in common, and what they do not.
 *
 *   comm a b         three columns: only in a, only in b, in both
 *   comm -12 a b     just the common lines
 *
 * Both files have to be sorted, which is the part people forget; when
 * they are not, this says so rather than quietly giving a wrong answer.
 */
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"

static bool show1 = true, show2 = true, show3 = true;
static const char *sep = "\t";
static bool checked = true;

static void emit(int col, const char *s)
{
    if (col == 1 && !show1) return;
    if (col == 2 && !show2) return;
    if (col == 3 && !show3) return;
    int lead = 0;
    if (col >= 2 && show1) lead++;
    if (col >= 3 && show2) lead++;
    for (int i = 0; i < lead; i++) lp_write(STDOUT_FILENO, sep, strlen(sep));
    printf("%s\n", s);
}

int main(int argc, char **argv)
{
    static const lp_lopt_t lo[] = {
        { "check-order", 0, 'C' }, { "nocheck-order", 0, 'N' },
        { "output-delimiter", 1, 'D' }, { "total", 0, 'T' },
        { "zero-terminated", 0, 'z' }, { "help", 0, 'H' }, { 0, 0, 0 }
    };
    lp_getopt_t g;
    lp_getopt_init(&g, argc, argv, "123z", lo);
    for (int c; (c = lp_getopt(&g)) != -1; ) {
        switch (c) {
        case '1': show1 = false; break;
        case '2': show2 = false; break;
        case '3': show3 = false; break;
        case 'C': checked = true; break;
        case 'N': checked = false; break;
        case 'D': sep = g.arg; break;
        case 'T': case 'z': break;
        case 'H':
            printf("Usage: comm [OPTION]... FILE1 FILE2\n"
                   "Compare sorted files FILE1 and FILE2 line by line.\n\n"
                   "  -1               suppress column 1 (lines unique to FILE1)\n"
                   "  -2               suppress column 2 (lines unique to FILE2)\n"
                   "  -3               suppress column 3 (lines that appear in both files)\n"
                   "      --nocheck-order  do not check that the input is correctly sorted\n"
                   "      --output-delimiter=STR  separate columns with STR\n"
                   "      --help     display this help and exit\n");
            return 0;
        default: lp_getopt_err("comm", &g); return 1;
        }
    }

    if (argc - g.ind != 2) {
        dprintf(STDERR_FILENO, "comm: missing operand\n");
        dprintf(STDERR_FILENO, "Try 'comm --help' for more information.\n");
        return 1;
    }

    int fd[2];
    for (int i = 0; i < 2; i++) {
        const char *name = argv[g.ind + i];
        if (strcmp(name, "-") == 0) { fd[i] = STDIN_FILENO; continue; }
        long f = lp_open(name, O_RDONLY, 0);
        if (f < 0) {
            lp_diag("comm", NULL, NULL, "cannot open", name, (int)-f);
            return 2;
        }
        fd[i] = (int)f;
    }

    static char a[65536], b[65536], pa[65536], pb[65536];
    bool have_a = readrec(fd[0], a, sizeof a, '\n', NULL) >= 0;
    bool have_b = readrec(fd[1], b, sizeof b, '\n', NULL) >= 0;
    pa[0] = pb[0] = '\0';
    bool first_a = true, first_b = true, warned = false;

    while (have_a || have_b) {
        int cmp;
        if (!have_b)      cmp = -1;
        else if (!have_a) cmp = 1;
        else              cmp = strcmp(a, b);

        if (cmp < 0) {
            emit(1, a);
            if (checked && !first_a && strcmp(pa, a) > 0 && !warned) {
                dprintf(STDERR_FILENO, "comm: file 1 is not in sorted order\n");
                warned = true;
            }
            strlcpy(pa, a, sizeof pa); first_a = false;
            have_a = readrec(fd[0], a, sizeof a, '\n', NULL) >= 0;
        } else if (cmp > 0) {
            emit(2, b);
            if (checked && !first_b && strcmp(pb, b) > 0 && !warned) {
                dprintf(STDERR_FILENO, "comm: file 2 is not in sorted order\n");
                warned = true;
            }
            strlcpy(pb, b, sizeof pb); first_b = false;
            have_b = readrec(fd[1], b, sizeof b, '\n', NULL) >= 0;
        } else {
            emit(3, a);
            strlcpy(pa, a, sizeof pa); strlcpy(pb, b, sizeof pb);
            first_a = first_b = false;
            have_a = readrec(fd[0], a, sizeof a, '\n', NULL) >= 0;
            have_b = readrec(fd[1], b, sizeof b, '\n', NULL) >= 0;
        }
    }

    for (int i = 0; i < 2; i++) if (fd[i] != STDIN_FILENO) lp_close(fd[i]);
    return 0;
}
