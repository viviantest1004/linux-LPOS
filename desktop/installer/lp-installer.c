/*
 * lp-installer - the window that installs LP from the USB stick.
 *
 * Every decision and every write belongs to lp-install (the Python
 * program beside this file); this is its face. The split is deliberate:
 * an installer whose logic lives in a GUI can only be tested by clicking
 * through it, and the one that matters most - the disk it refuses - has
 * to be right from a serial console too. So this window asks lp-install
 * for the disks (`list --json`) and the firmware facts (`check --json`),
 * shows them, and runs `install --progress`, reading one line per event.
 *
 * The pages, in order:
 *
 *   welcome    English or 한국어 (English preselected); Install, or Try
 *              LP without installing (exit status 10 - the setup gate
 *              then starts the ordinary desktop).
 *   problem    Secure Boot on, or the SATA controller in RAID mode with
 *              no disk visible: what to change in the BIOS, and Check
 *              again. Neither can be fixed from here.
 *   account    name, user name, password twice (setup-pages.c). Asked
 *              here and not at first boot because the password is also
 *              the recovery password, and that goes onto LP-RECOVERY
 *              with the copy - the page says so.
 *   region     time zone and computer name.
 *   disks      one large row per disk; the stick we run from and disks
 *              with mounted partitions are shown, greyed, with the reason.
 *              Under the chosen disk, how LP goes on it: the whole disk
 *              (erased), one partition, or its free space - the last
 *              two leave the rest of the disk, Windows say, as it is.
 *   parts      the partitions of that disk, each with its size, what is
 *              on it, and whether LP can go there (greyed, with the
 *              reason, when not). Only when "a partition" was chosen.
 *   confirm    the disk partition by partition, as lp-install plans it
 *              (install --dry-run --json): what is erased, what is made
 *              and how big, what is kept, what goes onto the EFI
 *              partition - then a tick box before the red button.
 *              No typed confirmation: the person may have no keyboard
 *              yet, and a tick box is as deliberate as typing a name.
 *   progress   the step, and a bar that eases on a spring.
 *   done       remove the stick, restart.  failed  what lp-install said.
 *
 * Touch first: 56-64 px targets at scale 2, nothing that needs a hover,
 * nothing that needs a keyboard. Motion per COMMON.md: pages slide 260 ms
 * the right way (a crossfade under 100 ms with reduced motion), disk rows
 * come in through revealers at the insert spring's 220 ms, the progress
 * bar follows a critically damped spring so 1% steps read as one motion.
 *
 *   lp-installer [--korean] [--windowed]
 *   LP_INSTALL=/path/to/lp-install   another backend (tests, screenshots)
 */
#include "setup-ui.h"
#include "setup-pages.h"
#include "lp-json.h"
#include "lp-motion.h"

#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <unistd.h>

#define EXIT_TRY 10

/* How LP goes on the chosen disk (lp-install install --disk, --partition,
 * --free-space). */
enum { MODE_NONE, MODE_WHOLE, MODE_PART, MODE_FREE };

typedef struct {
    GtkApplication *app;
    GtkWidget  *win;
    GtkWidget  *stack;
    int         exit_code;
    gboolean    windowed;

    /* welcome */
    GtkWidget  *en, *ko;
    /* problem */
    GtkWidget  *problem_text;
    /* disks */
    GtkWidget  *disk_box;
    GtkWidget  *disk_next;
    GtkWidget  *disk_group;         /* first toggle, the group leader */
    char        disk[64];           /* /dev/nvme0n1 */
    char        disk_model[128];
    char        disk_size[32];
    int         disk_parts;
    gboolean    disk_inplace;       /* the disk LP runs from, installed where it is */
    LpJson     *list;               /* `lp-install list --json`, while its rows live */
    LpJson     *disk_j;             /* the chosen disk's entry in it */
    int         mode;               /* MODE_* */
    GtkWidget  *mode_reveal, *mode_box;
    /* parts */
    GtkWidget  *part_box, *part_next, *part_group;
    char        part[64];           /* /dev/nvme0n1p3 */
    char       *part_en, *part_ko;  /* /dev/nvme0n1p3 (120 GB, ntfs "Data") */
    /* confirm */
    GtkWidget  *confirm_title;
    GtkWidget  *confirm_what, *confirm_warn, *confirm_check, *confirm_go;
    GtkWidget  *confirm_note;
    GtkWidget  *confirm_plan;       /* one row per partition: erased, made, kept */
    gboolean    plan_ok;            /* the backend's plan came back */
    /* progress */
    GtkWidget  *step;
    GtkWidget  *percent;
    SuProgress *bar;
    GSubprocess *proc;
    GDataInputStream *lines;
    char        last_error[1024];
    /* failed */
    GtkWidget  *fail_text;
    /* account, region; confirm's summary; done's sign-in line */
    SuAccount   acct;
    SuRegion    region;
    GtkWidget  *confirm_who;
    GtkWidget  *done_text, *done_sub;
    /* the card's entrance */
    GtkWidget  *card;
    LpSpring    card_in;
    LpMotion   *card_m;
} App;

static App A;

static const char *backend(void)
{
    const char *b = g_getenv("LP_INSTALL");
    return b && *b ? b : "/usr/local/bin/lp-install";
}

/* ── welcome ───────────────────────────────────────────────────────── */

static void on_language(GtkToggleButton *b, gpointer data)
{
    (void)data;
    if (!gtk_toggle_button_get_active(b))
        return;
    su_set_korean(GTK_WIDGET(b) == A.ko);
    su_region_language_changed(&A.region);
}


static void on_continue(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    /* The firmware first: with Secure Boot on or the disk hidden behind
     * RAID there is no point choosing a disk. */
    const char *argv[] = { backend(), "check", "--json", NULL };
    char *out = su_run(argv, NULL);
    LpJson *j = out ? lp_json_parse(out) : NULL;
    LpJson *probs = j ? lp_json_get(j, "problems") : NULL;
    if (probs && lp_json_len(probs) > 0) {
        GString *en = g_string_new(NULL), *ko = g_string_new(NULL);
        for (int i = 0; i < lp_json_len(probs); i++) {
            LpJson *p = lp_json_at(probs, i);
            g_string_append_printf(en, "%s%s", i ? "\n\n" : "", lp_json_str(p, "en", ""));
            g_string_append_printf(ko, "%s%s", i ? "\n\n" : "", lp_json_str(p, "ko", ""));
        }
        su_retext(A.problem_text, en->str, ko->str);
        g_string_free(en, TRUE);
        g_string_free(ko, TRUE);
        su_go(A.stack, "problem", TRUE);
    } else {
        su_go(A.stack, "account", TRUE);
    }
    if (j)
        lp_json_free(j);
    g_free(out);
}

/* "Try" goes to the live account's session, which reads its language
 * from ~/.config/lp/locale (session-run): the language chosen on this
 * page is written there, or a person who picked 한국어 got an English
 * desktop. This window runs as that account (uid 1000) with a HOME of its
 * own under /run, so the account's real home comes from passwd. */
static void on_try(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) {
        char *dir = g_build_filename(pw->pw_dir, ".config", "lp", NULL);
        char *f = g_build_filename(dir, "locale", NULL);
        GError *e = NULL;
        g_mkdir_with_parents(dir, 0755);
        if (!g_file_set_contents(f, su_korean ? "ko_KR.UTF-8\n" : "en_US.UTF-8\n", -1, &e)) {
            g_printerr("lp-installer: %s: %s\n", f, e->message);
            g_error_free(e);
        }
        g_free(f);
        g_free(dir);
    }
    A.exit_code = EXIT_TRY;
    gtk_window_destroy(GTK_WINDOW(A.win));
}

static void page_welcome(void)
{
    SuPage p;
    su_page(&p, "Welcome to LP", "LP 에 오신 것을 환영합니다",
            "Choose a language. You can change it again later in Settings.",
            "언어를 고르세요. 나중에 설정에서 다시 바꿀 수 있습니다.");
    A.en = su_choice("English", "English", "English (United States)",
                     "영어 (미국)", NULL);
    A.ko = su_choice("한국어", "한국어", "Korean", "한국어 (대한민국)", NULL);
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(A.ko), GTK_TOGGLE_BUTTON(A.en));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(su_korean ? A.ko : A.en), TRUE);
    g_signal_connect(A.en, "toggled", G_CALLBACK(on_language), NULL);
    g_signal_connect(A.ko, "toggled", G_CALLBACK(on_language), NULL);
    gtk_box_append(GTK_BOX(p.body), A.en);
    gtk_box_append(GTK_BOX(p.body), A.ko);
    gtk_box_append(GTK_BOX(p.body), su_label(
        "LP will be installed on this computer's disk. Nothing is written "
        "until you confirm which disk, on the last page before the copy starts.",
        "LP 를 이 컴퓨터의 디스크에 설치합니다. 복사를 시작하기 전 마지막 "
        "화면에서 디스크를 확인하기 전까지는 아무것도 쓰지 않습니다.", "su-note"));

    GtkWidget *try = su_button("Try LP without installing", "설치하지 않고 써 보기",
                               "su-secondary");
    g_signal_connect(try, "clicked", G_CALLBACK(on_try), NULL);
    gtk_box_append(GTK_BOX(p.left), try);
    GtkWidget *go = su_button("Install LP", "LP 설치", "su-primary");
    g_signal_connect(go, "clicked", G_CALLBACK(on_continue), NULL);
    gtk_box_append(GTK_BOX(p.right), go);
    gtk_stack_add_named(GTK_STACK(A.stack), p.root, "welcome");
}

/* ── problem ───────────────────────────────────────────────────────── */

static void power(const char *word)
{
    /* In the kiosk LP_POWER is the setup gate's relay; elsewhere the
     * setuid lp-power that the desktop's own power menu uses. */
    const char *p = g_getenv("LP_POWER");
    const char *argv[] = { p && *p ? p : "/bin/lp-power", word, NULL };
    g_free(su_run(argv, NULL));
}

static void on_restart(GtkButton *b, gpointer d) { (void)b; (void)d; power("restart"); }
static void on_poweroff(GtkButton *b, gpointer d) { (void)b; (void)d; power("off"); }
static void on_back_welcome(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    su_go(A.stack, "welcome", FALSE);
}

static void page_problem(void)
{
    SuPage p;
    su_page(&p, "Change a BIOS setting first", "먼저 BIOS 설정을 바꿔야 합니다",
            "LP cannot be installed the way this computer is set up now.",
            "지금 설정으로는 LP 를 설치할 수 없습니다.");
    A.problem_text = su_label("", "", "su-warn");
    gtk_box_append(GTK_BOX(p.body), A.problem_text);
    gtk_box_append(GTK_BOX(p.body), su_label(
        "Restart, press F2 when the Dell logo appears, change the setting, "
        "save, and start from this USB stick again (F12).",
        "다시 시작한 뒤 Dell 로고가 보일 때 F2 를 누르고, 설정을 바꿔 저장한 "
        "다음 이 USB 로 다시 시작하세요 (F12).", "su-note"));
    GtkWidget *back = su_button("Back", "뒤로", "su-secondary");
    g_signal_connect(back, "clicked", G_CALLBACK(on_back_welcome), NULL);
    gtk_box_append(GTK_BOX(p.left), back);
    GtkWidget *again = su_button("Check again", "다시 확인", "su-secondary");
    g_signal_connect(again, "clicked", G_CALLBACK(on_continue), NULL);
    gtk_box_append(GTK_BOX(p.right), again);
    GtkWidget *rs = su_button("Restart", "다시 시작", "su-primary");
    g_signal_connect(rs, "clicked", G_CALLBACK(on_restart), NULL);
    gtk_box_append(GTK_BOX(p.right), rs);
    gtk_stack_add_named(GTK_STACK(A.stack), p.root, "problem");
}

/* ── account and region (setup-pages.c) ──────────────────────────── */

static void on_back_region(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    su_go(A.stack, "region", FALSE);
}

static void on_account_back(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    su_go(A.stack, "welcome", FALSE);
}

static void on_account_next(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    su_region_suggest_host(&A.region, su_account_login(&A.acct));
    su_go(A.stack, "region", TRUE);
}

static void on_region_back(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    su_go(A.stack, "account", FALSE);
}

static void show_disks(void);

static void on_region_next(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    show_disks();
    su_go(A.stack, "disks", TRUE);
}

static void page_account(void)
{
    su_account_build(&A.acct, TRUE);
    g_signal_connect(A.acct.back, "clicked", G_CALLBACK(on_account_back), NULL);
    g_signal_connect(A.acct.next, "clicked", G_CALLBACK(on_account_next), NULL);
    gtk_stack_add_named(GTK_STACK(A.stack), A.acct.page.root, "account");
}

static void page_region(void)
{
    su_region_build(&A.region);
    g_signal_connect(A.region.back, "clicked", G_CALLBACK(on_region_back), NULL);
    g_signal_connect(A.region.next, "clicked", G_CALLBACK(on_region_next), NULL);
    gtk_stack_add_named(GTK_STACK(A.stack), A.region.page.root, "region");
}

/* ── disks ─────────────────────────────────────────────────────────── */

static const char *transport_name(const char *t, gboolean ko)
{
    if (!strcmp(t, "nvme")) return "NVMe SSD";
    if (!strcmp(t, "usb")) return ko ? "USB 저장장치" : "USB drive";
    if (!strcmp(t, "sata")) return "SATA";
    if (!strcmp(t, "mmc")) return ko ? "SD 카드" : "SD card";
    if (!strcmp(t, "virtio")) return ko ? "가상 디스크" : "Virtual disk";
    return ko ? "디스크" : "Disk";
}

/* Why a whole disk cannot be erased for LP (list_disks' "reason"). */
static void disk_reason_words(const char *r, const char **en, const char **ko)
{
    *en = !strcmp(r, "running") ? "LP is running from this disk"
        : !strcmp(r, "mounted") ? "In use: it has mounted partitions"
        : !strcmp(r, "in-use")  ? "In use (encrypted or LVM)"
        : !strcmp(r, "read-only") ? "Read-only" : r;
    *ko = !strcmp(r, "running") ? "지금 LP 가 돌고 있는 디스크입니다"
        : !strcmp(r, "mounted") ? "사용 중: 마운트된 파티션이 있습니다"
        : !strcmp(r, "in-use")  ? "사용 중 (암호화 또는 LVM)"
        : !strcmp(r, "read-only") ? "읽기 전용" : r;
}

/* Why a partition, or a disk's free space, cannot take LP: the keys
 * lp-install's shared_layout() gives. Both strings are the caller's. */
static void reason_words(const char *r, LpJson *disk, const char *mount,
                         char **en, char **ko)
{
    const char *need = lp_json_str(disk, "root_need_text", "");
    if (!strcmp(r, "running")) {
        *en = g_strdup("LP is running from this disk");
        *ko = g_strdup("지금 LP 가 돌고 있는 디스크입니다");
    } else if (!strcmp(r, "in-use")) {
        *en = g_strdup("In use (encrypted or LVM)");
        *ko = g_strdup("사용 중 (암호화 또는 LVM)");
    } else if (!strcmp(r, "read-only")) {
        *en = g_strdup("Read-only");
        *ko = g_strdup("읽기 전용");
    } else if (!strcmp(r, "not-gpt") && !*lp_json_str(disk, "table", "")) {
        *en = g_strdup("The disk has no partitions yet");
        *ko = g_strdup("이 디스크에는 아직 파티션이 없습니다");
    } else if (!strcmp(r, "not-gpt")) {
        *en = g_strdup("An MBR disk: LP can share only a GPT disk");
        *ko = g_strdup("MBR 디스크입니다. 다른 시스템과 함께 쓰려면 GPT 디스크여야 합니다");
    } else if (!strcmp(r, "esp")) {
        *en = g_strdup("The EFI system partition the computer starts from: LP only "
                       "adds its start-up files to it");
        *ko = g_strdup("컴퓨터가 시작하는 EFI 시스템 파티션입니다. LP 는 여기에 시작 "
                       "파일만 더합니다");
    } else if (!strcmp(r, "mounted")) {
        *en = g_strdup_printf("In use: mounted at %s", mount);
        *ko = g_strdup_printf("사용 중: %s 에 마운트되어 있습니다", mount);
    } else if (!strcmp(r, "swap")) {
        *en = g_strdup("In use as swap");
        *ko = g_strdup("스왑으로 쓰고 있습니다");
    } else if (!strcmp(r, "too-small")) {
        *en = g_strdup_printf("Too small: LP needs %s or more", need);
        *ko = g_strdup_printf("너무 작습니다. LP 는 %s 이상이 필요합니다", need);
    } else if (!strcmp(r, "no-esp")) {
        *en = g_strdup("The disk has no EFI system partition to start LP from, and no "
                       "512 MB of free space to make one");
        *ko = g_strdup("이 디스크에는 LP 를 시작할 EFI 시스템 파티션이 없고, 새로 만들 "
                       "512MB 빈 공간도 없습니다");
    } else if (!strcmp(r, "esp-mounted")) {
        *en = g_strdup("The disk's EFI system partition is mounted");
        *ko = g_strdup("이 디스크의 EFI 시스템 파티션이 마운트되어 있습니다");
    } else if (!strcmp(r, "esp-bad")) {
        *en = g_strdup("The disk's EFI system partition cannot be read");
        *ko = g_strdup("이 디스크의 EFI 시스템 파티션을 읽을 수 없습니다");
    } else if (!strcmp(r, "esp-full")) {
        double fr = lp_json_num(disk, "esp_free", 0);
        char *f = g_format_size((guint64)MAX(0.0, fr));
        char *n = g_format_size((guint64)lp_json_num(disk, "esp_need", 0));
        *en = g_strdup_printf("The EFI system partition is too full: %s free, LP's "
                              "start-up files need %s", f, n);
        *ko = g_strdup_printf("EFI 시스템 파티션에 자리가 모자랍니다: %s 남았고, LP 시작 "
                              "파일에 %s 가 필요합니다", f, n);
        g_free(f);
        g_free(n);
    } else {
        *en = g_strdup(r);
        *ko = g_strdup(r);
    }
}

/* Reveal the rows one after another, 40 ms apart: the list assembles
 * rather than appearing in one flash. */
static gboolean reveal_one(gpointer data)
{
    gtk_revealer_set_reveal_child(GTK_REVEALER(data), TRUE);
    return G_SOURCE_REMOVE;
}

static GtkWidget *revealed(GtkWidget *child, int i)
{
    GtkWidget *rv = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(rv),
        lp_motion_reduced() ? GTK_REVEALER_TRANSITION_TYPE_CROSSFADE
                            : GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(rv),
        lp_motion_reduced() ? 90 : lp_spring_ms(LP_SPRING_INSERT, FALSE));
    gtk_revealer_set_child(GTK_REVEALER(rv), child);
    g_timeout_add(200 + 40 * i, reveal_one, rv);
    return rv;
}

static void on_mode_toggled(GtkToggleButton *b, gpointer d)
{
    if (!gtk_toggle_button_get_active(b))
        return;
    A.mode = GPOINTER_TO_INT(d);
    gtk_widget_set_sensitive(A.disk_next, TRUE);
}

static GtkWidget *mode_row(int mode, const char *en, const char *ko,
                           const char *den, const char *dko, gboolean ok,
                           GtkWidget **group)
{
    GtkWidget *b = su_choice(en, ko, den, dko, NULL);
    gtk_widget_set_sensitive(b, ok);
    if (*group)
        gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(*group));
    else
        *group = b;
    g_signal_connect(b, "toggled", G_CALLBACK(on_mode_toggled), GINT_TO_POINTER(mode));
    gtk_box_append(GTK_BOX(A.mode_box), b);
    return b;
}

/* Under the chosen disk: how LP goes on it. The whole disk, erased - the
 * install LP always had; one partition of it, erased, the others kept;
 * or its unallocated space, nothing erased. A way that is not open on
 * this disk is shown greyed with the reason, so "why can't I keep
 * Windows?" has an answer on the screen. Nothing is preselected while
 * there is a choice: which one erases what is the person's decision. */
static void show_modes(void)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(A.mode_box)))
        gtk_box_remove(GTK_BOX(A.mode_box), c);
    A.mode = MODE_NONE;
    LpJson *d = A.disk_j;
    if (!d || A.disk_inplace) {
        /* Installed where it runs: one way, and nothing erased. */
        A.mode = A.disk_inplace ? MODE_WHOLE : MODE_NONE;
        gtk_revealer_set_reveal_child(GTK_REVEALER(A.mode_reveal), FALSE);
        gtk_widget_set_sensitive(A.disk_next, A.mode != MODE_NONE);
        return;
    }
    GtkWidget *group = NULL, *only = NULL;
    int n_ok = 0;
    char *en, *ko;

    gboolean ok = lp_json_bool(d, "usable", 0);
    if (ok) {
        en = g_strdup_printf("Everything on the disk is erased; LP gets all %s, with "
                             "its recovery system.", A.disk_size);
        ko = g_strdup_printf("디스크의 모든 것을 지우고 %s 전체를 LP 가 씁니다. 복구 "
                             "시스템도 함께 설치합니다.", A.disk_size);
    } else {
        const char *wen, *wko;
        disk_reason_words(lp_json_str(d, "reason", ""), &wen, &wko);
        en = g_strdup(wen);
        ko = g_strdup(wko);
    }
    GtkWidget *b = mode_row(MODE_WHOLE, "Erase the whole disk", "디스크 전체에 설치 (모두 지움)",
                            en, ko, ok, &group);
    g_free(en); g_free(ko);
    if (ok) {
        n_ok++;
        only = b;
    }

    ok = lp_json_bool(d, "part_ok", 0);
    LpJson *esp_new = lp_json_get(d, "esp_new");
    /* A partition that makes LP's own EFI partition inside itself: the
     * disk's cannot start LP (full, unreadable, mounted, or none). */
    gboolean own_esp = FALSE;
    LpJson *dparts = lp_json_get(d, "parts");
    for (int i = 0; dparts && i < lp_json_len(dparts); i++) {
        LpJson *p = lp_json_at(dparts, i);
        if (lp_json_bool(p, "usable", 0) && lp_json_bool(p, "own_esp", 0))
            own_esp = TRUE;
    }
    if (ok && own_esp) {
        en = g_strdup("Choose one partition to erase for LP; the other partitions stay as "
                      "they are. The disk's EFI system partition cannot start LP, so LP "
                      "makes its own start-up partition (512 MB) inside the one you choose, "
                      "and its recovery system when there is room.");
        ko = g_strdup("LP 를 설치할 파티션 하나를 골라 지웁니다. 나머지 파티션은 그대로 "
                      "둡니다. 이 디스크의 EFI 시스템 파티션으로는 LP 를 시작할 수 없어서, "
                      "고른 파티션 안에 LP 전용 시작 파티션(512MB)을 만들고 자리가 있으면 "
                      "복구 시스템도 만듭니다.");
    } else if (ok && esp_new && lp_json_len(esp_new) > 0) {
        en = g_strdup("Choose one partition to erase for LP; the other partitions "
                      "stay as they are. The disk has no EFI system partition, so a "
                      "512 MB one is made in its free space. No recovery system.");
        ko = g_strdup("LP 를 설치할 파티션 하나를 골라 지웁니다. 나머지 파티션은 "
                      "그대로 둡니다. 이 디스크에는 EFI 시스템 파티션이 없어서 빈 공간에 "
                      "512MB 시작 파티션을 만듭니다. 복구 시스템은 없습니다.");
    } else if (ok) {
        en = g_strdup("Choose one partition to erase for LP; the other partitions "
                      "stay as they are. No recovery system (it needs a partition of "
                      "its own).");
        ko = g_strdup("LP 를 설치할 파티션 하나를 골라 지웁니다. 나머지 파티션은 "
                      "그대로 둡니다. 복구 시스템은 없습니다 (따로 파티션이 필요합니다).");
    } else {
        /* The disk's own reason, or the EFI partition's (every partition
         * big enough carries it), or: none is big enough and free. */
        const char *r = lp_json_str(d, "shared_reason", "");
        LpJson *parts = lp_json_get(d, "parts");
        for (int i = 0; !*r && parts && i < lp_json_len(parts); i++) {
            const char *pr = lp_json_str(lp_json_at(parts, i), "reason", "");
            if (g_str_has_prefix(pr, "esp-") || !strcmp(pr, "no-esp"))
                r = pr;
        }
        if (*r) {
            reason_words(r, d, "", &en, &ko);
        } else {
            const char *need = lp_json_str(d, "root_need_text", "");
            en = g_strdup_printf("No partition here is free to use and %s or larger", need);
            ko = g_strdup_printf("쓸 수 있는 %s 이상의 파티션이 없습니다", need);
        }
    }
    /* Openable whenever the disk has partitions, even when none can take
     * LP: the next page lists every one with what stops it. A greyed row
     * with one sentence left "where is my partition?" unanswered. */
    gboolean can_look = !ok && dparts && lp_json_len(dparts) > 0 &&
                        !lp_json_bool(d, "running", 0);
    b = mode_row(MODE_PART, "Install into a partition", "파티션에 설치", en, ko,
                 ok || can_look, &group);
    g_free(en); g_free(ko);
    if (ok) {
        n_ok++;
        only = b;
    }

    /* Free space: offered where there is enough, and otherwise shown
     * greyed with what it would take - all three ways are always on the
     * page, so "why can't I use the empty space?" has an answer. */
    LpJson *free = lp_json_get(d, "free");
    if (!free || lp_json_len(free) == 0) {
        const char *r = lp_json_str(d, "shared_reason", "");
        if (*r) {
            reason_words(r, d, "", &en, &ko);
        } else {
            const char *fneed = lp_json_str(d, "free_need_text", "");
            const char *big = lp_json_str(d, "free_largest_text", "");
            if (*big) {
                en = g_strdup_printf("No unallocated space of %s or more on this disk (the "
                                     "largest is %s)", fneed, big);
                ko = g_strdup_printf("이 디스크에는 %s 이상의 할당되지 않은 빈 공간이 "
                                     "없습니다 (가장 큰 것이 %s)", fneed, big);
            } else {
                en = g_strdup_printf("No unallocated space on this disk; LP needs %s", fneed);
                ko = g_strdup_printf("이 디스크에는 할당되지 않은 빈 공간이 없습니다. LP 는 %s "
                                     "가 필요합니다", fneed);
            }
        }
        mode_row(MODE_FREE, "Install into free space", "빈 공간에 설치", en, ko, FALSE, &group);
        g_free(en); g_free(ko);
    } else {
        LpJson *r0 = lp_json_at(free, 0);          /* the largest */
        const char *sz = lp_json_str(r0, "size_text", "");
        const char *rsz = lp_json_str(r0, "root_size_text", "");
        ok = lp_json_bool(d, "free_ok", 0) && lp_json_bool(r0, "usable", 0);
        if (ok && lp_json_bool(r0, "recovery", 0)) {
            en = g_strdup_printf("New partitions in the %s of unallocated space: LP (%s), its "
                                 "recovery system and its own start-up partition. Nothing "
                                 "is erased.", sz, rsz);
            ko = g_strdup_printf("할당되지 않은 빈 공간 %s 에 새 파티션을 만듭니다: LP (%s), "
                                 "복구 시스템, LP 전용 시작 파티션. 아무것도 지우지 "
                                 "않습니다.", sz, rsz);
        } else if (ok && lp_json_bool(r0, "new_esp", 0)) {
            en = g_strdup_printf("A new partition in the %s of unallocated space, and a "
                                 "512 MB EFI partition of its own to start it; nothing is "
                                 "erased. No recovery system.", sz);
            ko = g_strdup_printf("할당되지 않은 빈 공간 %s 에 새 파티션과 LP 전용 512MB "
                                 "시작 파티션을 만듭니다. 아무것도 지우지 않습니다. 복구 "
                                 "시스템은 없습니다.", sz);
        } else if (ok) {
            en = g_strdup_printf("A new partition in the %s of unallocated space; nothing "
                                 "is erased. No room for the recovery system.", sz);
            ko = g_strdup_printf("할당되지 않은 빈 공간 %s 에 새 파티션을 만듭니다. "
                                 "아무것도 지우지 않습니다. 복구 시스템을 둘 자리는 "
                                 "없습니다.", sz);
        } else {
            reason_words(lp_json_str(r0, "reason", ""), d, "", &en, &ko);
        }
        b = mode_row(MODE_FREE, "Install into free space", "빈 공간에 설치", en, ko, ok, &group);
        g_free(en); g_free(ko);
        if (ok) {
            n_ok++;
            only = b;
        }
    }
    gtk_widget_set_sensitive(A.disk_next, FALSE);
    /* One way open is no choice: it is taken, as a single usable disk is.
     * The confirmation still names what is erased before anything is. */
    if (n_ok == 1)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(only), TRUE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(A.mode_reveal), TRUE);
}

static void on_disk_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    if (!gtk_toggle_button_get_active(b))
        return;
    g_strlcpy(A.disk, g_object_get_data(G_OBJECT(b), "path"), sizeof A.disk);
    g_strlcpy(A.disk_model, g_object_get_data(G_OBJECT(b), "model"), sizeof A.disk_model);
    g_strlcpy(A.disk_size, g_object_get_data(G_OBJECT(b), "size"), sizeof A.disk_size);
    A.disk_parts = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "parts"));
    A.disk_inplace = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "inplace"));
    A.disk_j = g_object_get_data(G_OBJECT(b), "json");
    show_modes();
}

static void show_disks(void)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(A.disk_box)))
        gtk_box_remove(GTK_BOX(A.disk_box), c);
    A.disk[0] = '\0';
    A.disk_group = NULL;
    A.disk_j = NULL;
    A.disk_inplace = FALSE;
    show_modes();
    if (A.list)
        lp_json_free(A.list);
    A.list = NULL;
    gtk_widget_set_sensitive(A.disk_next, FALSE);

    const char *argv[] = { backend(), "list", "--json", NULL };
    char *out = su_run(argv, NULL);
    LpJson *j = A.list = out ? lp_json_parse(out) : NULL;
    int n = j ? lp_json_len(j) : 0;
    int usable = 0;
    GtkWidget *only = NULL;
    for (int i = 0; i < n; i++) {
        LpJson *d = lp_json_at(j, i);
        const char *model = lp_json_str(d, "model", "?");
        const char *size = lp_json_str(d, "size_text", "");
        const char *tran = lp_json_str(d, "transport", "");
        const char *reason = lp_json_str(d, "reason", "");
        int parts = (int)lp_json_num(d, "partitions", 0);
        gboolean ok = lp_json_bool(d, "usable", 0);
        gboolean here = lp_json_bool(d, "inplace", 0);
        /* Not to be erased, but with a partition or free space for LP. */
        gboolean shared = lp_json_bool(d, "part_ok", 0) || lp_json_bool(d, "free_ok", 0);
        const char *need = lp_json_str(d, "inplace_need_text", "");

        char *den = g_strdup_printf("%s  ·  %s  ·  %s", size, transport_name(tran, FALSE),
                                    lp_json_str(d, "path", ""));
        char *dko = g_strdup_printf("%s  ·  %s  ·  %s", size, transport_name(tran, TRUE),
                                    lp_json_str(d, "path", ""));
        if (here) {
            /* The disk LP runs from, big enough to stay on: nothing on it
             * is erased, LP grows to fill it (lp-install install_inplace). */
            char *e2 = g_strdup_printf("%s  —  LP is running from it: installs here, "
                                       "nothing is erased", den);
            char *k2 = g_strdup_printf("%s  —  지금 LP 가 도는 디스크: 지우지 않고 "
                                       "여기에 설치합니다", dko);
            g_free(den); g_free(dko);
            den = e2; dko = k2;
        } else if (!ok && !strcmp(reason, "running") && *need) {
            char *e2 = g_strdup_printf("%s  —  LP is running from it; make it %s or "
                                       "larger to install here", den, need);
            char *k2 = g_strdup_printf("%s  —  지금 LP 가 도는 디스크입니다. %s 이상으로 "
                                       "늘리면 여기에 설치할 수 있습니다", dko, need);
            g_free(den); g_free(dko);
            den = e2; dko = k2;
        } else if (!ok) {
            const char *wen, *wko;
            disk_reason_words(reason, &wen, &wko);
            char *e2 = g_strdup_printf(shared ? "%s  —  %s; a partition or free space on "
                                                "it can still take LP" : "%s  —  %s", den, wen);
            char *k2 = g_strdup_printf(shared ? "%s  —  %s. 파티션이나 빈 공간에는 설치할 "
                                                "수 있습니다" : "%s  —  %s", dko, wko);
            g_free(den); g_free(dko);
            den = e2; dko = k2;
        }
        GtkWidget *b = su_choice(model, model, den, dko,
                                 !strcmp(tran, "usb") ? "drive-removable-media"
                                                      : "drive-harddisk");
        g_free(den); g_free(dko);
        ok = ok || here || shared;
        /* A disk with partitions opens even when nothing on it can take
         * LP, so its partitions can be seen with the reason for each. */
        gtk_widget_set_sensitive(b, ok || (parts > 0 && strcmp(reason, "running")));
        g_object_set_data_full(G_OBJECT(b), "path", g_strdup(lp_json_str(d, "path", "")), g_free);
        g_object_set_data_full(G_OBJECT(b), "model", g_strdup(model), g_free);
        g_object_set_data_full(G_OBJECT(b), "size", g_strdup(size), g_free);
        g_object_set_data(G_OBJECT(b), "parts", GINT_TO_POINTER(parts));
        g_object_set_data(G_OBJECT(b), "inplace", GINT_TO_POINTER(here));
        g_object_set_data(G_OBJECT(b), "json", d);
        if (A.disk_group)
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(A.disk_group));
        else
            A.disk_group = b;
        g_signal_connect(b, "toggled", G_CALLBACK(on_disk_toggled), NULL);
        gtk_box_append(GTK_BOX(A.disk_box), revealed(b, i));
        if (ok && ++usable == 1)
            only = b;
    }
    /* One usable disk - the laptop's own - is the usual case, and then it
     * is chosen already. With several, the person picks: preselecting
     * one of two disks is choosing which one to erase for them. */
    if (usable == 1)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(only), TRUE);
    if (n == 0)
        gtk_box_append(GTK_BOX(A.disk_box), su_label("No disks were found.",
                                                     "디스크를 찾지 못했습니다.", "su-warn"));
    g_free(out);
}

static void show_parts(void);
static void show_confirm(void);

static void on_disk_next(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (A.mode == MODE_PART) {
        show_parts();
        su_go(A.stack, "parts", TRUE);
    } else if (A.mode != MODE_NONE) {
        show_confirm();
        su_go(A.stack, "confirm", TRUE);
    }
}

static GtkWidget *list_box(GtkWidget **box, int max_height)
{
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_widget_add_css_class(sw, "su-list");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), max_height);
    gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(sw), TRUE);
    *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), *box);
    return sw;
}

static void page_disks(void)
{
    SuPage p;
    su_page(&p, "Where should LP go?", "LP 를 어디에 설치할까요?",
            "Choose the disk, then how: all of it, erased for LP - or one partition "
            "or its free space, with the rest of the disk left as it is.",
            "디스크를 고른 다음 설치 방법을 고르세요. 디스크 전체를 지우고 설치하거나, "
            "파티션 하나나 빈 공간에 설치하고 나머지는 그대로 둘 수 있습니다.");
    /* Room for three disks; the ways to install go under the list, not
     * inside it, so they are never scrolled out of sight. */
    gtk_box_append(GTK_BOX(p.body), list_box(&A.disk_box, 250));

    GtkWidget *modes = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(modes), su_label("How should LP go on this disk?",
                                            "이 디스크에 어떻게 설치할까요?", "su-caption"));
    A.mode_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(modes), A.mode_box);
    A.mode_reveal = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(A.mode_reveal),
        lp_motion_reduced() ? GTK_REVEALER_TRANSITION_TYPE_CROSSFADE
                            : GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(A.mode_reveal),
        lp_motion_reduced() ? 90 : lp_spring_ms(LP_SPRING_INSERT, FALSE));
    gtk_revealer_set_child(GTK_REVEALER(A.mode_reveal), modes);
    gtk_box_append(GTK_BOX(p.body), A.mode_reveal);

    GtkWidget *back = su_button("Back", "뒤로", "su-secondary");
    g_signal_connect(back, "clicked", G_CALLBACK(on_back_region), NULL);
    gtk_box_append(GTK_BOX(p.left), back);
    A.disk_next = su_button("Next", "다음", "su-primary");
    g_signal_connect(A.disk_next, "clicked", G_CALLBACK(on_disk_next), NULL);
    gtk_box_append(GTK_BOX(p.right), A.disk_next);
    gtk_stack_add_named(GTK_STACK(A.stack), p.root, "disks");
}

/* ── parts ─────────────────────────────────────────────────────────── */

static void on_part_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    if (!gtk_toggle_button_get_active(b))
        return;
    g_strlcpy(A.part, g_object_get_data(G_OBJECT(b), "dev"), sizeof A.part);
    g_free(A.part_en);
    g_free(A.part_ko);
    A.part_en = g_strdup(g_object_get_data(G_OBJECT(b), "en"));
    A.part_ko = g_strdup(g_object_get_data(G_OBJECT(b), "ko"));
    gtk_widget_set_sensitive(A.part_next, TRUE);
}

/* The partitions of the chosen disk, in table order. What each holds is
 * said the way a person knows it - its filesystem and label, "Windows
 * recovery" rather than a type GUID - because the row tapped here is the
 * one erased. The ones LP cannot go into are shown greyed with the
 * reason: a Windows disk where only the EFI partition is listed would
 * leave the person wondering where their C: went. Nothing preselected. */
static void show_parts(void)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(A.part_box)))
        gtk_box_remove(GTK_BOX(A.part_box), c);
    A.part[0] = '\0';
    A.part_group = NULL;
    gtk_widget_set_sensitive(A.part_next, FALSE);

    LpJson *parts = A.disk_j ? lp_json_get(A.disk_j, "parts") : NULL;
    int n = parts ? lp_json_len(parts) : 0;
    for (int i = 0; i < n; i++) {
        LpJson *p = lp_json_at(parts, i);
        const char *dev = lp_json_str(p, "dev", "");
        const char *fs = lp_json_str(p, "fstype", "");
        const char *label = lp_json_str(p, "label", "");
        const char *tname = lp_json_str(p, "type_name", "");
        const char *size = lp_json_str(p, "size_text", "");
        gboolean ok = lp_json_bool(p, "usable", 0);
        if (!*label)
            label = lp_json_str(p, "partlabel", "");

        /* ntfs "Data" - what the confirmation and the row both say. */
        char *qlabel = *label ? g_strdup_printf(" \"%s\"", label) : g_strdup("");
        char *what_en = g_strdup_printf("%s%s", *fs ? fs : "no file system", qlabel);
        char *what_ko = g_strdup_printf("%s%s", *fs ? fs : "파일 시스템 없음", qlabel);
        GString *en = g_string_new(what_en), *ko = g_string_new(what_ko);
        if (*tname) {
            g_string_append_printf(en, "  ·  %s", tname);
            g_string_append_printf(ko, "  ·  %s", tname);
        }
        const char *pr = lp_json_str(p, "reason", "");
        if (ok && lp_json_bool(p, "own_esp", 0)) {
            g_string_append(en, "  —  LP can go here, with its own 512 MB start-up "
                                "partition made inside it");
            g_string_append(ko, "  —  여기에 설치할 수 있습니다. 이 안에 LP 전용 512MB 시작 "
                                "파티션을 만듭니다");
        } else if (ok) {
            g_string_append(en, "  —  LP can go here");
            g_string_append(ko, "  —  여기에 설치할 수 있습니다");
        } else {
            char *ren, *rko;
            reason_words(pr, A.disk_j, lp_json_str(p, "mount", ""), &ren, &rko);
            g_string_append_printf(en, "  —  %s", ren);
            g_string_append_printf(ko, "  —  %s", rko);
            g_free(ren); g_free(rko);
            /* Big enough for LP, not for LP and its own EFI partition. */
            if (g_str_has_prefix(pr, "esp-") || !strcmp(pr, "no-esp")) {
                g_string_append(en, ". 512 MB more and LP would make its own start-up "
                                    "partition inside it");
                g_string_append(ko, ". 512MB 더 크면 이 안에 LP 전용 시작 파티션을 만들 수 "
                                    "있습니다");
            }
        }
        char *title = g_strdup_printf("%s  ·  %s", lp_json_str(p, "name", ""), size);
        GtkWidget *b = su_choice(title, title, en->str, ko->str, NULL);
        gtk_widget_set_sensitive(b, ok);
        g_object_set_data_full(G_OBJECT(b), "dev", g_strdup(dev), g_free);
        g_object_set_data_full(G_OBJECT(b), "en",
                               g_strdup_printf("%s (%s, %s)", dev, size, what_en), g_free);
        g_object_set_data_full(G_OBJECT(b), "ko",
                               g_strdup_printf("%s (%s, %s)", dev, size, what_ko), g_free);
        if (A.part_group)
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(A.part_group));
        else
            A.part_group = b;
        g_signal_connect(b, "toggled", G_CALLBACK(on_part_toggled), NULL);
        gtk_box_append(GTK_BOX(A.part_box), revealed(b, i));
        g_free(title);
        g_string_free(en, TRUE);
        g_string_free(ko, TRUE);
        g_free(what_en);
        g_free(what_ko);
        g_free(qlabel);
    }
    if (n == 0)
        gtk_box_append(GTK_BOX(A.part_box), su_label("This disk has no partitions.",
                                                     "이 디스크에는 파티션이 없습니다.",
                                                     "su-warn"));
}

static void on_parts_back(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    su_go(A.stack, "disks", FALSE);
}

static void on_parts_next(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    if (!A.part[0])
        return;
    show_confirm();
    su_go(A.stack, "confirm", TRUE);
}

static void page_parts(void)
{
    SuPage p;
    su_page(&p, "Which partition should LP go into?", "어느 파티션에 설치할까요?",
            "The partition you choose is erased and becomes LP's. The other partitions "
            "on the disk stay as they are.",
            "고른 파티션의 내용은 지워지고 그 자리에 LP 가 설치됩니다. 디스크의 나머지 "
            "파티션은 그대로 둡니다.");
    gtk_box_append(GTK_BOX(p.body), list_box(&A.part_box, 420));
    GtkWidget *back = su_button("Back", "뒤로", "su-secondary");
    g_signal_connect(back, "clicked", G_CALLBACK(on_parts_back), NULL);
    gtk_box_append(GTK_BOX(p.left), back);
    A.part_next = su_button("Next", "다음", "su-primary");
    g_signal_connect(A.part_next, "clicked", G_CALLBACK(on_parts_next), NULL);
    gtk_box_append(GTK_BOX(p.right), A.part_next);
    gtk_stack_add_named(GTK_STACK(A.stack), p.root, "parts");
}

/* A wrapping label asks for the width of its whole text on one line, and
 * the page stack is as wide as its widest page: the confirmation's long
 * notes made every page's card as wide as the screen. A small natural
 * width (the "character" is the font's average, which with Hangul in it is
 * wide) lets the other pages set the card's width; the label still fills
 * that width and wraps there. */
static GtkWidget *narrow(GtkWidget *label)
{
    gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
    return label;
}

/* One row of the plan: what the install does to one partition, in the
 * words of lp-install's own plan (install --dry-run --json) - the list a
 * person agrees to is the one the backend computed, not a paraphrase. */
static void plan_row(LpJson *r, gboolean own_esp)
{
    const char *act = lp_json_str(r, "action", "keep");
    const char *dev = lp_json_str(r, "dev", "");
    const char *name = lp_json_str(r, "name", "");
    const char *size = lp_json_str(r, "size_text", "");
    const char *what = lp_json_str(r, "what", "");
    const char *ten, *tko, *cls;
    if (!strcmp(act, "erase")) {
        ten = "Erased"; tko = "지움"; cls = "lp-tag-erase";
    } else if (!strcmp(act, "new")) {
        ten = "New"; tko = "새로 만듦"; cls = "lp-tag-new";
    } else if (!strcmp(act, "esp-add")) {
        ten = "Adds \\EFI\\LP"; tko = "\\EFI\\LP 추가"; cls = "lp-tag-add";
    } else if (!strcmp(act, "grow")) {
        ten = "Grows"; tko = "늘어남"; cls = "lp-tag-new";
    } else {
        ten = "Kept"; tko = "그대로"; cls = "lp-tag-keep";
    }
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(row, "lp-plan-row");
    GtkWidget *tag = su_label(ten, tko, "lp-plan-tag");
    gtk_widget_add_css_class(tag, cls);
    gtk_label_set_wrap(GTK_LABEL(tag), FALSE);
    gtk_label_set_xalign(GTK_LABEL(tag), 0.5);
    gtk_widget_set_size_request(tag, 140, -1);
    gtk_widget_set_valign(tag, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), tag);

    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(col, TRUE);
    char *title, *wen, *wko;
    if (!strcmp(act, "new")) {
        title = g_strdup_printf("%s  ·  %s", name, size);
        wen = g_strdup(what);
        /* LP-ROOT-2 and the like too: a disk that has LP's names already. */
        wko = g_strdup(g_str_has_prefix(name, "LP-ESP") ? (own_esp ? "FAT32 · LP 전용 EFI 시작 파티션"
                                                                   : "FAT32 · EFI 시작 파티션")
                       : g_str_has_prefix(name, "LP-RECOVERY") ? "ext4 · LP 복구 시스템"
                       : g_str_has_prefix(name, "LP-ROOT") ? "ext4 · LP" : what);
    } else {
        title = g_strdup_printf("%s  ·  %s", dev, size);
        if (!strcmp(act, "erase") && *name) {
            wen = g_strdup_printf("%s  →  %s", what, name);
            wko = g_strdup_printf("%s  →  %s", what, name);
        } else if (!strcmp(lp_json_str(r, "note", ""), "lp-unstarted")) {
            /* Another LP starts from the EFI partition this install takes
             * \EFI\LP from: kept, but it will not start any more. */
            wen = g_strdup_printf("%s  —  the LP here no longer starts; its files are kept",
                                  what);
            wko = g_strdup_printf("%s  —  이 LP 는 더 이상 시작되지 않습니다. 파일은 그대로 "
                                  "둡니다", what);
        } else {
            wen = g_strdup(what);
            wko = g_strdup(what);
        }
    }
    gtk_box_append(GTK_BOX(col), su_label(title, title, "lp-plan-title"));
    gtk_box_append(GTK_BOX(col), narrow(su_label(wen, wko, "su-choice-detail")));
    gtk_box_append(GTK_BOX(row), col);
    gtk_box_append(GTK_BOX(A.confirm_plan), row);
    g_free(title);
    g_free(wen);
    g_free(wko);
}

/* The last page before anything is written: the backend's plan for the
 * way chosen - every partition of the disk, erased, made, or kept - and
 * what LP does to the disk's start-up. Nothing on the disk has been
 * touched to make it (--dry-run). */
static void show_confirm(void)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(A.confirm_plan)))
        gtk_box_remove(GTK_BOX(A.confirm_plan), c);
    const char *flag = A.mode == MODE_PART ? "--partition"
                     : A.mode == MODE_FREE ? "--free-space" : "--disk";
    const char *where = A.mode == MODE_PART ? A.part : A.disk;
    const char *argv[] = { backend(), "install", flag, where, "--dry-run", "--json",
                           "--progress", NULL };
    char *out = su_run(argv, NULL);
    LpJson *plan = out && out[0] == '{' ? lp_json_parse(out) : NULL;
    A.plan_ok = plan != NULL;

    gboolean esp_shared = plan && lp_json_bool(plan, "esp_shared", 0);
    gboolean fb_other = plan && lp_json_bool(plan, "fallback_other", 0);
    gboolean recovery = plan && lp_json_bool(plan, "recovery", 0);
    gboolean had_lp = plan && lp_json_bool(plan, "esp_has_lp", 0);
    const char *dsk_esp = A.disk_j ? lp_json_str(A.disk_j, "esp", "") : "";
    const char *esp = plan ? lp_json_str(plan, "esp", "") : "";
    LpJson *rows = plan ? lp_json_get(plan, "rows") : NULL;
    gboolean new_esp = FALSE;
    for (int i = 0; rows && i < lp_json_len(rows); i++)
        if (!strcmp(lp_json_str(lp_json_at(rows, i), "name", ""), "LP-ESP"))
            new_esp = TRUE;
    for (int i = 0; rows && i < lp_json_len(rows); i++)
        plan_row(lp_json_at(rows, i), TRUE);

    char *wen, *wko, *nen, *nko;
    const char *lp_en = had_lp && esp_shared ? " The LP start-up files already there are replaced." : "";
    const char *lp_ko = had_lp && esp_shared ? " 그곳에 있던 LP 시작 파일은 새것으로 바뀝니다." : "";
    const char *fb_en = fb_other
        ? " The other system's start-up file (\\EFI\\BOOT\\BOOTX64.EFI) stays as it is; "
          "the computer gets a start-up entry for LP."
        : " \\EFI\\BOOT\\BOOTX64.EFI was free, so LP's start-up menu goes there too.";
    const char *fb_ko = fb_other
        ? " 다른 시스템의 시작 파일(\\EFI\\BOOT\\BOOTX64.EFI)은 그대로 두고, 컴퓨터에 LP "
          "시작 항목을 더합니다."
        : " \\EFI\\BOOT\\BOOTX64.EFI 자리가 비어 있어서 LP 시작 메뉴를 그곳에도 둡니다.";
    /* LP's boot menu starts LP and nothing else, and LP goes first in the
     * firmware's order: the other system is one key away, and the person
     * should know which key before they need it. */
    const char *other_en = A.disk_parts ? " From now on LP starts first; the other system "
                                          "is in the start-up menu (F12 at power-on)." : "";
    const char *other_ko = A.disk_parts ? " 이제부터는 LP 가 먼저 시작합니다. 다른 시스템은 "
                                          "켤 때 F12 를 누르면 나오는 시작 메뉴에서 고르세요." : "";
    /* Another LP that starts from the disk's EFI partition today (its
     * \EFI\LP names that LP's root): a partition install takes \EFI\LP
     * over, and the plan names that LP's partition. */
    const char *other_lp = plan ? lp_json_str(plan, "esp_lp_dev", "") : "";
    const char *why = plan ? lp_json_str(plan, "new_esp_why", "") : "";
    LpJson *taken = A.disk_j ? lp_json_get(A.disk_j, "lp_names") : NULL;
    gboolean lp_there = taken && lp_json_len(taken) > 0;
    char *what = g_strdup_printf("%s  ·  %s  ·  %s", A.disk_model, A.disk_size, A.disk);
    su_retext(A.confirm_what, what, what);
    g_free(what);

    gboolean split = plan && lp_json_bool(plan, "split", 0);
    if (A.mode == MODE_PART) {
        wen = g_strdup_printf("Everything on %s will be erased. The other partitions "
                              "are left as they are.", A.part_en);
        wko = g_strdup_printf("%s의 내용이 모두 지워집니다. 나머지 파티션은 그대로 "
                              "둡니다.", A.part_ko);
        if (split) {
            /* The disk's EFI partition cannot start LP: LP's own go where
             * the chosen partition was. */
            const char *een = !strcmp(why, "esp-full")
                ? "The disk's EFI system partition is too full for LP's start-up files"
                : !strcmp(why, "esp-mounted") ? "The disk's EFI system partition is in use"
                : !strcmp(why, "esp-bad") ? "The disk's EFI system partition cannot be read"
                : "The disk has no EFI system partition";
            const char *eko = !strcmp(why, "esp-full")
                ? "이 디스크의 EFI 시스템 파티션에는 LP 시작 파일을 둘 자리가 없습니다"
                : !strcmp(why, "esp-mounted") ? "이 디스크의 EFI 시스템 파티션이 사용 중입니다"
                : !strcmp(why, "esp-bad") ? "이 디스크의 EFI 시스템 파티션을 읽을 수 없습니다"
                : "이 디스크에는 EFI 시스템 파티션이 없습니다";
            nen = g_strdup_printf("%s, so in its place LP makes a 512 MB start-up partition of "
                                  "its own%s and LP itself; the disk's EFI partition is not "
                                  "touched.%s%s", een,
                                  recovery ? ", its recovery system" : "",
                                  recovery ? "" : " There is no room for the recovery system.",
                                  other_en);
            nko = g_strdup_printf("%s. 그래서 이 파티션 자리에 LP 전용 512MB 시작 "
                                  "파티션%s과 LP 를 만듭니다. 디스크의 EFI 파티션은 건드리지 "
                                  "않습니다.%s%s", eko, recovery ? ", 복구 시스템" : "",
                                  recovery ? "" : " 복구 시스템을 둘 자리는 없습니다.",
                                  other_ko);
        } else if (new_esp) {
            nen = g_strdup_printf("This disk has no EFI system partition, so a 512 MB one "
                                  "is made for LP in its free space. No recovery partition is "
                                  "made, so LP Recovery is not offered at start-up (the USB "
                                  "stick can reinstall LP).%s", other_en);
            nko = g_strdup_printf("이 디스크에는 EFI 시스템 파티션이 없어서 빈 공간에 LP 용 "
                                  "512MB 시작 파티션을 만듭니다. 복구 파티션은 만들지 "
                                  "않으므로 시작할 때 LP 복구는 나오지 않습니다 (USB 로 다시 "
                                  "설치할 수 있습니다).%s", other_ko);
        } else {
            nen = g_strdup_printf("LP starts from this disk's EFI system partition (%s): it "
                                  "adds its own folder, \\EFI\\LP, and changes nothing else "
                                  "there.%s%s No recovery partition is made, so LP Recovery is "
                                  "not offered at start-up (the USB stick can reinstall LP).%s",
                                  esp, lp_en, fb_en, other_en);
            nko = g_strdup_printf("LP 는 이 디스크의 EFI 시스템 파티션(%s)에 자기 폴더 "
                                  "\\EFI\\LP 만 더해서 시작하고, 그 밖의 것은 바꾸지 "
                                  "않습니다.%s%s 복구 파티션은 만들지 않으므로 시작할 때 LP 복구는 "
                                  "나오지 않습니다 (USB 로 다시 설치할 수 있습니다).%s", esp,
                                  lp_ko, fb_ko, other_ko);
        }
        if (*other_lp) {
            char *e2 = g_strdup_printf("%s The LP on %s starts from this EFI partition now: "
                                       "after this install it no longer starts (its "
                                       "partition is kept).", nen, other_lp);
            char *k2 = g_strdup_printf("%s 지금은 %s 의 LP 가 이 EFI 파티션으로 시작합니다. "
                                       "설치한 뒤에는 그 LP 는 시작되지 않습니다 (파티션은 "
                                       "그대로 둡니다).", nko, other_lp);
            g_free(nen); g_free(nko);
            nen = e2; nko = k2;
        }
        su_retext(A.confirm_title, "Erase this partition and install LP?",
                  "이 파티션을 지우고 LP 를 설치할까요?");
        su_retext(A.confirm_check, "I understand that everything on this partition will be erased",
                  "이 파티션의 내용이 모두 지워진다는 것을 이해했습니다");
        su_retext(A.confirm_go, "Erase and install", "지우고 설치");
        gtk_widget_remove_css_class(A.confirm_go, "su-primary");
        gtk_widget_add_css_class(A.confirm_go, "su-danger");
    } else if (A.mode == MODE_FREE) {
        const char *sz = plan ? lp_json_str(plan, "free", "") : "";
        wen = g_strdup_printf("New partitions for LP are made in the %s of free space on "
                              "%s. Nothing on the other partitions is erased.", sz, A.disk);
        wko = g_strdup_printf("%s 의 빈 공간 %s 에 LP 용 새 파티션을 만듭니다. 다른 "
                              "파티션의 내용은 지우지 않습니다.", A.disk, sz);
        if (recovery) {
            nen = g_strdup_printf("LP gets its own start-up partition and its recovery "
                                  "system%s%s%s", *dsk_esp ? "; the disk's EFI system "
                                  "partition (" : ".", *dsk_esp ? dsk_esp : "",
                                  *dsk_esp ? ") is not touched." : "");
            nko = g_strdup_printf("LP 전용 시작 파티션과 복구 시스템을 함께 만듭니다.%s%s%s",
                                  *dsk_esp ? " 이 디스크의 EFI 시스템 파티션(" : "",
                                  *dsk_esp ? dsk_esp : "",
                                  *dsk_esp ? ")은 건드리지 않습니다." : "");
            char *e2 = g_strconcat(nen, other_en, NULL), *k2 = g_strconcat(nko, other_ko, NULL);
            g_free(nen); g_free(nko);
            nen = e2; nko = k2;
        } else if (new_esp) {
            /* LP's own EFI partition without a recovery system: why. */
            const char *een, *eko;
            if (!strcmp(why, "other-lp")) {
                een = "The disk's EFI system partition already starts another LP, so this "
                      "one gets a 512 MB start-up partition of its own and the other keeps "
                      "starting.";
                eko = "이 디스크의 EFI 시스템 파티션은 이미 다른 LP 를 시작하고 있어서, 이 LP 는 "
                      "512MB 전용 시작 파티션을 따로 만들고 다른 LP 는 그대로 시작합니다.";
            } else if (!strcmp(why, "esp-full")) {
                een = "The disk's EFI system partition is too full for LP's start-up files, so "
                      "LP gets a 512 MB one of its own; the disk's is not touched.";
                eko = "이 디스크의 EFI 시스템 파티션에 LP 시작 파일을 둘 자리가 없어서 LP 전용 "
                      "512MB 시작 파티션을 만듭니다. 원래 것은 건드리지 않습니다.";
            } else if (*why && strcmp(why, "none")) {
                een = "The disk's EFI system partition cannot be used, so LP gets a 512 MB one "
                      "of its own; the disk's is not touched.";
                eko = "이 디스크의 EFI 시스템 파티션을 쓸 수 없어서 LP 전용 512MB 시작 파티션을 "
                      "만듭니다. 원래 것은 건드리지 않습니다.";
            } else {
                een = "The disk has no EFI system partition, so a 512 MB one is made for LP.";
                eko = "이 디스크에는 EFI 시스템 파티션이 없어서 LP 용 512MB 시작 파티션을 "
                      "만듭니다.";
            }
            nen = g_strdup_printf("%s %s%s", een, lp_there
                                  ? "No recovery system: this disk has LP partitions already."
                                  : "There is no room for the recovery system.", other_en);
            nko = g_strdup_printf("%s %s%s", eko, lp_there
                                  ? "복구 시스템은 없습니다: 이 디스크에는 이미 LP 파티션이 "
                                    "있습니다."
                                  : "복구 시스템을 둘 자리는 없습니다.", other_ko);
        } else {
            nen = g_strdup_printf("LP starts from this disk's EFI system partition (%s): it "
                                  "adds its own folder, \\EFI\\LP, and changes nothing else "
                                  "there.%s%s %s%s",
                                  esp, lp_en, fb_en, lp_there ? "No recovery system: this disk "
                                  "has LP partitions already." : "There is no room for the "
                                  "recovery system.", other_en);
            nko = g_strdup_printf("LP 는 이 디스크의 EFI 시스템 파티션(%s)에 자기 폴더 "
                                  "\\EFI\\LP 만 더해서 시작하고, 그 밖의 것은 바꾸지 "
                                  "않습니다.%s%s %s%s", esp,
                                  lp_ko, fb_ko, lp_there ? "복구 시스템은 없습니다: 이 "
                                  "디스크에는 이미 LP 파티션이 있습니다." : "복구 시스템을 둘 "
                                  "자리는 없습니다.", other_ko);
        }
        su_retext(A.confirm_title, "Install LP in the free space?",
                  "빈 공간에 LP 를 설치할까요?");
        su_retext(A.confirm_check, "I understand that LP will be installed in the free "
                  "space of this disk", "이 디스크의 빈 공간에 LP 가 설치된다는 것을 "
                  "이해했습니다");
        su_retext(A.confirm_go, "Install", "설치");
        gtk_widget_remove_css_class(A.confirm_go, "su-danger");
        gtk_widget_add_css_class(A.confirm_go, "su-primary");
    } else {
        nen = g_strdup("LP uses the whole disk: 512 MB to start up, about 1.3 GB for the "
                       "recovery system, and the rest for LP and your files.");
        nko = g_strdup("LP 가 디스크 전체를 씁니다: 시작용 512MB, 복구 시스템 약 1.3GB, "
                       "나머지는 LP 와 내 파일에 씁니다.");
        if (A.disk_inplace) {
            wen = g_strdup_printf(
                "LP is installed on %s, the disk it is running from. Nothing is "
                "erased: LP grows to fill the disk and becomes the installed system.", A.disk);
            wko = g_strdup_printf(
                "지금 LP 가 도는 디스크 %s 에 설치합니다. 아무것도 지우지 않습니다: "
                "LP 가 디스크 전체로 늘어나 설치된 시스템이 됩니다.", A.disk);
            su_retext(A.confirm_title, "Install LP on this disk?", "이 디스크에 LP 를 설치할까요?");
            su_retext(A.confirm_check, "I understand that LP will be installed on this disk",
                      "이 디스크에 LP 가 설치된다는 것을 이해했습니다");
            su_retext(A.confirm_go, "Install", "설치");
            gtk_widget_remove_css_class(A.confirm_go, "su-danger");
            gtk_widget_add_css_class(A.confirm_go, "su-primary");
        } else {
            wen = g_strdup_printf(
                "Everything on %s will be deleted%s. This cannot be undone.", A.disk,
                A.disk_parts ? ", including the partitions on it now" : "");
            wko = g_strdup_printf(
                "%s 의 모든 것이 지워집니다%s. 되돌릴 수 없습니다.", A.disk,
                A.disk_parts ? " (지금 있는 파티션 포함)" : "");
            su_retext(A.confirm_title, "Erase this disk and install LP?",
                      "이 디스크를 지우고 LP 를 설치할까요?");
            su_retext(A.confirm_check, "I understand that everything on this disk will be erased",
                      "이 디스크의 모든 것이 지워진다는 것을 이해했습니다");
            su_retext(A.confirm_go, "Erase and install", "지우고 설치");
            gtk_widget_remove_css_class(A.confirm_go, "su-primary");
            gtk_widget_add_css_class(A.confirm_go, "su-danger");
        }
    }
    if (!plan) {
        /* The backend refused the way chosen, or could not say: nothing
         * can be agreed to, and its words are what the page shows. */
        const char *e = out && g_str_has_prefix(out, "ERROR ") ? out + 6 : "";
        const char *sp = strchr(e, ' ');
        char *msg = g_strstrip(g_strdup(sp ? sp + 1 : e));
        g_free(wen); g_free(wko);
        wen = g_strdup_printf("LP cannot go there: %s", *msg ? msg : "lp-install gave no plan");
        wko = g_strdup_printf("여기에는 설치할 수 없습니다: %s",
                              *msg ? msg : "lp-install 이 계획을 내놓지 않았습니다");
        g_free(msg);
    }
    su_retext(A.confirm_warn, wen, wko);
    su_retext(A.confirm_note, nen, nko);
    gtk_widget_set_visible(A.confirm_note, plan != NULL);
    g_free(wen); g_free(wko);
    g_free(nen); g_free(nko);
    char *sen = g_strdup_printf("Account %s  ·  %s  ·  computer name %s",
                                su_account_login(&A.acct), su_region_timezone(&A.region),
                                su_region_hostname(&A.region));
    char *sko = g_strdup_printf("계정 %s  ·  %s  ·  컴퓨터 이름 %s",
                                su_account_login(&A.acct), su_region_timezone(&A.region),
                                su_region_hostname(&A.region));
    su_retext(A.confirm_who, sen, sko);
    g_free(sen); g_free(sko);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(A.confirm_check), FALSE);
    gtk_widget_set_sensitive(A.confirm_check, A.plan_ok);
    gtk_widget_set_sensitive(A.confirm_go, FALSE);
    if (plan)
        lp_json_free(plan);
    g_free(out);
}

/* ── confirm ───────────────────────────────────────────────────────── */

static void on_check(GtkCheckButton *c, gpointer d)
{
    (void)d;
    gtk_widget_set_sensitive(A.confirm_go, A.plan_ok && gtk_check_button_get_active(c));
}

static void on_back_disks(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    su_go(A.stack, A.mode == MODE_PART ? "parts" : "disks", FALSE);
}

static void start_install(void);

static void on_go(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    start_install();
}

static void page_confirm(void)
{
    SuPage p;
    su_page(&p, "Erase this disk and install LP?", "이 디스크를 지우고 LP 를 설치할까요?",
            NULL, NULL);
    A.confirm_title = p.title;
    A.confirm_what = su_label("", "", "su-choice-title");
    gtk_box_append(GTK_BOX(p.body), A.confirm_what);
    A.confirm_warn = narrow(su_label("", "", "su-warn"));
    gtk_box_append(GTK_BOX(p.body), A.confirm_warn);
    /* The disk, partition by partition: erased, made, kept. */
    gtk_box_append(GTK_BOX(p.body), list_box(&A.confirm_plan, 300));
    gtk_box_set_spacing(GTK_BOX(A.confirm_plan), 6);
    /* What LP does to the disk's start-up, which depends on the way chosen. */
    A.confirm_note = narrow(su_label("", "", "su-note"));
    gtk_box_append(GTK_BOX(p.body), A.confirm_note);
    A.confirm_who = su_label("", "", "su-note");
    gtk_box_append(GTK_BOX(p.body), A.confirm_who);
    A.confirm_check = gtk_check_button_new();
    gtk_widget_add_css_class(A.confirm_check, "su-check");
    su_retext(A.confirm_check, "I understand that everything on this disk will be erased",
              "이 디스크의 모든 것이 지워진다는 것을 이해했습니다");
    g_signal_connect(A.confirm_check, "toggled", G_CALLBACK(on_check), NULL);
    gtk_box_append(GTK_BOX(p.body), A.confirm_check);

    GtkWidget *back = su_button("Back", "뒤로", "su-secondary");
    g_signal_connect(back, "clicked", G_CALLBACK(on_back_disks), NULL);
    gtk_box_append(GTK_BOX(p.left), back);
    A.confirm_go = su_button("Erase and install", "지우고 설치", "su-danger");
    gtk_widget_set_sensitive(A.confirm_go, FALSE);
    g_signal_connect(A.confirm_go, "clicked", G_CALLBACK(on_go), NULL);
    gtk_box_append(GTK_BOX(p.right), A.confirm_go);
    gtk_stack_add_named(GTK_STACK(A.stack), p.root, "confirm");
}

/* ── progress ──────────────────────────────────────────────────────── */

static void step_text(const char *key)
{
    static const struct { const char *key, *en, *ko; } STEPS[] = {
        { "partition", "Partitioning the disk",       "디스크를 나누는 중" },
        { "format",    "Formatting",                  "포맷하는 중" },
        { "copy",      "Copying LP",                  "LP 를 복사하는 중" },
        { "recovery",  "Setting up the recovery system", "복구 시스템을 준비하는 중" },
        { "boot",      "Making the disk start up",    "시작 준비를 하는 중" },
        { "finish",    "Finishing",                   "마무리하는 중" },
    };
    /* Into a partition, the table is not remade: one entry is named. */
    if (!strcmp(key, "partition") && A.mode == MODE_PART) {
        su_retext(A.step, "Preparing the partition", "파티션을 준비하는 중");
        return;
    }
    if (!strcmp(key, "partition") && A.mode == MODE_FREE) {
        su_retext(A.step, "Making LP's partitions in the free space",
                  "빈 공간에 LP 파티션을 만드는 중");
        return;
    }
    for (guint i = 0; i < G_N_ELEMENTS(STEPS); i++)
        if (!strcmp(key, STEPS[i].key))
            su_retext(A.step, STEPS[i].en, STEPS[i].ko);
}

static void finished(gboolean ok)
{
    if (A.lines)
        g_clear_object(&A.lines);
    if (ok) {
        char *en = g_strdup_printf(
            "After the restart LP asks once for a keyboard and Wi-Fi, and then "
            "it is yours. Your account is %s.", su_account_login(&A.acct));
        char *ko = g_strdup_printf(
            "다시 시작하면 LP 가 키보드와 Wi-Fi 를 한 번 묻고, 그 다음부터는 바로 쓸 수 "
            "있습니다. 계정은 %s 입니다.", su_account_login(&A.acct));
        su_retext(A.done_text, en, ko);
        g_free(en); g_free(ko);
        /* Installed where it runs: there is no stick to pull out. */
        if (A.disk_inplace)
            su_retext(A.done_sub, "Restart, and LP starts from this disk as installed.",
                      "다시 시작하면 LP 가 설치된 시스템으로 시작합니다.");
        su_go(A.stack, "done", TRUE);
    } else {
        char *en = g_strdup_printf("lp-install said: %s", A.last_error[0] ? A.last_error
                                                                           : "(nothing)");
        char *ko = g_strdup_printf("lp-install 의 메시지: %s", A.last_error[0] ? A.last_error
                                                                            : "(없음)");
        su_retext(A.fail_text, en, ko);
        g_free(en); g_free(ko);
        su_go(A.stack, "failed", TRUE);
    }
}

static void on_line(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)d;
    gsize len = 0;
    GError *err = NULL;
    char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res,
                                                      &len, &err);
    if (!line) {
        g_clear_error(&err);
        gboolean ok = A.proc && g_subprocess_wait_check(A.proc, NULL, NULL);
        g_clear_object(&A.proc);
        finished(ok);
        return;
    }
    /* PROGRESS <pct> <step> | STEP <step> <text> | ERROR <key> <text> | DONE ... */
    char **f = g_strsplit(line, " ", 3);
    if (f[0] && f[1]) {
        if (!strcmp(f[0], "PROGRESS")) {
            int pct = atoi(f[1]);
            su_progress_set(A.bar, pct / 100.0);
            char t[16];
            g_snprintf(t, sizeof t, "%d%%", pct);
            gtk_label_set_text(GTK_LABEL(A.percent), t);
        } else if (!strcmp(f[0], "STEP")) {
            step_text(f[1]);
        } else if (!strcmp(f[0], "ERROR")) {
            g_strlcpy(A.last_error, f[2] ? f[2] : f[1], sizeof A.last_error);
        }
    }
    g_strfreev(f);
    g_free(line);
    g_data_input_stream_read_line_async(A.lines, G_PRIORITY_DEFAULT, NULL, on_line, NULL);
}

static void start_install(void)
{
    su_progress_set(A.bar, 0.0);
    gtk_label_set_text(GTK_LABEL(A.percent), "0%");
    step_text("partition");
    A.last_error[0] = '\0';
    su_go(A.stack, "progress", TRUE);

    GError *err = NULL;
    /* Where: the whole disk, one partition of it, or its free space. */
    const char *flag = A.mode == MODE_PART ? "--partition"
                     : A.mode == MODE_FREE ? "--free-space" : "--disk";
    const char *where = A.mode == MODE_PART ? A.part : A.disk;
    /* The answers as options, the password on stdin: an argument is
     * readable by every process on the machine through /proc. */
    A.proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
                              G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                              G_SUBPROCESS_FLAGS_STDERR_MERGE, &err,
                              backend(), "install", flag, where, "--yes",
                              "--progress", "--lang",
                              su_korean ? "ko_KR.UTF-8" : "en_US.UTF-8",
                              "--user", su_account_login(&A.acct),
                              "--fullname", su_account_fullname(&A.acct),
                              "--password-stdin",
                              "--hostname", su_region_hostname(&A.region),
                              "--timezone", su_region_timezone(&A.region),
                              NULL);
    if (A.proc) {
        GOutputStream *in = g_subprocess_get_stdin_pipe(A.proc);
        const char *pw = su_account_password(&A.acct);
        g_output_stream_write_all(in, pw, strlen(pw), NULL, NULL, NULL);
        g_output_stream_write_all(in, "\n", 1, NULL, NULL, NULL);
        g_output_stream_close(in, NULL, NULL);
    }
    if (!A.proc) {
        g_strlcpy(A.last_error, err ? err->message : "could not start", sizeof A.last_error);
        g_clear_error(&err);
        finished(FALSE);
        return;
    }
    A.lines = g_data_input_stream_new(g_subprocess_get_stdout_pipe(A.proc));
    g_data_input_stream_read_line_async(A.lines, G_PRIORITY_DEFAULT, NULL, on_line, NULL);
}

static void page_progress(void)
{
    SuPage p;
    su_page(&p, "Installing LP", "LP 를 설치하는 중",
            "This takes a few minutes. Keep the computer plugged in.",
            "몇 분 걸립니다. 전원을 연결해 두세요.");
    A.step = su_label("", "", "su-choice-title");
    gtk_box_append(GTK_BOX(p.body), A.step);
    A.bar = su_progress_new();
    gtk_box_append(GTK_BOX(p.body), su_progress_widget(A.bar));
    A.percent = gtk_label_new("0%");
    gtk_widget_add_css_class(A.percent, "su-note");
    gtk_label_set_xalign(GTK_LABEL(A.percent), 1.0);
    gtk_box_append(GTK_BOX(p.body), A.percent);
    gtk_stack_add_named(GTK_STACK(A.stack), p.root, "progress");
}

/* ── done / failed ─────────────────────────────────────────────────── */

static void page_done(void)
{
    SuPage p;
    su_page(&p, "LP is installed", "LP 설치를 마쳤습니다",
            "Remove the USB stick, then restart.",
            "USB 를 뽑은 뒤 다시 시작하세요.");
    A.done_sub = p.subtitle;
    A.done_text = su_label("", "", "su-body");
    gtk_box_append(GTK_BOX(p.body), A.done_text);
    GtkWidget *off = su_button("Power off", "전원 끄기", "su-secondary");
    g_signal_connect(off, "clicked", G_CALLBACK(on_poweroff), NULL);
    gtk_box_append(GTK_BOX(p.left), off);
    GtkWidget *rs = su_button("Restart", "다시 시작", "su-primary");
    g_signal_connect(rs, "clicked", G_CALLBACK(on_restart), NULL);
    gtk_box_append(GTK_BOX(p.right), rs);
    gtk_stack_add_named(GTK_STACK(A.stack), p.root, "done");
}

static void on_again(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    show_disks();
    su_go(A.stack, "disks", FALSE);
}

static void page_failed(void)
{
    SuPage p;
    su_page(&p, "The installation did not finish", "설치를 마치지 못했습니다",
            "The disk may be partly written. Nothing on the USB stick was changed.",
            "디스크에 일부만 쓰였을 수 있습니다. USB 에 있는 것은 바뀌지 않았습니다.");
    A.fail_text = narrow(su_label("", "", "su-warn"));
    gtk_label_set_selectable(GTK_LABEL(A.fail_text), TRUE);
    gtk_box_append(GTK_BOX(p.body), A.fail_text);
    GtkWidget *off = su_button("Power off", "전원 끄기", "su-secondary");
    g_signal_connect(off, "clicked", G_CALLBACK(on_poweroff), NULL);
    gtk_box_append(GTK_BOX(p.left), off);
    GtkWidget *again = su_button("Try again", "다시 시도", "su-primary");
    g_signal_connect(again, "clicked", G_CALLBACK(on_again), NULL);
    gtk_box_append(GTK_BOX(p.right), again);
    gtk_stack_add_named(GTK_STACK(A.stack), p.root, "failed");
}

/* ── the window ────────────────────────────────────────────────────── */

/* LP_SETUP_NEXT=<page>@<ms>: slide to a page after a delay, so a test
 * can photograph the transition in flight (there is no input device in
 * the headless rig to press the button with). */
static gboolean auto_next(gpointer data)
{
    su_go(A.stack, data, TRUE);
    return G_SOURCE_REMOVE;
}

static gboolean card_start(GtkWidget *w, GdkFrameClock *fc, gpointer d)
{
    static int frames;
    (void)w; (void)fc; (void)d;
    if (++frames < 3)
        return G_SOURCE_CONTINUE;
    lp_spring_set_target(&A.card_in, 1.0);
    lp_motion_kick(A.card_m);
    return G_SOURCE_REMOVE;
}

static void card_frame(GtkWidget *w, gpointer d)
{
    (void)d;
    gtk_widget_set_opacity(w, CLAMP(A.card_in.x, 0.0, 1.0));
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)w; (void)d;
    /* Closing mid-copy would leave a half-written disk and a running
     * rsync with nobody reading its output. */
    return A.proc != NULL;
}

/* The confirmation's plan rows: a coloured tag per partition - red for
 * what is erased, the accent for what is made, a quiet one for what is
 * kept - so the one that goes is found at a glance. */
static const char PLAN_CSS[] =
    ".lp-plan-row { padding: 8px 12px; border-radius: 12px;\n"
    "  background-color: alpha(white, 0.04); }\n"
    ".lp-plan-tag { font-size: 14px; font-weight: 700; border-radius: 8px;\n"
    "  padding: 5px 10px; }\n"
    ".lp-tag-erase { background-color: alpha(#c01c28, 0.45); color: #ffe1dc; }\n"
    ".lp-tag-new { background-color: alpha(#f28c28, 0.32); color: #ffe6cc; }\n"
    ".lp-tag-add { background-color: alpha(#3584e4, 0.32); color: #dcebff; }\n"
    ".lp-tag-keep { background-color: alpha(white, 0.08); color: #9fb3c4; }\n"
    ".lp-plan-title { font-size: 16px; font-weight: 600; color: #eaf2f8; }\n";

static void activate(GtkApplication *app, gpointer d)
{
    (void)d;
    su_load_css();
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, PLAN_CSS, -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    g_object_unref(css);
    A.win = gtk_application_window_new(app);
    gtk_widget_add_css_class(A.win, "su-window");
    gtk_window_set_title(GTK_WINDOW(A.win), "Install LP");
    gtk_window_set_default_size(GTK_WINDOW(A.win), 1100, 760);
    g_signal_connect(A.win, "close-request", G_CALLBACK(on_close), NULL);

    GtkWidget *card = A.card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "su-card");
    gtk_widget_set_size_request(card, 760, 560);
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    A.stack = su_stack();
    gtk_widget_set_vexpand(A.stack, TRUE);
    gtk_box_append(GTK_BOX(card), A.stack);
    gtk_window_set_child(GTK_WINDOW(A.win), su_card_holder(card));
    su_osk_card(card);

    page_welcome();
    page_problem();
    page_account();
    page_region();
    page_disks();
    page_parts();
    page_confirm();
    page_progress();
    page_done();
    page_failed();
    su_set_korean(su_korean);
    gtk_stack_set_visible_child_name(GTK_STACK(A.stack), "welcome");

    if (!A.windowed)
        gtk_window_fullscreen(GTK_WINDOW(A.win));
    gtk_window_present(GTK_WINDOW(A.win));

    /* The card fades in on the window spring (340 ms, no overshoot)
     * over the gate's plain background, which is the colour the boot
     * splash ends on: firmware logo, splash, then the card appearing on
     * the same dark ground, with no frame of anything else between. */
    lp_spring_init(&A.card_in, LP_SPRING_WINDOW, lp_motion_reduced() ? 1.0 : 0.0);
    gtk_widget_set_opacity(card, A.card_in.x);
    A.card_m = lp_motion_new(card, card_frame, NULL);
    lp_motion_add(A.card_m, &A.card_in);
    /* Started from the third frame, not the first: the first frames of a
     * new window at 3840x2160 are its layout and its fonts loading, and
     * an animation that begins then loses its first 300 ms to them. */
    gtk_widget_add_tick_callback(A.card, card_start, NULL, NULL);

    const char *next = g_getenv("LP_SETUP_NEXT");
    if (next && strchr(next, '@')) {
        char *to = g_strndup(next, strchr(next, '@') - next);
        g_timeout_add(atoi(strchr(next, '@') + 1), auto_next, to);
    }

    /* For screenshots and tests: start on a page. */
    const char *page = g_getenv("LP_SETUP_PAGE");
    if (page && *page) {
        if (!strcmp(page, "disks") || !strcmp(page, "confirm") ||
            !strcmp(page, "progress") || !strcmp(page, "parts")) {
            /* Filled in as a person would have, for the screenshots. */
            gtk_editable_set_text(GTK_EDITABLE(A.acct.fullname), "Alex Kim");
            gtk_editable_set_text(GTK_EDITABLE(A.acct.pw1), "example");
            gtk_editable_set_text(GTK_EDITABLE(A.acct.pw2), "example");
            su_region_suggest_host(&A.region, su_account_login(&A.acct));
            show_disks();
        }
        if (!strcmp(page, "region"))
            su_region_suggest_host(&A.region, "alex");
        if (!strcmp(page, "parts") && A.disk_j && lp_json_bool(A.disk_j, "part_ok", 0)) {
            A.mode = MODE_PART;
            show_parts();
        }
        if (!strcmp(page, "confirm") && A.disk[0] && A.mode != MODE_NONE)
            on_disk_next(NULL, NULL);
        else
            gtk_stack_set_visible_child_name(GTK_STACK(A.stack), page);
        if (!strcmp(page, "progress"))
            start_install();
        if (!strcmp(page, "problem"))
            on_continue(NULL, NULL);
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--korean"))
            su_korean = TRUE;
        else if (!strcmp(argv[i], "--windowed"))
            A.windowed = TRUE;
    }
    const char *lang = g_getenv("LANG");
    if (lang && g_str_has_prefix(lang, "ko"))
        su_korean = TRUE;
    A.app = gtk_application_new("org.lp.Installer", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(A.app, "activate", G_CALLBACK(activate), NULL);
    char *args[] = { argv[0], NULL };
    int st = g_application_run(G_APPLICATION(A.app), 1, args);
    g_object_unref(A.app);
    return st ? st : A.exit_code;
}
