/* sh.h - the types and declarations every part of the shell shares.
 *
 * The shell is one program built from one translation unit: sh.c
 * includes this header and then each part (core.c, var.c, parse.c,
 * expand.c, exec.c, builtin.c, edit.c) in turn. The userland Makefile
 * builds every program from <name>/<name>.c alone, and one unit also
 * lets every function stay static - the linker's --gc-sections then
 * drops whatever a build does not use, the same as for the other
 * programs.
 *
 * Everything here is declared before any part is included, so the parts
 * can call each other in any order without a web of forward
 * declarations scattered through them. */
#ifndef LP_SH_H
#define LP_SH_H

#include <stdarg.h>
#include "types.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "syscall.h"
#include "regex.h"

/* ── memory, strings, vectors (core.c) ─────────────────────────────── */

static void  *xmalloc(size_t n);
static void  *xcalloc(size_t n);
static void  *xrealloc(void *p, size_t n);
static void   xfree(void *p);
static char  *xstrdup(const char *s);
static char  *xstrndup(const char *s, size_t n);

/* A growable byte string. Always NUL terminated once anything has been
 * put in it; `s` may be NULL while it is still empty. */
typedef struct {
    char  *s;
    size_t len, cap;
} strbuf_t;

static void   sb_grow(strbuf_t *b, size_t need);
static void   sb_putc(strbuf_t *b, char c);
static void   sb_putn(strbuf_t *b, const char *s, size_t n);
static void   sb_puts(strbuf_t *b, const char *s);
static void   sb_printf(strbuf_t *b, const char *fmt, ...);
static void   sb_vprintf(strbuf_t *b, const char *fmt, va_list ap);
static char  *sb_str(strbuf_t *b);          /* never NULL */
static char  *sb_take(strbuf_t *b);         /* hand the buffer over */
static void   sb_free(strbuf_t *b);

/* A growable array of strings, NULL terminated. */
typedef struct {
    char **v;
    int    n, cap;
} strvec_t;

static void   sv_push(strvec_t *v, char *s);  /* takes ownership */
static void   sv_free(strvec_t *v);            /* frees the strings too */

/* Buffered output for builtins. fd 1 and 2 are written through these so
 * `printf` in a loop is not one system call per character; they are
 * flushed at the end of every builtin and before every fork. */
typedef struct {
    int    fd;
    size_t n;
    bool   err;         /* a write failed since the last check */
    int    errnum;
    char   buf[4096];
} out_t;

static out_t out1_s, out2_s;
#define out1 (&out1_s)
#define out2 (&out2_s)

static void   outn(out_t *o, const char *s, size_t n);
static void   outs(out_t *o, const char *s);
static void   outc(out_t *o, char c);
static void   outf(out_t *o, const char *fmt, ...);
static void   flush_out(out_t *o);
static void   flush_all(void);

/* Messages. sh_warn prints "sh: ..." (or "script: line N: ..." while a
 * script runs) and nothing else. sh_error does the same and then applies
 * the POSIX rule for an error the shell itself hit: a non-interactive
 * shell exits, an interactive one abandons the command it was running
 * and returns to the prompt. */
static void   sh_warn(const char *fmt, ...);
static void   sh_error(int status, const char *fmt, ...);
static void   sh_perror(const char *what, long err);  /* err is -errno */

static bool   is_name_start(int c);
static bool   is_name_char(int c);
static bool   is_valid_name(const char *s);
static bool   is_digit(int c);
static bool   is_blank(int c);
static bool   parse_int(const char *s, long long *out);
static char  *itoa_s(long long v, char *buf);          /* buf >= 24 */

/* Kernel calls the shared libc does not wrap. */
static long   sys_umask(long mask);
static long   sys_times(long *four);
static int    sys_geteuid(void);
static int    sys_getegid(void);
static int    sys_getgid_(void);
static int    sys_getuid_(void);
static int    sys_getppid(void);
static int    sys_getpgrp(void);
static int    sys_setpgid(int pid, int pgid);
static int    sys_tcgetpgrp(int fd);
static int    sys_tcsetpgrp(int fd, int pgid);
static int    fd_move_high(int fd);             /* dup to >= 10, cloexec */
static int    fd_dup_high(int fd);              /* dup to >= 10, cloexec */
static bool   fd_is_open(int fd);
static long   sys_sigmask(int how, const u64 *set, u64 *old);
static void   sig_disposition(int sig, long *old_handler);
static long   xread(int fd, void *buf, size_t n);   /* retries EINTR */
static bool   xwrite_all(int fd, const void *buf, size_t n);
static s64    now_ms(void);

/* ── options ───────────────────────────────────────────────────────── */

enum {
    O_allexport, O_notify, O_noclobber, O_errexit, O_noglob, O_hashall,
    O_interactive, O_monitor, O_noexec, O_nounset, O_verbose, O_xtrace,
    O_pipefail, O_ignoreeof, O_nolog, O_posix, O_emacs, O_vi,
    NOPTS
};

typedef struct {
    const char *name;
    char        letter;     /* 0 when only the long form exists */
} optname_t;

static bool opt[NOPTS];

/* ── variables (var.c) ─────────────────────────────────────────────── */

#define V_EXPORT    0x01
#define V_READONLY  0x02
#define V_ARRAY     0x04
#define V_INTEGER   0x08    /* declare -i: accepted, values are text */
#define V_UNSET     0x10    /* declared (local x) but holds no value */
#define V_SPECIAL   0x20    /* its value is computed: RANDOM, SECONDS... */

typedef struct var {
    struct var *next;       /* hash chain */
    char       *name;
    char       *val;        /* NULL when unset or an array */
    char      **arr;        /* V_ARRAY: elements, NULL for a hole */
    int         arrn;       /* V_ARRAY: size of arr */
    int         flags;
} var_t;

static var_t      *var_lookup(const char *name);
static const char *var_get(const char *name);       /* NULL if unset */
static bool        var_set(const char *name, const char *val, int flags);
static bool        var_set_n(const char *name, size_t nlen, const char *val,
                             int flags);
static bool        var_unset(const char *name);
static bool        var_set_elem(const char *name, long long idx,
                                const char *val);
static bool        var_set_array(const char *name, strvec_t *vals,
                                 bool append);
static const char *var_elem(var_t *v, long long idx);
static char      **env_build(void);
static void        var_import_env(char **envp);
static void        var_local(const char *name, const char *val, int flags,
                             bool has_val);
static int         locals_mark(void);
static void        locals_restore(int mark);
static void        var_tmp_scope_begin(void);
static void        var_tmp_scope_end(void);
static void        var_print_all(bool setform);
static void        var_print_flag(int flag, const char *prefix);
static const char *ifs_value(void);
static void        path_changed(void);
static void        print_quoted(out_t *o, const char *s);
static void        sb_quoted(strbuf_t *b, const char *s);

/* Positional parameters: $0 is kept apart from $1..$n because a
 * function call replaces the latter and never the former. */
static char  *arg0;
static char **posv;         /* $1.. (not NULL terminated in spirit, but is) */
static int    posc;

static void   pos_set(char **v, int n);          /* copies */
static void   pos_free(void);

/* ── parse tree (parse.c) ──────────────────────────────────────────── */

typedef struct node     node_t;
typedef struct word     word_t;
typedef struct wpart    wpart_t;
typedef struct redir    redir_t;
typedef struct caseitem caseitem_t;
typedef struct dbx      dbx_t;
typedef struct arena    arena_t;

enum {
    WP_LIT,         /* text; quoted says whether glob/split may touch it */
    WP_PARAM,       /* $x ${x...} */
    WP_CMDSUB,      /* $(...) and `...` */
    WP_ARITH,       /* $((...)) */
    WP_ARRAY        /* the (a b c) of an array assignment */
};

/* ${name OP arg} operators */
enum {
    PO_NONE, PO_DEFAULT, PO_ASSIGN, PO_ERROR, PO_ALT,     /* - = ? + */
    PO_RMSUF, PO_RMSUFL, PO_RMPRE, PO_RMPREL,             /* % %% # ## */
    PO_REPL, PO_REPLALL, PO_REPLPRE, PO_REPLSUF,          /* / // /# /% */
    PO_SUBSTR,                                            /* :off:len */
    PO_UPPER1, PO_UPPER, PO_LOWER1, PO_LOWER              /* ^ ^^ , ,, */
};

#define PF_COLON    0x01    /* :- := :? :+ */
#define PF_LEN      0x02    /* ${#x} */
#define PF_INDIRECT 0x04    /* ${!x} */
#define PF_KEYS     0x08    /* ${!a[@]} */
#define PF_BRACED   0x10    /* written with braces */

struct wpart {
    wpart_t *next;
    u8       type;
    u8       quoted;
    u8       op;
    u8       flags;
    char    *text;          /* LIT: the bytes; PARAM: the name */
    size_t   len;           /* LIT: how many */
    word_t  *arg;           /* PARAM operand, ARITH expression */
    word_t  *arg2;          /* PARAM: replacement or :len */
    word_t  *sub;           /* PARAM: array subscript */
    node_t  *tree;          /* CMDSUB */
    word_t  *list;          /* ARRAY: the elements */
};

#define W_QUOTED   0x01     /* some part of it was quoted */
#define W_HASPARTS 0x02     /* contains an expansion */

struct word {
    word_t  *next;
    wpart_t *parts;
    char    *lit;           /* the text if it is plain unquoted literal */
    u8       flags;
};

enum {
    R_IN, R_OUT, R_APPEND, R_CLOBBER, R_RDWR, R_DUPIN, R_DUPOUT,
    R_HEREDOC, R_HERESTR, R_OUTERR, R_APPENDERR
};

struct redir {
    redir_t *next;
    int      fd;
    u8       type;
    u8       hexpand;       /* heredoc: expand the body */
    u8       hstrip;        /* <<- */
    word_t  *target;
    char    *hbody;         /* heredoc body, filled in at the newline */
};

enum { CT_BREAK, CT_FALL, CT_TEST };      /* ;;  ;&  ;;& */

struct caseitem {
    caseitem_t *next;
    word_t     *pats;
    node_t     *body;
    u8          term;
};

enum { DB_AND, DB_OR, DB_NOT, DB_UNARY, DB_BINARY, DB_WORD };

struct dbx {
    u8         kind;
    char       op[4];
    dbx_t     *l, *r;
    word_t    *w1, *w2;
};

enum {
    N_CMD, N_PIPE, N_AND, N_OR, N_SEQ, N_BG, N_NOT, N_SUBSHELL, N_GROUP,
    N_IF, N_WHILE, N_UNTIL, N_FOR, N_CASE, N_FUNC, N_DBRACK, N_ARITH,
    N_FORARITH, N_TIME
};

struct node {
    u8          type;
    u8          flags;
    int         lineno;
    node_t     *a, *b, *c, *d;
    node_t     *next;       /* the next stage of a pipeline */
    word_t     *words;
    word_t     *assigns;
    redir_t    *redirs;
    char       *name;
    caseitem_t *items;
    dbx_t      *dbx;
    arena_t    *arena;      /* N_FUNC: where the body lives */
};

struct arena {
    struct ablock *blocks;
    int            refs;
};

static arena_t *arena_new(void);
static void     arena_ref(arena_t *a);
static void     arena_unref(arena_t *a);
static arena_t *cur_arena;      /* where the parser allocates */

/* Input sources: a script file, a string (-c, eval, a trap), an alias
 * being expanded, or the terminal through the line editor. */
static void   input_push_file(int fd, bool is_tty_editor);
static void   input_push_string(const char *s, size_t len, void *alias);
static void   input_pop(void);
static int    input_depth(void);
static int    input_lineno(void);

#define PARSE_EOF   ((node_t *)-1)
static node_t *parse_command(bool *error);
static void    parse_reset(void);
static word_t *parse_string_word(const char *s, bool heredoc_mode,
                                 bool *error);
static void    fmt_node(strbuf_t *b, node_t *n, int indent);
static void    fmt_word(strbuf_t *b, word_t *w);
static bool    is_keyword(const char *s);

/* ── expansion (expand.c) ──────────────────────────────────────────── */

#define X_SPLIT    0x01     /* field splitting */
#define X_GLOB     0x02     /* pathname expansion */
#define X_TILDE    0x04
#define X_ASSIGN   0x08     /* tilde after : as well (assignments) */
#define X_PATTERN  0x10     /* produce a pattern: quoted chars escaped */
#define X_NOBRACE  0x20

static bool   expand_words(word_t *w, strvec_t *out, int flags);
static char  *expand_str(word_t *w, int flags);     /* NULL on error */
static char  *expand_heredoc(const char *body);     /* NULL on error */
static bool   expand_error;          /* set by the last expansion */
static bool   pmatch(const char *pat, const char *s, bool pathname);
static bool   has_glob_chars(const char *pat);
static char  *pat_unescape(const char *pat);
static bool   arith_eval(const char *expr, long long *out);
static char  *cmdsub_capture(node_t *tree, int *status);
static bool   glob_expand(const char *pat, strvec_t *out);
static char  *tilde_home(const char *user);          /* NULL if unknown */
static char  *prompt_expand(const char *ps, int *visible_width);

/* ── execution (exec.c) ────────────────────────────────────────────── */

#define EV_EXIT    0x01     /* the process exits after this node */
#define EV_TESTED  0x02     /* errexit is off for this node */
#define EV_BG      0x04

enum { SKIP_NONE, SKIP_BREAK, SKIP_CONT, SKIP_RETURN, SKIP_ABORT };

static int    exitstatus;           /* $? */
static int    evalskip;             /* break/continue/return/abort */
static int    skipcount;
static int    loopnest;
static int    funcnest;
static int    sourcenest;
static bool   in_subshell;          /* a forked child, not the top shell */
static bool   toplevel_interactive; /* the prompt loop owns this process */
static int    rootpid;              /* $$ */
static int    lastbgpid;            /* $! */
static int    cmd_lineno;           /* LINENO for the running command */
static char  *script_name;          /* for messages: "name: line N:" */
static bool   is_login;

static int    evaltree(node_t *n, int flags);
static int    evalstring(const char *s, int flags);
static void   exitshell(int status) __attribute__((noreturn));
static int    run_file(const char *path, bool must_exist);
static void   run_pending_traps(void);
static volatile int pending_signals;

/* Function table */
typedef struct func {
    struct func *next;
    char        *name;
    node_t      *body;          /* an N_FUNC node */
} func_t;

static func_t *func_lookup(const char *name);
static void    func_define(node_t *fn);
static bool    func_remove(const char *name);

/* Command lookup */
enum { CMD_NONE, CMD_SPECIAL, CMD_BUILTIN, CMD_FUNC, CMD_EXTERNAL,
       CMD_FALLBACK };

typedef int (*builtin_fn)(int argc, char **argv);

typedef struct {
    const char *name;
    builtin_fn  fn;
    u8          kind;           /* CMD_SPECIAL, CMD_BUILTIN, CMD_FALLBACK */
    const char *usage;
    const char *help;
} builtin_t;

static const builtin_t *builtin_find(const char *name);
static bool   path_search(const char *name, char *out, size_t size,
                          const char *path, bool use_hash);
static void   hash_clear(void);

/* Traps */
#define NSIG_SH 65
static char  *trap_cmd[NSIG_SH];    /* NULL: default, "": ignored */
static bool   sig_ignored_at_entry[NSIG_SH];
static int    sig_number(const char *name);
static const char *sig_name(int sig);
static void   trap_set(int sig, const char *cmd);
static void   signals_init(void);
static void   signals_reset_for_child(bool async_nojc);
static void   sigint_check(void);

/* Jobs */
static bool   job_control;          /* set -m is really in effect */
static int    tty_fd = -1;
static int    shell_pgid;
static void   jobs_init(void);
static void   jobs_notify(bool all);
static int    job_wait_arg(const char *arg, bool *found);
static int    job_wait_all(void);
static void   jobs_hup_all(void);
static bool   jobs_any_stopped(void);

/* ── builtins (builtin.c) ──────────────────────────────────────────── */

static int    test_main(int argc, char **argv, bool bracket);
static bool   read_line_fd(int fd, strbuf_t *out, int delim, bool *eof);
static void   dirs_init(void);
static const char *pwd_logical(void);
static void   pwd_update(void);

/* Aliases */
typedef struct alias {
    struct alias *next;
    char         *name;
    char         *val;
    bool          busy;         /* being expanded: no recursion */
} alias_t;

static alias_t *alias_lookup(const char *name);
static void     alias_set(const char *name, const char *val);

/* ── the terminal (edit.c) ─────────────────────────────────────────── */

#define EDIT_EOF   (-1)
#define EDIT_INTR  (-2)
static long   edit_read_line(const char *prompt, strbuf_t *out);
static void   history_init(void);
static void   history_add(const char *line);
static bool   history_expand(const char *line, strbuf_t *out, bool *print);
static void   history_list(int n);
static void   history_clear(void);
static bool   history_delete(int n);
static void   history_write(void);
static int    history_count(void);
static void   term_save_mode(void);
static void   term_restore_mode(void);
static void   term_update_size(void);
static int    term_cols;
static volatile int got_sigwinch;

#endif /* LP_SH_H */
