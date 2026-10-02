/* software - the software centre: install and remove Debian packages and
 * LP's own packages, and keep them up to date.
 *
 *   lp-software                 open on Explore
 *   lp-software --page=updates  open on a page (explore, installed,
 *                               updates, sources) - the shell's "updates
 *                               available" notification uses this
 *
 * ── What it is a front for ──
 *
 * Reading needs no privilege and is done here directly, from the files
 * apt and dpkg keep: /var/lib/dpkg/status says what is installed and how
 * big it is, /var/lib/dpkg/info/<pkg>.list says which package put which
 * .desktop file down (so an installed application can be shown with its
 * own icon and name, and removed by its package), `apt-cache` searches
 * and describes what could be installed, and `apt list --upgradable`
 * says what has an update. LP's own packages (userland/pkg) are two
 * plain files: /data/pkg/index and /data/pkg/db/.
 *
 * Changing anything goes through lp-privd, the one root process the
 * desktop may ask, with the person's password (lp-kit.c does the asking).
 * This program never runs apt or dpkg with privilege itself and never
 * builds a command line for the daemon - it sends a verb and package
 * names, and the daemon checks each one against Debian's rules for a
 * package name before anything happens.
 *
 * ── Why the pages look the way they do ──
 *
 * The spec (apps-and-settings §2-3): search, recommendations, installed,
 * updates; a card shows its size AND the space left after installing;
 * installing goes on when the window is closed; repositories are managed
 * from a sub-page here, not in Settings. The recommendations are the
 * spec's §3-2 list - the applications deliberately left out of the image
 * because not everybody wants them - which is exactly the list somebody
 * opening a software centre on this machine is most likely looking for.
 *
 * A job has a bar at the bottom of the window: what is happening, a
 * progress bar that glides to apt's real percentage (and pulses only
 * while apt has not given one), and the log apt printed, one tap away.
 * Jobs queue; lp-privd runs one at a time anyway, and a queue here means
 * "Install" on three cards in a row does what it says instead of
 * answering "busy" twice.
 *
 * Closing the window while a job runs hides it and keeps the process
 * until the queue is empty, then says how it went in a notification:
 * the daemon would finish the job regardless, but nobody would learn
 * whether it worked.
 */
#define _GNU_SOURCE 1
#include "lp-kit.h"
#include "lp-fit.h"

#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#define APP_ID      "org.lpzero.Software"
#define STATE_NAME  "software"

#define WINDOW_W    1120
#define WINDOW_H    780
#define SIDEBAR_W   188

#define DPKG_STATUS "/var/lib/dpkg/status"
#define DPKG_INFO   "/var/lib/dpkg/info"
#define LP_INDEX    "/data/pkg/index"
#define LP_DB       "/data/pkg/db"
#define LP_REPO     "/data/pkg/repo"
/* Flathub's catalogue, as lp-privd's flatpak-refresh downloads it: the
 * appstream data `flatpak search` reads, and an icon per application. */
#define FLATHUB_AS    "/var/lib/flatpak/appstream/flathub/x86_64/active"
#define FLATHUB_ICONS FLATHUB_AS "/icons/128x128"

#define SEARCH_MAX  150      /* apt-cache search answers are cut here */
#define LOG_MAX     4000     /* lines kept in the job log */

/* ═══════════════════════════════════════════════════════════════════
 * A package, as every page shows it
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum { SRC_DEBIAN, SRC_LP, SRC_FLATPAK } PkgSource;
typedef enum { ST_IDLE, ST_QUEUED, ST_WORKING } PkgState;

#define LPS_TYPE_PKG (lps_pkg_get_type())
G_DECLARE_FINAL_TYPE(LpsPkg, lps_pkg, LPS, PKG, GObject)

struct _LpsPkg {
    GObject    parent_instance;
    PkgSource  src;
    char      *name;         /* the package name: what the daemon gets */
    char      *title;        /* what a person reads: app name or package */
    char      *summary;
    char      *icon;         /* icon name, or NULL for the generic one */
    char      *tile;         /* colour behind a symbolic icon, or NULL */
    char      *version;      /* installed version (or available) */
    char      *new_version;  /* updates page: what it becomes */
    char      *desktop_id;   /* installed application: its .desktop id */
    gint64     size_kib;     /* installed size, -1 unknown */
    gboolean   installed;
    gboolean   system;       /* part of LP itself: cannot be removed here */
    PkgState   state;
    gboolean   fresh;        /* just added: slide in when first shown */
    gboolean   leaving;      /* on its way out of the list */
};

G_DEFINE_TYPE(LpsPkg, lps_pkg, G_TYPE_OBJECT)

static guint pkg_changed_signal;

static void lps_pkg_finalize(GObject *o)
{
    LpsPkg *p = LPS_PKG(o);
    g_free(p->name);
    g_free(p->title);
    g_free(p->summary);
    g_free(p->icon);
    g_free(p->tile);
    g_free(p->version);
    g_free(p->new_version);
    g_free(p->desktop_id);
    G_OBJECT_CLASS(lps_pkg_parent_class)->finalize(o);
}

static void lps_pkg_class_init(LpsPkgClass *k)
{
    G_OBJECT_CLASS(k)->finalize = lps_pkg_finalize;
    /* "changed": the row showing this package redraws itself from it.
     * Cheaper and calmer than items-changed, which rebuilds the row. */
    pkg_changed_signal = g_signal_new("changed", LPS_TYPE_PKG,
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void lps_pkg_init(LpsPkg *p)
{
    p->size_kib = -1;
}

static LpsPkg *pkg_new(PkgSource src, const char *name)
{
    LpsPkg *p = g_object_new(LPS_TYPE_PKG, NULL);
    p->src = src;
    p->name = g_strdup(name);
    p->title = g_strdup(name);
    return p;
}

static void pkg_changed(LpsPkg *p)
{
    g_signal_emit(p, pkg_changed_signal, 0);
}

/* ═══════════════════════════════════════════════════════════════════
 * What the system knows (read in a thread, swapped in at once)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    char   *version;
    char   *summary;
    gint64  size_kib;
    gboolean installed;
} DpkgInfo;

static void dpkg_info_free(gpointer d)
{
    DpkgInfo *i = d;
    g_free(i->version);
    g_free(i->summary);
    g_free(i);
}

typedef struct {
    char   *name;
    char   *version;
    gint64  size_kib;
} FlatpakInfo;

static void flatpak_info_free(gpointer d)
{
    FlatpakInfo *i = d;
    g_free(i->name);
    g_free(i->version);
    g_free(i);
}

typedef struct {
    GHashTable *dpkg;          /* package -> DpkgInfo (installed or half) */
    GHashTable *desktop_pkg;   /* desktop id -> package */
    GHashTable *lp_installed;  /* LP package -> version text */
    GHashTable *flatpak;       /* Flathub application ID -> FlatpakInfo */
} Facts;

static void facts_free(Facts *f)
{
    if (!f)
        return;
    g_clear_pointer(&f->dpkg, g_hash_table_unref);
    g_clear_pointer(&f->desktop_pkg, g_hash_table_unref);
    g_clear_pointer(&f->lp_installed, g_hash_table_unref);
    g_clear_pointer(&f->flatpak, g_hash_table_unref);
    g_free(f);
}

/* /var/lib/dpkg/status: paragraphs of "Field: value", the first line of
 * Description being the summary. Only fully installed packages count as
 * installed - a package in "deinstall ok config-files" is gone as far as
 * a person is concerned. */
static GHashTable *read_dpkg_status(void)
{
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          dpkg_info_free);
    char *text = NULL;
    if (!g_file_get_contents(DPKG_STATUS, &text, NULL, NULL))
        return h;
    char *name = NULL;
    DpkgInfo *cur = g_new0(DpkgInfo, 1);
    cur->size_kib = -1;
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        if (*line == '\0') {
            if (name) {
                g_hash_table_replace(h, name, cur);
                cur = g_new0(DpkgInfo, 1);
                cur->size_kib = -1;
                name = NULL;
            }
            continue;
        }
        if (g_str_has_prefix(line, "Package: "))
            name = g_strdup(line + 9);
        else if (g_str_has_prefix(line, "Status: "))
            cur->installed = g_str_has_suffix(line, " installed") &&
                             !strstr(line, "not-installed") &&
                             !strstr(line, "config-files");
        else if (g_str_has_prefix(line, "Version: "))
            cur->version = g_strdup(line + 9);
        else if (g_str_has_prefix(line, "Installed-Size: "))
            cur->size_kib = g_ascii_strtoll(line + 16, NULL, 10);
        else if (g_str_has_prefix(line, "Description: "))
            cur->summary = g_strdup(line + 13);
    }
    if (name)
        g_hash_table_replace(h, name, cur);
    else
        dpkg_info_free(cur);
    g_free(text);
    return h;
}

/* Which package owns which .desktop file, from dpkg's own file lists. */
static GHashTable *read_desktop_owners(void)
{
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GDir *d = g_dir_open(DPKG_INFO, 0, NULL);
    if (!d)
        return h;
    const char *f;
    while ((f = g_dir_read_name(d))) {
        if (!g_str_has_suffix(f, ".list"))
            continue;
        char *path = g_build_filename(DPKG_INFO, f, NULL);
        char *text = NULL;
        if (g_file_get_contents(path, &text, NULL, NULL) &&
            strstr(text, "/usr/share/applications/")) {
            /* "libreoffice-writer:amd64.list" -> "libreoffice-writer" */
            char *pkg = g_strndup(f, strlen(f) - 5);
            char *colon = strchr(pkg, ':');
            if (colon)
                *colon = '\0';
            for (char *l = text, *n; l && *l; l = n) {
                n = strchr(l, '\n');
                if (n)
                    *n++ = '\0';
                if (g_str_has_prefix(l, "/usr/share/applications/") &&
                    g_str_has_suffix(l, ".desktop") && !strchr(l + 24, '/'))
                    g_hash_table_replace(h, g_strdup(l + 24), g_strdup(pkg));
            }
            g_free(pkg);
        }
        g_free(text);
        g_free(path);
    }
    g_dir_close(d);
    return h;
}

/* LP's packages: /data/pkg/db/<name>.list marks one installed, and the
 * first line of <name>.info describes it. */
static GHashTable *read_lp_installed(void)
{
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GDir *d = g_dir_open(LP_DB, 0, NULL);
    if (!d)
        return h;
    const char *f;
    while ((f = g_dir_read_name(d))) {
        if (!g_str_has_suffix(f, ".list"))
            continue;
        char *name = g_strndup(f, strlen(f) - 5);
        char *ip = g_strdup_printf("%s/%s.info", LP_DB, name);
        char *text = NULL;
        char *first = NULL;
        if (g_file_get_contents(ip, &text, NULL, NULL)) {
            char *nl = strchr(text, '\n');
            first = g_strndup(text, nl ? (gsize)(nl - text) : strlen(text));
        }
        g_hash_table_replace(h, name, first ? first : g_strdup(""));
        g_free(text);
        g_free(ip);
    }
    g_dir_close(d);
    return h;
}

/* "123.4 MB", "980 kB", "1.2 GB" (GLib's g_format_size, which puts a
 * no-break space between the number and the unit) -> KiB, or -1. */
static gint64 size_text_kib(const char *s)
{
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (!end || end == s)
        return -1;
    while (*end == ' ' || (guchar)*end == 0xc2 || (guchar)*end == 0xa0)
        end++;
    double mul = g_str_has_prefix(end, "kB") ? 1e3 : g_str_has_prefix(end, "MB") ? 1e6 :
                 g_str_has_prefix(end, "GB") ? 1e9 : g_str_has_prefix(end, "TB") ? 1e12 : 1;
    return (gint64)(v * mul / 1024);
}

/* The Flathub applications installed for everybody. `flatpak list`
 * separates the columns it was asked for with tabs when it is not
 * talking to a terminal. No flatpak, or nothing installed: an empty
 * table. */
static GHashTable *read_flatpak_installed(void)
{
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          flatpak_info_free);
    char *argv[] = { "flatpak", "list", "--system", "--app",
                     "--columns=application,name,version,size", NULL };
    char *out = NULL;
    if (!g_spawn_sync(NULL, argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, &out, NULL, NULL, NULL))
        return h;
    char **lines = g_strsplit(out ? out : "", "\n", 0);
    for (int i = 0; lines[i]; i++) {
        char **f = g_strsplit(lines[i], "\t", 0);
        if (f[0] && f[1] && strchr(f[0], '.') && !strchr(f[0], ' ')) {
            FlatpakInfo *fi = g_new0(FlatpakInfo, 1);
            fi->name = g_strdup(f[1]);
            fi->version = g_strdup(f[2] ? f[2] : "");
            fi->size_kib = f[2] && f[3] ? size_text_kib(f[3]) : -1;
            g_hash_table_replace(h, g_strdup(f[0]), fi);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(out);
    return h;
}

static void facts_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)data; (void)c;
    Facts *f = g_new0(Facts, 1);
    f->dpkg = read_dpkg_status();
    f->desktop_pkg = read_desktop_owners();
    f->lp_installed = read_lp_installed();
    f->flatpak = read_flatpak_installed();
    g_task_return_pointer(task, f, (GDestroyNotify)facts_free);
}

/* ═══════════════════════════════════════════════════════════════════
 * The window
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum { OP_INSTALL, OP_REMOVE, OP_UPGRADE, OP_UPGRADE_ALL,
               OP_REFRESH, OP_FLAT_REFRESH, OP_FLAT_UPGRADE_ALL } OpKind;

typedef struct {
    OpKind  kind;
    LpsPkg *pkg;      /* NULL for the whole-system operations */
} Op;

typedef struct {
    GtkApplication *gapp;
    GtkWidget  *win;
    GtkWidget  *sidebar;
    GtkWidget  *stack;
    GtkWidget  *search;
    GtkWidget  *updates_badge;
    char       *page;          /* the page to go back to after a search */
    guint       search_timer;
    GCancellable *search_cancel;

    Facts      *facts;
    gboolean    admin;         /* lp-privd says we may ask */
    GtkWidget  *banner;        /* revealer: "not an administrator" etc. */
    GtkWidget  *banner_label;
    GtkWidget  *banner_button;

    /* Explore */
    GPtrArray  *featured;      /* LpsPkg */
    GtkWidget  *featured_box;

    /* Lists */
    GListStore *search_store;
    GtkWidget  *search_status;
    GListStore *apps_store;    /* installed applications */
    GListStore *all_store;     /* every installed Debian package */
    GListStore *lp_store;      /* LP's own packages */
    GtkWidget  *installed_stack;
    GListStore *updates_store;
    guint       updates_gen;   /* which check the lists belong to */
    GtkWidget  *updates_title;
    GtkWidget  *update_all;
    GtkWidget  *sources_box;

    /* Jobs */
    GQueue      queue;         /* Op*, waiting */
    Op         *running;
    GtkWidget  *jobbar;        /* revealer */
    GtkWidget  *job_title;
    GtkWidget  *job_detail;
    GtkWidget  *job_progress;
    GtkWidget  *job_log_rev;
    GtkWidget  *job_log;
    GtkWidget  *job_log_btn;
    guint       job_hide;
    int         jobs_ok, jobs_failed;
    char       *last_fail;
    gboolean    held;          /* window closed with work left */

    GKeyFile   *state;
} App;

static App *A;

static void refresh_facts(void);
static void load_updates(void);
static void select_side(const char *id);
static void run_next(void);
static void queue_op(OpKind kind, LpsPkg *pkg);
static void ask_remove(LpsPkg *p);

static GtkWindow *win(void) { return GTK_WINDOW(A->win); }

static void save_state(void)
{
    lp_kit_state_save(A->state, STATE_NAME);
}

/* ── formatting ───────────────────────────────────────────────────── */

static char *fmt_kib(gint64 kib)
{
    if (kib < 0)
        return g_strdup("");
    return g_format_size_full((guint64)kib * 1024, G_FORMAT_SIZE_DEFAULT);
}

static guint64 free_bytes(void)
{
    struct statvfs s;
    if (statvfs("/", &s) != 0)
        return 0;
    return (guint64)s.f_bavail * s.f_frsize;
}

/* "85 MB · 9.1 GB left after installing" - the spec's pairing: the size
 * alone does not say whether it fits. */
static char *size_line(LpsPkg *p)
{
    if (p->size_kib < 0)
        return g_strdup("");
    char *sz = fmt_kib(p->size_kib);
    if (p->installed) {
        char *s = g_strdup_printf(T("%s on disk", "디스크 %s"), sz);
        g_free(sz);
        return s;
    }
    guint64 fb = free_bytes();
    guint64 need = (guint64)p->size_kib * 1024;
    char *s;
    if (fb && need < fb) {
        char *left = g_format_size(fb - need);
        s = g_strdup_printf(T("%s · %s free after", "%s · 설치 후 %s 남음"), sz, left);
        g_free(left);
    } else if (fb) {
        s = g_strdup_printf(T("%s · not enough space", "%s · 공간 부족"), sz);
    } else {
        s = g_strdup(sz);
    }
    g_free(sz);
    return s;
}

/* ═══════════════════════════════════════════════════════════════════
 * The recommendations (spec §3-2: left out of the image on purpose)
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    const char *pkg, *icon;
    const char *en, *ko, *sum_en, *sum_ko;
} Featured;

static const Featured FEATURED[] = {
    { "libreoffice", "libreoffice-startcenter", "LibreOffice", "리브레오피스",
      "Documents, spreadsheets and slides", "문서, 스프레드시트, 프레젠테이션" },
    { "thunderbird", "thunderbird", "Thunderbird", "썬더버드",
      "Mail, calendar and contacts", "메일, 달력, 연락처" },
    { "gimp", "gimp", "GIMP", "김프",
      "Edit photos and pictures", "사진과 그림 편집" },
    { "inkscape", "inkscape", "Inkscape", "잉크스케이프",
      "Vector drawing", "벡터 그림" },
    { "vlc", "vlc", "VLC", "VLC",
      "Plays almost any video", "거의 모든 영상을 재생" },
    { "rhythmbox", "rhythmbox", "Rhythmbox", "리듬박스",
      "Music library and radio", "음악 모음과 라디오" },
    { "audacity", "audacity", "Audacity", "오대서티",
      "Record and edit sound", "소리 녹음과 편집" },
    { "shotwell", "shotwell", "Shotwell", "샷웰",
      "Organise your photos", "사진 정리" },
    { "cheese", "cheese", "Cheese", "치즈",
      "Webcam photos and video", "웹캠 사진과 영상" },
    { "transmission-gtk", "transmission", "Transmission", "트랜스미션",
      "BitTorrent downloads", "비트토렌트 다운로드" },
    { "remmina", "remmina", "Remmina", "레미나",
      "Remote desktop", "원격 데스크톱" },
    { "gnome-calendar", "org.gnome.Calendar", "Calendar", "달력",
      "Your schedule", "일정 관리" },
    { "gnome-contacts", "org.gnome.Contacts", "Contacts", "연락처",
      "People and addresses", "사람과 주소" },
    { "gnome-maps", "org.gnome.Maps", "Maps", "지도",
      "Find places and routes", "장소와 길 찾기" },
    { "gnome-weather", "org.gnome.Weather", "Weather", "날씨",
      "Forecasts where you are", "지금 있는 곳의 날씨" },
    { "krita", "krita", "Krita", "크리타",
      "Digital painting", "디지털 페인팅" },
};

/* What people ask for by name and Debian does not carry, or carries old:
 * Flathub's. Installed for everybody through lp-privd, as the Debian ones
 * are; `pkg` is the Flathub application ID, and the icon comes from
 * Flathub's catalogue once it has been downloaded (flatpak-refresh). */
static const Featured POPULAR[] = {
    { "com.discordapp.Discord", NULL, "Discord", "디스코드",
      "Voice, video and text chat", "음성, 영상, 텍스트 채팅" },
    { "com.spotify.Client", NULL, "Spotify", "스포티파이",
      "Music and podcasts", "음악과 팟캐스트" },
    { "us.zoom.Zoom", NULL, "Zoom", "줌",
      "Video meetings", "화상 회의" },
    { "com.google.Chrome", NULL, "Google Chrome", "구글 크롬",
      "Google's web browser", "구글 웹 브라우저" },
    { "com.visualstudio.code", NULL, "Visual Studio Code", "비주얼 스튜디오 코드",
      "Code editor", "코드 편집기" },
    { "com.valvesoftware.Steam", NULL, "Steam", "스팀",
      "Games and the store", "게임과 게임 상점" },
    { "com.obsproject.Studio", NULL, "OBS Studio", "OBS 스튜디오",
      "Record the screen and stream", "화면 녹화와 방송" },
    { "org.telegram.desktop", NULL, "Telegram", "텔레그램",
      "Messaging", "메신저" },
    { "com.slack.Slack", NULL, "Slack", "슬랙",
      "Team chat", "팀 채팅" },
    { "md.obsidian.Obsidian", NULL, "Obsidian", "옵시디언",
      "Notes and a knowledge base", "메모와 지식 정리" },
    { "com.brave.Browser", NULL, "Brave", "브레이브",
      "Private web browser", "개인정보를 지키는 브라우저" },
    { "org.blender.Blender", NULL, "Blender", "블렌더",
      "3D modelling and animation", "3D 모델링과 애니메이션" },
};

/* ═══════════════════════════════════════════════════════════════════
 * One row, used by every list
 * ═══════════════════════════════════════════════════════════════════ */

typedef struct {
    GtkWidget *rev;
    GtkWidget *icon;
    GtkWidget *title;
    GtkWidget *summary;
    GtkWidget *meta;
    GtkWidget *action;
    GtkWidget *open;
    LpsPkg    *pkg;
    gulong     handler;
} Row;

/* An icon is a theme name, or - for a Flathub application not installed
 * yet - the path of the PNG in Flathub's catalogue. */
static void set_icon(GtkWidget *img, const char *icon)
{
    GtkIconTheme *th = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (icon && icon[0] == '/')
        gtk_image_set_from_file(GTK_IMAGE(img), icon);
    else
        gtk_image_set_from_icon_name(GTK_IMAGE(img),
            (icon && gtk_icon_theme_has_icon(th, icon)) ? icon : "package-x-generic");
}

static GtkWidget *icon_tile(const char *icon, int px)
{
    GtkWidget *img = gtk_image_new();
    set_icon(img, icon);
    gtk_image_set_pixel_size(GTK_IMAGE(img), px);
    return img;
}

/* A Flathub application's icon: its own, exported into the icon theme,
 * once it is installed; the catalogue's before that; none without either. */
static char *flatpak_icon(const char *id)
{
    GtkIconTheme *th = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (gtk_icon_theme_has_icon(th, id))
        return g_strdup(id);
    char *png = g_strdup_printf("%s/%s.png", FLATHUB_ICONS, id);
    if (g_file_test(png, G_FILE_TEST_EXISTS))
        return png;
    g_free(png);
    return NULL;
}

/* The button says what a tap will do, and nothing else - "Install",
 * "Remove", "Update". While queued or running it says so and cannot be
 * tapped twice. */
static void action_label(LpsPkg *p, GtkWidget *b, gboolean updates_page)
{
    gtk_widget_remove_css_class(b, "suggested-action");
    gtk_widget_remove_css_class(b, "destructive-action");
    gtk_widget_set_sensitive(b, TRUE);
    gtk_widget_set_visible(b, TRUE);
    if (p->state == ST_QUEUED) {
        gtk_button_set_label(GTK_BUTTON(b), T("Waiting", "대기 중"));
        gtk_widget_set_sensitive(b, FALSE);
    } else if (p->state == ST_WORKING) {
        gtk_button_set_label(GTK_BUTTON(b), T("Working…", "진행 중…"));
        gtk_widget_set_sensitive(b, FALSE);
    } else if (updates_page) {
        gtk_button_set_label(GTK_BUTTON(b), T("Update", "업데이트"));
        gtk_widget_add_css_class(b, "suggested-action");
    } else if (p->system) {
        gtk_button_set_label(GTK_BUTTON(b), T("Part of LP", "LP 기본 구성"));
        gtk_widget_set_sensitive(b, FALSE);
    } else if (p->installed) {
        gtk_button_set_label(GTK_BUTTON(b), T("Remove", "제거"));
    } else {
        gtk_button_set_label(GTK_BUTTON(b), T("Install", "설치"));
        gtk_widget_add_css_class(b, "suggested-action");
    }
    if (!A->admin && !p->system && p->state == ST_IDLE)
        gtk_widget_set_sensitive(b, FALSE);
}

static void row_fill(Row *r, gboolean updates_page)
{
    LpsPkg *p = r->pkg;
    set_icon(r->icon, p->icon);
    gtk_label_set_text(GTK_LABEL(r->title), p->title);
    gtk_label_set_text(GTK_LABEL(r->summary), p->summary ? p->summary : "");
    char *meta;
    if (updates_page && p->new_version)
        meta = g_strdup_printf("%s  →  %s", p->version ? p->version : "?",
                               p->new_version);
    else {
        char *sz = size_line(p);
        const char *src = p->src == SRC_LP ? T("LP package", "LP 패키지") :
                          p->src == SRC_FLATPAK ? "Flathub" :
                          strcmp(p->title, p->name) == 0 ? T("Debian package", "데비안 패키지") :
                          p->name;
        meta = *sz ? g_strdup_printf("%s · %s", src, sz) : g_strdup(src);
        g_free(sz);
    }
    gtk_label_set_text(GTK_LABEL(r->meta), meta);
    g_free(meta);
    action_label(p, r->action, updates_page);
    gtk_widget_set_visible(r->open, p->installed && p->desktop_id && !updates_page);
    if (p->leaving)
        lp_kit_reveal_out(r->rev, NULL, NULL);
}

static void on_row_pkg_changed(LpsPkg *p, gpointer data)
{
    (void)p;
    Row *r = data;
    row_fill(r, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r->rev), "updates")));
}

static void on_action(GtkButton *b, gpointer data)
{
    (void)b;
    Row *r = data;
    LpsPkg *p = r->pkg;
    if (!p)
        return;
    gboolean upd = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r->rev), "updates"));
    if (upd)
        queue_op(OP_UPGRADE, p);
    else if (p->installed)
        ask_remove(p);
    else
        queue_op(OP_INSTALL, p);
}

static void launch_pkg(LpsPkg *p)
{
    if (!p->desktop_id)
        return;
    GDesktopAppInfo *ai = g_desktop_app_info_new(p->desktop_id);
    if (!ai)
        return;
    GdkAppLaunchContext *ctx =
        gdk_display_get_app_launch_context(gdk_display_get_default());
    g_app_info_launch(G_APP_INFO(ai), NULL, G_APP_LAUNCH_CONTEXT(ctx), NULL);
    g_object_unref(ctx);
    g_object_unref(ai);
}

static void on_open(GtkButton *b, gpointer data)
{
    (void)b;
    Row *r = data;
    if (r->pkg)
        launch_pkg(r->pkg);
}

static void row_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer upd)
{
    (void)f;
    Row *r = g_new0(Row, 1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(box, "lps-row");

    r->icon = icon_tile(NULL, 40);
    gtk_widget_set_valign(r->icon, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), r->icon);

    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(text, TRUE);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    r->title = gtk_label_new("");
    gtk_widget_add_css_class(r->title, "lps-row-title");
    gtk_label_set_xalign(GTK_LABEL(r->title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(r->title), PANGO_ELLIPSIZE_END);
    r->summary = gtk_label_new("");
    gtk_widget_add_css_class(r->summary, "lps-row-summary");
    gtk_label_set_xalign(GTK_LABEL(r->summary), 0);
    gtk_label_set_ellipsize(GTK_LABEL(r->summary), PANGO_ELLIPSIZE_END);
    r->meta = gtk_label_new("");
    gtk_widget_add_css_class(r->meta, "lps-row-meta");
    gtk_label_set_xalign(GTK_LABEL(r->meta), 0);
    gtk_label_set_ellipsize(GTK_LABEL(r->meta), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(text), r->title);
    gtk_box_append(GTK_BOX(text), r->summary);
    gtk_box_append(GTK_BOX(text), r->meta);
    gtk_box_append(GTK_BOX(box), text);

    r->open = gtk_button_new_with_label(T("Open", "열기"));
    gtk_widget_add_css_class(r->open, "lps-action");
    gtk_widget_set_valign(r->open, GTK_ALIGN_CENTER);
    g_signal_connect(r->open, "clicked", G_CALLBACK(on_open), r);
    gtk_box_append(GTK_BOX(box), r->open);

    r->action = gtk_button_new_with_label("");
    gtk_widget_add_css_class(r->action, "lps-action");
    gtk_widget_set_valign(r->action, GTK_ALIGN_CENTER);
    g_signal_connect(r->action, "clicked", G_CALLBACK(on_action), r);
    gtk_box_append(GTK_BOX(box), r->action);

    r->rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(r->rev),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_child(GTK_REVEALER(r->rev), box);
    g_object_set_data(G_OBJECT(r->rev), "updates", upd);
    g_object_set_data_full(G_OBJECT(r->rev), "lps-row", r, g_free);
    gtk_list_item_set_child(li, r->rev);
}

static gboolean open_row_next_frame(GtkWidget *w, GdkFrameClock *c, gpointer d)
{
    (void)c; (void)d;
    gtk_revealer_set_reveal_child(GTK_REVEALER(w), TRUE);
    return G_SOURCE_REMOVE;
}

static void row_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer upd)
{
    (void)f;
    GtkWidget *rev = gtk_list_item_get_child(li);
    Row *r = g_object_get_data(G_OBJECT(rev), "lps-row");
    r->pkg = gtk_list_item_get_item(li);
    char *nm = g_strdup_printf("row-%s", r->pkg->name);   /* lp_kit_drive */
    gtk_widget_set_name(r->action, nm);
    g_free(nm);
    r->handler = g_signal_connect(r->pkg, "changed",
                                  G_CALLBACK(on_row_pkg_changed), r);
    /* A package that has just arrived in this list slides in; one that
     * was there all along is simply there - recycled rows must not
     * ripple while scrolling. */
    if (r->pkg->fresh) {
        r->pkg->fresh = FALSE;
        gtk_revealer_set_transition_duration(GTK_REVEALER(rev), 0);
        gtk_revealer_set_reveal_child(GTK_REVEALER(rev), FALSE);
        gtk_revealer_set_transition_duration(GTK_REVEALER(rev),
            lp_spring_ms(LP_SPRING_INSERT, FALSE));
        gtk_widget_add_tick_callback(rev, open_row_next_frame, NULL, NULL);
    } else {
        gtk_revealer_set_transition_duration(GTK_REVEALER(rev), 0);
        gtk_revealer_set_reveal_child(GTK_REVEALER(rev), !r->pkg->leaving);
        gtk_revealer_set_transition_duration(GTK_REVEALER(rev),
            lp_spring_ms(LP_SPRING_INSERT, FALSE));
    }
    row_fill(r, GPOINTER_TO_INT(upd));
}

static void row_unbind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    GtkWidget *rev = gtk_list_item_get_child(li);
    Row *r = g_object_get_data(G_OBJECT(rev), "lps-row");
    if (r->pkg && r->handler)
        g_signal_handler_disconnect(r->pkg, r->handler);
    r->handler = 0;
    r->pkg = NULL;
}

static GtkWidget *pkg_list(GListStore *store, gboolean updates_page)
{
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    gpointer u = GINT_TO_POINTER(updates_page);
    g_signal_connect(f, "setup", G_CALLBACK(row_setup), u);
    g_signal_connect(f, "bind", G_CALLBACK(row_bind), u);
    g_signal_connect(f, "unbind", G_CALLBACK(row_unbind), u);
    GtkNoSelection *sel = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(store)));
    GtkWidget *lv = gtk_list_view_new(GTK_SELECTION_MODEL(sel), f);
    gtk_widget_add_css_class(lv, "lps-list");
    return lp_kit_scroller(lv, FALSE);
}

/* Remove one package from a store with the row sliding out first. */
typedef struct { GListStore *store; LpsPkg *pkg; } Leave;

static gboolean leave_now(gpointer d)
{
    Leave *l = d;
    guint pos;
    if (g_list_store_find(l->store, l->pkg, &pos))
        g_list_store_remove(l->store, pos);
    g_object_unref(l->pkg);
    g_free(l);
    return G_SOURCE_REMOVE;
}

static void store_leave(GListStore *store, LpsPkg *p)
{
    guint pos;
    if (!g_list_store_find(store, p, &pos))
        return;
    p->leaving = TRUE;
    pkg_changed(p);
    Leave *l = g_new0(Leave, 1);
    l->store = store;
    l->pkg = g_object_ref(p);
    g_timeout_add(lp_spring_ms(LP_SPRING_INSERT, TRUE) + 30, leave_now, l);
}

/* ═══════════════════════════════════════════════════════════════════
 * Facts -> the stores
 * ═══════════════════════════════════════════════════════════════════ */

static int by_title(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    return g_utf8_collate(((LpsPkg *)a)->title, ((LpsPkg *)b)->title);
}

/* Everything apt put down that also has an application entry: the list
 * people mean by "installed". Our own applications (/usr/local, not a
 * package) are listed as part of LP and have no Remove. */
static void fill_apps(void)
{
    GListStore *st = A->apps_store;
    gboolean first = g_list_model_get_n_items(G_LIST_MODEL(st)) == 0;
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GList *all = g_app_info_get_all();
    GPtrArray *fresh = g_ptr_array_new_with_free_func(g_object_unref);
    for (GList *l = all; l; l = l->next) {
        GAppInfo *ai = l->data;
        if (!g_app_info_should_show(ai))
            continue;
        const char *id = g_app_info_get_id(ai);
        if (!id)
            continue;
        const char *pkg = g_hash_table_lookup(A->facts->desktop_pkg, id);
        /* An entry exported by Flatpak is a Flathub application, removed
         * by its application ID (the file name without .desktop). */
        const char *file = G_IS_DESKTOP_APP_INFO(ai) ?
            g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(ai)) : NULL;
        gboolean flat = file && strstr(file, "/flatpak/exports/share/applications/");
        LpsPkg *p;
        if (flat) {
            char *appid = g_str_has_suffix(id, ".desktop") ?
                g_strndup(id, strlen(id) - 8) : g_strdup(id);
            p = pkg_new(SRC_FLATPAK, appid);
            FlatpakInfo *fi = g_hash_table_lookup(A->facts->flatpak, appid);
            if (fi) {
                p->size_kib = fi->size_kib;
                p->version = g_strdup(fi->version);
            }
            g_free(appid);
        } else {
            p = pkg_new(SRC_DEBIAN, pkg ? pkg : id);
        }
        g_free(p->title);
        p->title = g_strdup(g_app_info_get_display_name(ai));
        const char *d = g_app_info_get_description(ai);
        p->summary = g_strdup(d ? d : "");
        GIcon *ic = g_app_info_get_icon(ai);
        if (ic && G_IS_THEMED_ICON(ic)) {
            const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(ic));
            if (names && names[0])
                p->icon = g_strdup(names[0]);
        }
        p->desktop_id = g_strdup(id);
        p->installed = TRUE;
        if (flat) {
            /* the size and version came from `flatpak list` above */
        } else if (pkg) {
            DpkgInfo *di = g_hash_table_lookup(A->facts->dpkg, pkg);
            if (di) {
                p->size_kib = di->size_kib;
                p->version = g_strdup(di->version);
            }
        } else {
            p->system = TRUE;    /* not from a package: LP's own */
        }
        g_ptr_array_add(fresh, p);
    }
    g_list_free_full(all, g_object_unref);

    /* Keep the rows already there (their objects are what the widgets
     * are bound to), drop the ones gone, add the new ones. */
    for (guint i = 0; i < fresh->len; i++)
        g_hash_table_add(seen, ((LpsPkg *)g_ptr_array_index(fresh, i))->desktop_id);
    for (guint i = g_list_model_get_n_items(G_LIST_MODEL(st)); i-- > 0;) {
        LpsPkg *p = g_list_model_get_item(G_LIST_MODEL(st), i);
        if (!g_hash_table_contains(seen, p->desktop_id) && !p->leaving)
            store_leave(st, p);
        g_object_unref(p);
    }
    GHashTable *have = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(st)); i++) {
        LpsPkg *p = g_list_model_get_item(G_LIST_MODEL(st), i);
        g_hash_table_add(have, p->desktop_id);
        g_object_unref(p);
    }
    for (guint i = 0; i < fresh->len; i++) {
        LpsPkg *p = g_ptr_array_index(fresh, i);
        if (g_hash_table_contains(have, p->desktop_id))
            continue;
        p->fresh = !first;
        g_list_store_insert_sorted(st, p, by_title, NULL);
    }
    g_hash_table_unref(have);
    g_hash_table_unref(seen);
    g_ptr_array_unref(fresh);
}

static int by_title_ptr(gconstpointer a, gconstpointer b, gpointer d)
{
    return by_title(*(LpsPkg **)a, *(LpsPkg **)b, d);
}

static void fill_all(void)
{
    g_list_store_remove_all(A->all_store);
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_object_unref);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, A->facts->dpkg);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        DpkgInfo *di = v;
        if (!di->installed)
            continue;
        LpsPkg *p = pkg_new(SRC_DEBIAN, k);
        p->summary = g_strdup(di->summary);
        p->version = g_strdup(di->version);
        p->size_kib = di->size_kib;
        p->installed = TRUE;
        p->icon = g_strdup(k);
        g_ptr_array_add(arr, p);
    }
    g_ptr_array_sort_with_data(arr, by_title_ptr, NULL);
    g_list_store_splice(A->all_store, 0, 0, arr->pdata, arr->len);
    g_ptr_array_unref(arr);
}

/* LP packages: what the index offers, and what is installed. */
static void fill_lp(void)
{
    g_list_store_remove_all(A->lp_store);
    GHashTable *listed = g_hash_table_new(g_str_hash, g_str_equal);
    char *text = NULL;
    if (g_file_get_contents(LP_INDEX, &text, NULL, NULL)) {
        for (char *l = text, *n; l && *l; l = n) {
            n = strchr(l, '\n');
            if (n)
                *n++ = '\0';
            if (*l == '#' || !*l)
                continue;
            char **f = g_strsplit_set(l, " \t", 0);
            int nf = 0;
            char *field[5] = { 0 };
            for (int i = 0; f[i] && nf < 5; i++)
                if (*f[i])
                    field[nf++] = f[i];
            if (nf >= 3) {
                LpsPkg *p = pkg_new(SRC_LP, field[0]);
                p->version = g_strdup(field[1]);
                p->size_kib = g_ascii_strtoll(field[2], NULL, 10) / 1024;
                p->installed = g_hash_table_contains(A->facts->lp_installed, field[0]);
                const char *info = g_hash_table_lookup(A->facts->lp_installed, field[0]);
                p->summary = g_strdup(info && *info ? info : "");
                p->icon = g_strdup("application-x-executable");
                g_list_store_append(A->lp_store, p);
                g_hash_table_add(listed, p->name);
                g_object_unref(p);
            }
            g_strfreev(f);
        }
    }
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, A->facts->lp_installed);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (g_hash_table_contains(listed, k))
            continue;
        LpsPkg *p = pkg_new(SRC_LP, k);
        p->summary = g_strdup(v);
        p->installed = TRUE;
        p->icon = g_strdup("application-x-executable");
        g_list_store_append(A->lp_store, p);
        g_object_unref(p);
    }
    g_hash_table_unref(listed);
    g_free(text);
}

/* The featured cards and every row on screen re-read "installed" from
 * the facts: after a job, a Remove button must become Install. */
static void refresh_pkg(LpsPkg *p)
{
    if (!A->facts) {
        pkg_changed(p);
        return;
    }
    if (p->src == SRC_LP) {
        p->installed = g_hash_table_contains(A->facts->lp_installed, p->name);
    } else if (p->src == SRC_FLATPAK) {
        FlatpakInfo *fi = g_hash_table_lookup(A->facts->flatpak, p->name);
        p->installed = fi != NULL;
        if (fi) {
            if (fi->size_kib >= 0)
                p->size_kib = fi->size_kib;
            g_free(p->version);
            p->version = g_strdup(fi->version);
        } else {
            /* Flathub's catalogue has no sizes, and what the application
             * takes is the least of it - the runtime under it can be a
             * gigabyte. No number is better than a wrong one. */
            p->size_kib = -1;
        }
        /* Its application entry, for "Open", once Flatpak has exported
         * it; its icon, from the catalogue or from the installed app. */
        g_clear_pointer(&p->desktop_id, g_free);
        if (p->installed) {
            char *did = g_strconcat(p->name, ".desktop", NULL);
            GDesktopAppInfo *ai = g_desktop_app_info_new(did);
            if (ai) {
                p->desktop_id = did;
                g_object_unref(ai);
            } else {
                g_free(did);
            }
        }
        char *ic = flatpak_icon(p->name);
        if (ic) {
            g_free(p->icon);
            p->icon = ic;
        }
    } else {
        DpkgInfo *di = g_hash_table_lookup(A->facts->dpkg, p->name);
        gboolean was = p->installed;
        p->installed = di && di->installed;
        if (di && di->installed && di->size_kib >= 0)
            p->size_kib = di->size_kib;
        if (!was && p->installed && !p->desktop_id) {
            /* Newly installed: find its application entry, so "Open"
             * appears on the card that was just "Install". */
            GHashTableIter it;
            gpointer k, v;
            g_hash_table_iter_init(&it, A->facts->desktop_pkg);
            while (g_hash_table_iter_next(&it, &k, &v))
                if (strcmp(v, p->name) == 0) {
                    GDesktopAppInfo *ai = g_desktop_app_info_new(k);
                    if (ai) {
                        if (g_app_info_should_show(G_APP_INFO(ai)))
                            p->desktop_id = g_strdup(k);
                        g_object_unref(ai);
                    }
                    if (p->desktop_id)
                        break;
                }
        }
        if (!p->installed)
            g_clear_pointer(&p->desktop_id, g_free);
    }
    pkg_changed(p);
}

static void refresh_store(GListStore *st)
{
    for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(st)); i++) {
        LpsPkg *p = g_list_model_get_item(G_LIST_MODEL(st), i);
        refresh_pkg(p);
        g_object_unref(p);
    }
}

static void on_facts(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src; (void)data;
    Facts *f = g_task_propagate_pointer(G_TASK(res), NULL);
    if (!f)
        return;
    facts_free(A->facts);
    A->facts = f;
    for (guint i = 0; i < A->featured->len; i++)
        refresh_pkg(g_ptr_array_index(A->featured, i));
    fill_apps();
    fill_all();
    fill_lp();
    refresh_store(A->search_store);
}

static void refresh_facts(void)
{
    GTask *t = g_task_new(NULL, NULL, on_facts, NULL);
    g_task_run_in_thread(t, facts_thread);
    g_object_unref(t);
}

/* ═══════════════════════════════════════════════════════════════════
 * apt-cache, unprivileged
 * ═══════════════════════════════════════════════════════════════════ */

typedef void (*CmdDone)(const char *out, gboolean ok, gpointer data);
typedef struct { CmdDone cb; gpointer data; } Cmd;

static void on_cmd(GObject *src, GAsyncResult *res, gpointer data)
{
    Cmd *c = data;
    char *out = NULL;
    GError *err = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res,
                                                       &out, NULL, &err);
    if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        g_free(out);
        g_free(c);
        return;
    }
    g_clear_error(&err);
    ok = ok && g_subprocess_get_if_exited(G_SUBPROCESS(src)) &&
         g_subprocess_get_exit_status(G_SUBPROCESS(src)) == 0;
    c->cb(out ? out : "", ok, c->data);
    g_free(out);
    g_free(c);
}

/* argv, never a shell: a search term is one argument whatever is in it. */
static void run_cmd(const char *const *argv, GCancellable *cancel,
                    CmdDone cb, gpointer data)
{
    GError *err = NULL;
    GSubprocess *sp = g_subprocess_newv(argv,
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &err);
    if (!sp) {
        g_clear_error(&err);
        cb("", FALSE, data);
        return;
    }
    Cmd *c = g_new0(Cmd, 1);
    c->cb = cb;
    c->data = data;
    g_subprocess_communicate_utf8_async(sp, NULL, cancel, on_cmd, c);
    g_object_unref(sp);
}

/* "apt-cache show --no-all-versions a b c": sizes and summaries of
 * packages that are not installed, for the recommendation cards. */
static void on_featured_show(const char *out, gboolean ok, gpointer data)
{
    (void)data;
    if (!ok && !*out) {
        gtk_label_set_text(GTK_LABEL(A->banner_label),
            T("The package lists have not been downloaded yet. Check for "
              "updates once to see sizes and search everything.",
              "패키지 목록을 아직 받지 않았습니다. 크기를 보고 전체를 검색하려면 "
              "업데이트 확인을 한 번 하세요."));
        gtk_widget_set_visible(A->banner_button, A->admin);
        gtk_revealer_set_reveal_child(GTK_REVEALER(A->banner), TRUE);
    }
    char **paras = g_strsplit(out, "\n\n", 0);
    for (int i = 0; paras[i]; i++) {
        char *name = NULL;
        gint64 kib = -1;
        char **lines = g_strsplit(paras[i], "\n", 0);
        for (int j = 0; lines[j]; j++) {
            if (g_str_has_prefix(lines[j], "Package: "))
                name = lines[j] + 9;
            else if (g_str_has_prefix(lines[j], "Installed-Size: "))
                kib = g_ascii_strtoll(lines[j] + 16, NULL, 10);
        }
        for (guint k = 0; name && k < A->featured->len; k++) {
            LpsPkg *p = g_ptr_array_index(A->featured, k);
            if (strcmp(p->name, name) == 0 && !p->installed) {
                p->size_kib = kib;
                pkg_changed(p);
            }
        }
        g_strfreev(lines);
    }
    g_strfreev(paras);
}

static void load_featured_sizes(void)
{
    const char *argv[G_N_ELEMENTS(FEATURED) + 4];
    int n = 0;
    argv[n++] = "apt-cache";
    argv[n++] = "show";
    argv[n++] = "--no-all-versions";
    for (guint i = 0; i < G_N_ELEMENTS(FEATURED); i++)
        argv[n++] = FEATURED[i].pkg;
    argv[n] = NULL;
    run_cmd(argv, NULL, on_featured_show, NULL);
}

/* ── search ───────────────────────────────────────────────────────── */

static int search_rank(LpsPkg *p, const char *q)
{
    if (strcmp(p->name, q) == 0) return 0;
    if (g_str_has_prefix(p->name, q)) return 1;
    if (strstr(p->name, q)) return 2;
    return 3;
}

static const char *search_q;
static int by_rank(gconstpointer a, gconstpointer b)
{
    LpsPkg *x = *(LpsPkg **)a, *y = *(LpsPkg **)b;
    int d = search_rank(x, search_q) - search_rank(y, search_q);
    return d ? d : strcmp(x->name, y->name);
}

/* One search, two sources: Flathub's catalogue (`flatpak search`, no
 * privilege needed, nothing when it has not been downloaded) and then
 * apt-cache. The Flathub answers wait here for apt's. */
typedef struct {
    char      *q;
    GPtrArray *flat;          /* LpsPkg, from Flathub */
} Search;

/* Flathub matches by what a person reads - the application's name - not
 * by the ID: "discord" is Discord before it is anything that merely
 * mentions Discord in its description. */
static int flat_rank(LpsPkg *p, const char *q)
{
    char *t = g_utf8_strdown(p->title, -1);
    int r = strcmp(t, q) == 0 ? 0 : g_str_has_prefix(t, q) ? 1 : strstr(t, q) ? 2 :
            strstr(p->name, q) ? 3 : 4;
    g_free(t);
    return r;
}

static int by_flat_rank(gconstpointer a, gconstpointer b)
{
    LpsPkg *x = *(LpsPkg **)a, *y = *(LpsPkg **)b;
    int d = flat_rank(x, search_q) - flat_rank(y, search_q);
    return d ? d : g_utf8_collate(x->title, y->title);
}

/* Shared libraries (libuuid1, liblz4-1), headers and documentation:
 * what other packages pull in, never what anybody means by an app. A
 * search that asks for them by name ("libuuid", "-dev") still gets them. */
static gboolean plumbing(const char *name, const char *q)
{
    size_t n = strlen(name);
    if (g_str_has_prefix(q, "lib") || strchr(q, '-'))
        return FALSE;
    if (g_str_has_prefix(name, "lib") && !g_str_has_prefix(name, "libreoffice") &&
        n > 3 && g_ascii_isdigit(name[n - 1]))
        return TRUE;
    return g_str_has_suffix(name, "-dev") || g_str_has_suffix(name, "-doc") ||
           g_str_has_suffix(name, "-dbgsym") || strstr(name, "t64") != NULL;
}

static void on_search_out(const char *out, gboolean ok, gpointer data)
{
    Search *sr = data;
    char *q = sr->q;
    (void)ok;
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_object_unref);
    /* LP's own packages first: they are few and they are ours. */
    for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(A->lp_store)); i++) {
        LpsPkg *p = g_list_model_get_item(G_LIST_MODEL(A->lp_store), i);
        if (strstr(p->name, q))
            g_ptr_array_add(arr, p);
        else
            g_object_unref(p);
    }
    /* Then Flathub's, best match first and at most 40 of them: apt's
     * answers are what somebody scrolls through, these are what they
     * came for. */
    search_q = q;
    g_ptr_array_sort(sr->flat, by_flat_rank);
    for (guint i = 0; i < sr->flat->len && i < 40; i++)
        g_ptr_array_add(arr, g_object_ref(g_ptr_array_index(sr->flat, i)));
    g_ptr_array_unref(sr->flat);
    g_free(sr);
    guint lp_n = arr->len;
    char **lines = g_strsplit(out, "\n", 0);
    for (int i = 0; lines[i]; i++) {
        char *sep = strstr(lines[i], " - ");
        if (!sep)
            continue;
        *sep = '\0';
        if (plumbing(lines[i], q))
            continue;
        LpsPkg *p = pkg_new(SRC_DEBIAN, lines[i]);
        p->summary = g_strdup(sep + 3);
        p->icon = g_strdup(lines[i]);
        DpkgInfo *di = A->facts ? g_hash_table_lookup(A->facts->dpkg, lines[i]) : NULL;
        if (di && di->installed) {
            p->installed = TRUE;
            p->size_kib = di->size_kib;
            p->version = g_strdup(di->version);
        }
        for (guint k = 0; k < G_N_ELEMENTS(FEATURED); k++)
            if (strcmp(FEATURED[k].pkg, p->name) == 0) {
                g_free(p->title);
                p->title = g_strdup(T(FEATURED[k].en, FEATURED[k].ko));
                g_free(p->icon);
                p->icon = g_strdup(FEATURED[k].icon);
            }
        g_ptr_array_add(arr, p);
    }
    g_strfreev(lines);
    /* Ranked before it is cut: apt-cache answers alphabetically, and
     * "tree" itself must not fall off the end behind "altree". */
    search_q = q;
    if (arr->len > lp_n)
        qsort(arr->pdata + lp_n, arr->len - lp_n, sizeof(gpointer), by_rank);
    if (arr->len > lp_n + SEARCH_MAX)
        g_ptr_array_set_size(arr, lp_n + SEARCH_MAX);
    g_list_store_remove_all(A->search_store);
    g_list_store_splice(A->search_store, 0, 0, arr->pdata, arr->len);
    char *st;
    if (!arr->len)
        st = g_strdup_printf(T("Nothing found for “%s”", "“%s”에 해당하는 것이 없습니다"), q);
    else if (lp_korean())
        st = g_strdup_printf("“%s” 검색 결과 %u개", q, arr->len);
    else
        st = g_strdup_printf(arr->len == 1 ? "%u result for “%s”" : "%u results for “%s”",
                             arr->len, q);
    gtk_label_set_text(GTK_LABEL(A->search_status), st);
    g_free(st);
    g_ptr_array_unref(arr);
    g_free(q);
}

/* "com.discordapp.Discord<TAB>Discord<TAB>Messaging, Voice, and Video
 * Client", one application per line (tabs: not talking to a terminal). */
static void on_flat_search_out(const char *out, gboolean ok, gpointer data)
{
    Search *sr = data;
    (void)ok;
    sr->flat = g_ptr_array_new_with_free_func(g_object_unref);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    char **lines = g_strsplit(out, "\n", 0);
    for (int i = 0; lines[i]; i++) {
        char **f = g_strsplit(lines[i], "\t", 0);
        if (f[0] && f[1] && strchr(f[0], '.') && !strchr(f[0], ' ') &&
            !g_hash_table_contains(seen, f[0])) {
            LpsPkg *p = pkg_new(SRC_FLATPAK, f[0]);
            g_hash_table_add(seen, p->name);
            g_free(p->title);
            p->title = g_strdup(f[1]);
            p->summary = g_strdup(f[2] ? f[2] : "");
            p->icon = flatpak_icon(f[0]);
            FlatpakInfo *fi = A->facts ? g_hash_table_lookup(A->facts->flatpak, f[0]) : NULL;
            if (fi) {
                p->installed = TRUE;
                p->size_kib = fi->size_kib;
                p->version = g_strdup(fi->version);
                refresh_pkg(p);
            }
            g_ptr_array_add(sr->flat, p);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_hash_table_unref(seen);
    const char *argv[] = { "apt-cache", "search", "--", sr->q, NULL };
    run_cmd(argv, A->search_cancel, on_search_out, sr);
}

static gboolean do_search(gpointer d)
{
    (void)d;
    A->search_timer = 0;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(A->search));
    char *q = g_utf8_strdown(g_strstrip(g_strdup(text)), -1);
    if (A->search_cancel) {
        g_cancellable_cancel(A->search_cancel);
        g_clear_object(&A->search_cancel);
    }
    if (!*q) {
        g_free(q);
        lp_kit_stack_show(GTK_STACK(A->stack), A->page);
        select_side(A->page);
        return G_SOURCE_REMOVE;
    }
    gtk_label_set_text(GTK_LABEL(A->search_status), T("Searching…", "찾는 중…"));
    /* Search is not one of the sidebar's pages; nothing there is lit. */
    gtk_list_box_unselect_all(GTK_LIST_BOX(A->sidebar));
    lp_kit_stack_show(GTK_STACK(A->stack), "search");
    A->search_cancel = g_cancellable_new();
    Search *sr = g_new0(Search, 1);
    sr->q = q;
    const char *argv[] = { "flatpak", "search",
                           "--columns=application,name,description", "--", q, NULL };
    run_cmd(argv, A->search_cancel, on_flat_search_out, sr);
    return G_SOURCE_REMOVE;
}

static void on_search_changed(GtkEditable *e, gpointer d)
{
    (void)e; (void)d;
    if (A->search_timer)
        g_source_remove(A->search_timer);
    /* apt-cache search takes a moment; one search per pause in typing. */
    A->search_timer = g_timeout_add(280, do_search, NULL);
}

/* ── updates ──────────────────────────────────────────────────────── */

/* "name/bookworm-updates 1.2-3 amd64 [upgradable from: 1.2-2]" */
static void on_flat_updates(const char *out, gboolean ok, gpointer data);
static void updates_counted(void);

static void on_upgradable(const char *out, gboolean ok, gpointer data)
{
    (void)ok;
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_object_unref);
    char **lines = g_strsplit(out, "\n", 0);
    for (int i = 0; lines[i]; i++) {
        char *slash = strchr(lines[i], '/');
        if (!slash || !strstr(lines[i], "[upgradable from: "))
            continue;
        *slash = '\0';
        char **f = g_strsplit(slash + 1, " ", 0);
        LpsPkg *p = pkg_new(SRC_DEBIAN, lines[i]);
        if (f[0] && f[1])
            p->new_version = g_strdup(f[1]);
        char *from = strstr(slash + 1, "[upgradable from: ");
        if (from) {
            from += 18;
            char *end = strchr(from, ']');
            p->version = end ? g_strndup(from, (gsize)(end - from)) : g_strdup(from);
        }
        DpkgInfo *di = A->facts ? g_hash_table_lookup(A->facts->dpkg, p->name) : NULL;
        p->summary = g_strdup(di && di->summary ? di->summary : "");
        p->installed = TRUE;
        p->icon = g_strdup(p->name);
        g_ptr_array_add(arr, p);
        g_strfreev(f);
    }
    g_strfreev(lines);
    if (GPOINTER_TO_UINT(data) != A->updates_gen) {
        g_ptr_array_unref(arr);
        return;
    }
    g_list_store_remove_all(A->updates_store);
    g_list_store_splice(A->updates_store, 0, 0, arr->pdata, arr->len);
    g_ptr_array_unref(arr);
    updates_counted();
    /* Flathub's, added to the same list when they arrive: `remote-ls`
     * asks Flathub itself, and offline it just adds nothing. */
    if (A->facts && g_hash_table_size(A->facts->flatpak) > 0) {
        const char *argv[] = { "flatpak", "remote-ls", "--system", "--updates",
                               "--app", "--columns=application,name,version", NULL };
        run_cmd(argv, NULL, on_flat_updates, data);
    }
}

static void on_flat_updates(const char *out, gboolean ok, gpointer data)
{
    (void)ok;
    if (GPOINTER_TO_UINT(data) != A->updates_gen)
        return;
    char **lines = g_strsplit(out, "\n", 0);
    for (int i = 0; lines[i]; i++) {
        char **f = g_strsplit(lines[i], "\t", 0);
        FlatpakInfo *fi = f[0] && A->facts ? g_hash_table_lookup(A->facts->flatpak, f[0]) : NULL;
        if (fi) {
            LpsPkg *p = pkg_new(SRC_FLATPAK, f[0]);
            g_free(p->title);
            p->title = g_strdup(fi->name);
            p->version = g_strdup(fi->version);
            p->new_version = g_strdup(f[1] && f[2] && *f[2] ? f[2] : "");
            p->summary = g_strdup("");
            p->installed = TRUE;
            p->icon = flatpak_icon(f[0]);
            g_list_store_append(A->updates_store, p);
            g_object_unref(p);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    updates_counted();
}

/* The title, the "Update all" button and the sidebar badge, from what
 * the list holds now. */
static void updates_counted(void)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(A->updates_store));
    char *t = n ? g_strdup_printf(lp_korean() ? "업데이트 %u개" :
                                  (n == 1 ? "%u update available" : "%u updates available"), n)
                : g_strdup(T("Everything is up to date", "모두 최신입니다"));
    gtk_label_set_text(GTK_LABEL(A->updates_title), t);
    g_free(t);
    gtk_widget_set_sensitive(A->update_all, n > 0 && A->admin);
    char *b = g_strdup_printf("%u", n);
    gtk_label_set_text(GTK_LABEL(A->updates_badge), b);
    gtk_widget_set_visible(A->updates_badge, n > 0);
    g_free(b);
}

static void load_updates(void)
{
    /* A newer check makes the answers to an older one stale. */
    A->updates_gen++;
    const char *argv[] = { "apt", "list", "--upgradable", NULL };
    run_cmd(argv, NULL, on_upgradable, GUINT_TO_POINTER(A->updates_gen));
}

/* ═══════════════════════════════════════════════════════════════════
 * Jobs
 * ═══════════════════════════════════════════════════════════════════ */

static const char *op_verb(Op *op)
{
    switch (op->kind) {
    case OP_INSTALL:     return op->pkg->src == SRC_LP ? "pkg-install" :
                                op->pkg->src == SRC_FLATPAK ? "flatpak-install" : "apt-install";
    case OP_REMOVE:      return op->pkg->src == SRC_LP ? "pkg-remove" :
                                op->pkg->src == SRC_FLATPAK ? "flatpak-remove" : "apt-remove";
    case OP_UPGRADE:     return op->pkg->src == SRC_FLATPAK ? "flatpak-update" : "apt-upgrade";
    case OP_UPGRADE_ALL: return "apt-upgrade";
    case OP_REFRESH:     return "apt-update";
    case OP_FLAT_REFRESH:     return "flatpak-refresh";
    case OP_FLAT_UPGRADE_ALL: return "flatpak-update";
    }
    return "ping";
}

static char *op_title(Op *op, gboolean done, gboolean ok)
{
    const char *n = op->pkg ? op->pkg->title : "";
    if (!done) {
        switch (op->kind) {
        case OP_INSTALL: return g_strdup_printf(T("Installing %s", "%s 설치 중"), n);
        case OP_REMOVE:  return g_strdup_printf(T("Removing %s", "%s 제거 중"), n);
        case OP_UPGRADE: return g_strdup_printf(T("Updating %s", "%s 업데이트 중"), n);
        case OP_UPGRADE_ALL: return g_strdup(T("Updating everything", "모두 업데이트 중"));
        case OP_REFRESH: return g_strdup(T("Checking for updates", "업데이트 확인 중"));
        case OP_FLAT_REFRESH: return g_strdup(T("Getting Flathub's catalogue", "Flathub 목록 받는 중"));
        case OP_FLAT_UPGRADE_ALL: return g_strdup(T("Updating Flathub apps", "Flathub 앱 업데이트 중"));
        }
    }
    if (!ok) {
        switch (op->kind) {
        case OP_INSTALL: return g_strdup_printf(T("%s was not installed", "%s 을(를) 설치하지 못했습니다"), n);
        case OP_REMOVE:  return g_strdup_printf(T("%s was not removed", "%s 을(를) 제거하지 못했습니다"), n);
        case OP_UPGRADE: return g_strdup_printf(T("%s was not updated", "%s 을(를) 업데이트하지 못했습니다"), n);
        case OP_UPGRADE_ALL: return g_strdup(T("The update did not finish", "업데이트를 마치지 못했습니다"));
        case OP_REFRESH: return g_strdup(T("Could not check for updates", "업데이트를 확인하지 못했습니다"));
        case OP_FLAT_REFRESH: return g_strdup(T("Could not reach Flathub", "Flathub 에 연결하지 못했습니다"));
        case OP_FLAT_UPGRADE_ALL: return g_strdup(T("The Flathub update did not finish", "Flathub 업데이트를 마치지 못했습니다"));
        }
    }
    switch (op->kind) {
    case OP_INSTALL: return g_strdup_printf(T("%s is installed", "%s 설치됨"), n);
    case OP_REMOVE:  return g_strdup_printf(T("%s is removed", "%s 제거됨"), n);
    case OP_UPGRADE: return g_strdup_printf(T("%s is up to date", "%s 최신"), n);
    case OP_UPGRADE_ALL: return g_strdup(T("Everything is up to date", "모두 최신입니다"));
    case OP_REFRESH: return g_strdup(T("Package lists are up to date", "패키지 목록을 새로 받았습니다"));
    case OP_FLAT_REFRESH: return g_strdup(T("Flathub's catalogue is up to date", "Flathub 목록을 새로 받았습니다"));
    case OP_FLAT_UPGRADE_ALL: return g_strdup(T("Flathub apps are up to date", "Flathub 앱이 모두 최신입니다"));
    }
    return g_strdup("");
}

static const char *op_why(Op *op)
{
    switch (op->kind) {
    case OP_INSTALL: return T("Installing software changes the system.",
                              "소프트웨어 설치는 시스템을 바꿉니다.");
    case OP_REMOVE:  return T("Removing software changes the system.",
                              "소프트웨어 제거는 시스템을 바꿉니다.");
    default:         return T("Updating software changes the system.",
                              "소프트웨어 업데이트는 시스템을 바꿉니다.");
    }
}

static void log_append(const char *text)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(A->job_log));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, text, -1);
    gtk_text_buffer_insert(b, &end, "\n", 1);
    if (gtk_text_buffer_get_line_count(b) > LOG_MAX) {
        GtkTextIter s, e;
        gtk_text_buffer_get_start_iter(b, &s);
        gtk_text_buffer_get_iter_at_line(b, &e, 200);
        gtk_text_buffer_delete(b, &s, &e);
    }
    gtk_text_buffer_get_end_iter(b, &end);
    GtkTextMark *m = gtk_text_buffer_get_mark(b, "end");
    if (!m)
        m = gtk_text_buffer_create_mark(b, "end", &end, FALSE);
    else
        gtk_text_buffer_move_mark(b, m, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(A->job_log), m);
}

static void on_job_line(const char *kind, int pct, const char *text, gpointer d)
{
    (void)d;
    if (strcmp(kind, "progress") == 0) {
        if (pct >= 0)
            lp_kit_progress_set(A->job_progress, pct / 100.0);
        if (text && *text)
            gtk_label_set_text(GTK_LABEL(A->job_detail), text);
    } else if (strcmp(kind, "log") == 0) {
        log_append(text);
    }
}

static gboolean hide_jobbar(gpointer d)
{
    (void)d;
    A->job_hide = 0;
    if (!A->running && g_queue_is_empty(&A->queue))
        gtk_revealer_set_reveal_child(GTK_REVEALER(A->jobbar), FALSE);
    return G_SOURCE_REMOVE;
}

static void notify_done(void)
{
    GNotification *n = g_notification_new(T("Software", "소프트웨어"));
    char *body;
    if (A->jobs_failed)
        body = g_strdup(A->last_fail ? A->last_fail : T("Something did not finish.", "끝나지 않은 작업이 있습니다."));
    else
        body = g_strdup(T("All changes are done.", "모든 변경을 마쳤습니다."));
    g_notification_set_body(n, body);
    g_application_send_notification(G_APPLICATION(A->gapp), "jobs", n);
    g_object_unref(n);
    g_free(body);
}

static void on_job_done(gboolean ok, const char *why, const char *text, gpointer d)
{
    (void)d;
    Op *op = A->running;
    A->running = NULL;
    if (op->pkg) {
        op->pkg->state = ST_IDLE;
        pkg_changed(op->pkg);
    }
    char *title = op_title(op, TRUE, ok);
    gtk_label_set_text(GTK_LABEL(A->job_title), title);
    if (ok) {
        A->jobs_ok++;
        lp_kit_progress_set(A->job_progress, 1.0);
        gtk_label_set_text(GTK_LABEL(A->job_detail), "");
    } else {
        A->jobs_failed++;
        g_free(A->last_fail);
        A->last_fail = g_strdup(title);
        /* The daemon's own sentence: "libgtk-4-1 is part of the desktop
         * itself" says more than any rewording of it here. */
        if (text && strstr(text, "Not enough disk space"))
            text = T("Not enough free space on the disk",
                     "디스크에 남은 공간이 부족합니다");
        char *msg = g_strdup_printf("%s%s%s", text ? text : "",
                                    why && strcmp(why, "failed") == 0 ?
                                    T(" - the log has the reason", " - 로그에 이유가 있습니다") : "",
                                    "");
        gtk_label_set_text(GTK_LABEL(A->job_detail), msg);
        g_free(msg);
        log_append(title);
    }
    g_free(title);
    gboolean cancelled = why && strcmp(why, "cancelled") == 0;
    if (cancelled) {
        /* Nobody typed the password: the rest of the queue is dropped
         * too - asking again for each would be nagging. */
        Op *q;
        while ((q = g_queue_pop_head(&A->queue))) {
            if (q->pkg) {
                q->pkg->state = ST_IDLE;
                pkg_changed(q->pkg);
                g_object_unref(q->pkg);
            }
            g_free(q);
        }
    }
    if (op->pkg)
        g_object_unref(op->pkg);
    g_free(op);

    refresh_facts();
    load_updates();
    if (!g_queue_is_empty(&A->queue)) {
        run_next();
        return;
    }
    if (A->job_hide)
        g_source_remove(A->job_hide);
    A->job_hide = g_timeout_add_seconds(ok ? 4 : 12, hide_jobbar, NULL);
    if (A->held) {
        notify_done();
        A->held = FALSE;
        g_application_release(G_APPLICATION(A->gapp));
    }
}

static void run_next(void)
{
    if (A->running)
        return;
    Op *op = g_queue_pop_head(&A->queue);
    if (!op)
        return;
    A->running = op;
    if (A->job_hide) {
        g_source_remove(A->job_hide);
        A->job_hide = 0;
    }
    if (op->pkg) {
        op->pkg->state = ST_WORKING;
        pkg_changed(op->pkg);
    }
    char *title = op_title(op, FALSE, FALSE);
    gtk_label_set_text(GTK_LABEL(A->job_title), title);
    g_free(title);
    guint waiting = g_queue_get_length(&A->queue);
    if (waiting) {
        char *w = g_strdup_printf(T("%u more waiting", "%u개 대기 중"), waiting);
        gtk_label_set_text(GTK_LABEL(A->job_detail), w);
        g_free(w);
    } else {
        gtk_label_set_text(GTK_LABEL(A->job_detail), "");
    }
    lp_kit_progress_set(A->job_progress, 0.0);
    lp_kit_progress_set(A->job_progress, -1.0);   /* until apt says */
    gtk_revealer_set_reveal_child(GTK_REVEALER(A->jobbar), TRUE);
    char *head = g_strdup_printf("── %s ──", gtk_label_get_text(GTK_LABEL(A->job_title)));
    log_append(head);
    g_free(head);

    const char *fields[4] = { op_verb(op), NULL, NULL, NULL };
    if (op->pkg && op->kind != OP_UPGRADE_ALL && op->kind != OP_REFRESH &&
        op->kind != OP_FLAT_REFRESH && op->kind != OP_FLAT_UPGRADE_ALL)
        fields[1] = op->pkg->name;
    lp_priv_run(win(), fields, op_why(op), on_job_line, on_job_done, NULL);
}

static void queue_op(OpKind kind, LpsPkg *pkg)
{
    if (pkg && pkg->state != ST_IDLE)
        return;
    Op *op = g_new0(Op, 1);
    op->kind = kind;
    op->pkg = pkg ? g_object_ref(pkg) : NULL;
    if (pkg) {
        pkg->state = ST_QUEUED;
        pkg_changed(pkg);
    }
    g_queue_push_tail(&A->queue, op);
    run_next();
}

static void on_remove_confirmed(gboolean ok, gpointer d)
{
    LpsPkg *p = d;
    if (ok)
        queue_op(OP_REMOVE, p);
    g_object_unref(p);
}

/* Removing asks first; installing does not (the password sheet is the
 * pause there, and an install is easily undone). */
static void ask_remove(LpsPkg *p)
{
    char *t = g_strdup_printf(T("Remove %s?", "%s 을(를) 제거할까요?"), p->title);
    char *b = g_strdup_printf(T("The application is removed for every account on "
                                "this computer. Your own files made with it stay.",
                                "이 컴퓨터의 모든 계정에서 제거됩니다. 이 앱으로 "
                                "만든 파일은 그대로 남습니다."));
    lp_kit_confirm(win(), t, b, T("Remove", "제거"), TRUE,
                   on_remove_confirmed, g_object_ref(p));
    g_free(t);
    g_free(b);
}

/* ═══════════════════════════════════════════════════════════════════
 * Pages
 * ═══════════════════════════════════════════════════════════════════ */

static GtkWidget *page_head(const char *title, const char *sub)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(box, "lps-head");
    GtkWidget *t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "lps-h1");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_box_append(GTK_BOX(box), t);
    if (sub) {
        GtkWidget *s = gtk_label_new(sub);
        gtk_widget_add_css_class(s, "lps-sub");
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_label_set_wrap(GTK_LABEL(s), TRUE);
        gtk_box_append(GTK_BOX(box), s);
    }
    return box;
}

/* ── a recommendation card ────────────────────────────────────────── */

typedef struct {
    LpsPkg    *pkg;
    GtkWidget *icon;
    GtkWidget *size;
    GtkWidget *action;
    GtkWidget *open;
} Card;

static void card_fill(Card *c)
{
    LpsPkg *p = c->pkg;
    set_icon(c->icon, p->icon);
    char *s = size_line(p);
    gtk_label_set_text(GTK_LABEL(c->size), *s ? s : " ");
    g_free(s);
    action_label(p, c->action, FALSE);
    gtk_widget_set_visible(c->open, p->installed && p->desktop_id);
}

static void on_card_changed(LpsPkg *p, gpointer d)
{
    (void)p;
    card_fill(d);
}

static void on_card_action(GtkButton *b, gpointer d)
{
    (void)b;
    Card *c = d;
    if (c->pkg->installed)
        ask_remove(c->pkg);
    else
        queue_op(OP_INSTALL, c->pkg);
}

static void on_card_open(GtkButton *b, gpointer d)
{
    (void)b;
    launch_pkg(((Card *)d)->pkg);
}

static GtkWidget *card_new(LpsPkg *p)
{
    Card *c = g_new0(Card, 1);
    c->pkg = p;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(box, "lps-card");
    g_object_set_data_full(G_OBJECT(box), "card", c, g_free);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    c->icon = icon_tile(p->icon, 56);
    gtk_box_append(GTK_BOX(top), c->icon);
    GtkWidget *tx = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_valign(tx, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(tx, TRUE);
    GtkWidget *t = gtk_label_new(p->title);
    gtk_widget_add_css_class(t, "lps-card-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0);
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
    GtkWidget *s = gtk_label_new(p->summary);
    gtk_widget_add_css_class(s, "lps-card-sum");
    gtk_label_set_xalign(GTK_LABEL(s), 0);
    gtk_label_set_wrap(GTK_LABEL(s), TRUE);
    gtk_label_set_lines(GTK_LABEL(s), 2);
    gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(tx), t);
    gtk_box_append(GTK_BOX(tx), s);
    gtk_box_append(GTK_BOX(top), tx);
    gtk_box_append(GTK_BOX(box), top);

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(bottom, 8);
    c->size = gtk_label_new(" ");
    gtk_widget_add_css_class(c->size, "lps-row-meta");
    gtk_label_set_xalign(GTK_LABEL(c->size), 0);
    gtk_widget_set_hexpand(c->size, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(c->size), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(bottom), c->size);
    c->open = gtk_button_new_with_label(T("Open", "열기"));
    gtk_widget_add_css_class(c->open, "lps-action");
    g_signal_connect(c->open, "clicked", G_CALLBACK(on_card_open), c);
    gtk_box_append(GTK_BOX(bottom), c->open);
    c->action = gtk_button_new_with_label("");
    gtk_widget_add_css_class(c->action, "lps-action");
    char *nm = g_strdup_printf("card-%s", p->name);
    gtk_widget_set_name(c->action, nm);
    g_free(nm);
    g_signal_connect(c->action, "clicked", G_CALLBACK(on_card_action), c);
    gtk_box_append(GTK_BOX(bottom), c->action);
    gtk_box_append(GTK_BOX(box), bottom);

    /* The cards live as long as the window, and so do their packages. */
    g_signal_connect(p, "changed", G_CALLBACK(on_card_changed), c);
    card_fill(c);
    return box;
}

/* A heading and its cards. A grid of three, not a GtkFlowBox: 4.8's
 * homogeneous flow box under-measures its height for wrapped two-line
 * labels, and the last rows were cut off inside the scrolled window. */
static GtkWidget *explore_section(const char *title, const char *sub,
                                  const Featured *list, guint n, PkgSource src)
{
    GtkWidget *sec = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *h = gtk_label_new(title);
    gtk_widget_add_css_class(h, "lps-h2");
    gtk_label_set_xalign(GTK_LABEL(h), 0);
    gtk_box_append(GTK_BOX(sec), h);
    if (sub) {
        GtkWidget *s = gtk_label_new(sub);
        gtk_widget_add_css_class(s, "lps-sub");
        gtk_label_set_xalign(GTK_LABEL(s), 0);
        gtk_label_set_wrap(GTK_LABEL(s), TRUE);
        gtk_box_append(GTK_BOX(sec), s);
    }
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    for (guint i = 0; i < n; i++) {
        const Featured *f = &list[i];
        LpsPkg *p = pkg_new(src, f->pkg);
        g_free(p->title);
        p->title = g_strdup(T(f->en, f->ko));
        p->summary = g_strdup(T(f->sum_en, f->sum_ko));
        p->icon = src == SRC_FLATPAK ? flatpak_icon(f->pkg) : g_strdup(f->icon);
        g_ptr_array_add(A->featured, p);
        gtk_grid_attach(GTK_GRID(grid), card_new(p), (int)(i % 3), (int)(i / 3), 1, 1);
    }
    gtk_box_append(GTK_BOX(sec), grid);
    return sec;
}

static GtkWidget *build_explore(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(box, "lps-page");
    gtk_box_append(GTK_BOX(box), page_head(T("Explore", "둘러보기"),
        T("Applications that are not on LP from the start, because not "
          "everybody needs them. One tap installs.",
          "모두에게 필요하지는 않아서 처음부터 들어 있지 않은 앱들입니다. "
          "한 번 누르면 설치됩니다.")));
    gtk_box_append(GTK_BOX(box), explore_section(
        T("Popular apps", "인기 앱"),
        T("From Flathub - the apps Debian does not carry, and the newest "
          "versions of many it does.",
          "Flathub에서 받습니다. 데비안에 없는 앱과, 있어도 더 새 버전인 앱들입니다."),
        POPULAR, G_N_ELEMENTS(POPULAR), SRC_FLATPAK));
    GtkWidget *flow = explore_section(T("From Debian", "데비안에서"), NULL,
        FEATURED, G_N_ELEMENTS(FEATURED), SRC_DEBIAN);
    A->featured_box = flow;
    gtk_box_append(GTK_BOX(box), flow);
    return lp_kit_scroller(box, FALSE);
}

static GtkWidget *build_search(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(box, "lps-page");
    A->search_status = gtk_label_new("");
    gtk_widget_add_css_class(A->search_status, "lps-h2");
    gtk_label_set_xalign(GTK_LABEL(A->search_status), 0);
    gtk_box_append(GTK_BOX(box), A->search_status);
    gtk_box_append(GTK_BOX(box), pkg_list(A->search_store, FALSE));
    return box;
}

static void on_installed_seg(GtkToggleButton *b, gpointer d)
{
    if (!gtk_toggle_button_get_active(b))
        return;
    lp_kit_stack_show(GTK_STACK(A->installed_stack), d);
    g_key_file_set_string(A->state, "software", "installed-view", d);
    save_state();
}

static GtkWidget *build_installed(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(box, "lps-page");
    gtk_box_append(GTK_BOX(box), page_head(T("Installed", "설치됨"), NULL));

    GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(seg, "linked");
    gtk_widget_add_css_class(seg, "lps-seg");
    gtk_widget_set_halign(seg, GTK_ALIGN_START);
    const char *ids[] = { "apps", "all", "lp" };
    const char *labels[] = { T("Applications", "앱"),
                             T("All packages", "모든 패키지"),
                             T("LP packages", "LP 패키지") };
    char *want = g_key_file_get_string(A->state, "software", "installed-view", NULL);
    GtkWidget *group = NULL, *active = NULL;
    A->installed_stack = lp_kit_fade_stack();
    GListStore *stores[] = { A->apps_store, A->all_store, A->lp_store };
    for (int i = 0; i < 3; i++) {
        GtkWidget *b = gtk_toggle_button_new_with_label(labels[i]);
        if (group)
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b), GTK_TOGGLE_BUTTON(group));
        else
            group = b;
        g_signal_connect(b, "toggled", G_CALLBACK(on_installed_seg), (gpointer)ids[i]);
        char *nm = g_strdup_printf("seg-%s", ids[i]);
        gtk_widget_set_name(b, nm);
        g_free(nm);
        gtk_box_append(GTK_BOX(seg), b);
        gtk_stack_add_named(GTK_STACK(A->installed_stack),
                            pkg_list(stores[i], FALSE), ids[i]);
        if ((want && strcmp(want, ids[i]) == 0) || (!want && i == 0))
            active = b;
    }
    g_free(want);
    gtk_box_append(GTK_BOX(box), seg);
    gtk_box_append(GTK_BOX(box), A->installed_stack);
    if (active)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(active), TRUE);
    return box;
}

static gboolean have_flatpaks(void)
{
    return A->facts && g_hash_table_size(A->facts->flatpak) > 0;
}

static void on_check_updates(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    queue_op(OP_REFRESH, NULL);
    queue_op(OP_FLAT_REFRESH, NULL);
}

static void on_update_all(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    queue_op(OP_UPGRADE_ALL, NULL);
    if (have_flatpaks())
        queue_op(OP_FLAT_UPGRADE_ALL, NULL);
}

static GtkWidget *build_updates(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(box, "lps-page");
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *h = page_head(T("Updates", "업데이트"), NULL);
    gtk_widget_set_hexpand(h, TRUE);
    A->updates_title = gtk_label_new("");
    gtk_widget_add_css_class(A->updates_title, "lps-sub");
    gtk_label_set_xalign(GTK_LABEL(A->updates_title), 0);
    gtk_box_append(GTK_BOX(h), A->updates_title);
    gtk_box_append(GTK_BOX(head), h);
    GtkWidget *check = lp_kit_button("view-refresh-symbolic", T("Check now", "지금 확인"), NULL);
    gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
    g_signal_connect(check, "clicked", G_CALLBACK(on_check_updates), NULL);
    gtk_box_append(GTK_BOX(head), check);
    A->update_all = lp_kit_button(NULL, T("Update all", "모두 업데이트"), "suggested-action");
    gtk_widget_set_name(A->update_all, "update-all");
    gtk_widget_set_name(check, "check-now");
    gtk_widget_set_valign(A->update_all, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive(A->update_all, FALSE);
    g_signal_connect(A->update_all, "clicked", G_CALLBACK(on_update_all), NULL);
    gtk_box_append(GTK_BOX(head), A->update_all);
    gtk_box_append(GTK_BOX(box), head);
    gtk_box_append(GTK_BOX(box), pkg_list(A->updates_store, TRUE));
    return box;
}

/* Where packages come from. Shown, with the files they are in; changing
 * a source is an administrator editing /etc/apt, and the page says how
 * rather than offering a form that would need yet another root verb. */
static void add_source_row(GtkWidget *box, const char *where, const char *line)
{
    GtkWidget *r = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(r, "lps-source");
    GtkWidget *a = gtk_label_new(line);
    gtk_widget_add_css_class(a, "lps-mono");
    gtk_label_set_xalign(GTK_LABEL(a), 0);
    gtk_label_set_wrap(GTK_LABEL(a), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(a), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(a), TRUE);
    GtkWidget *b = gtk_label_new(where);
    gtk_widget_add_css_class(b, "lps-row-meta");
    gtk_label_set_xalign(GTK_LABEL(b), 0);
    gtk_box_append(GTK_BOX(r), a);
    gtk_box_append(GTK_BOX(r), b);
    gtk_box_append(GTK_BOX(box), r);
}

static void scan_sources_file(GtkWidget *box, const char *path)
{
    char *text = NULL;
    if (!g_file_get_contents(path, &text, NULL, NULL))
        return;
    gboolean deb822 = g_str_has_suffix(path, ".sources");
    char **lines = g_strsplit(text, "\n", 0);
    GString *para = g_string_new(NULL);
    for (int i = 0; lines[i]; i++) {
        char *l = g_strstrip(lines[i]);
        if (!deb822) {
            if (*l && *l != '#')
                add_source_row(box, path, l);
            continue;
        }
        if (!*l) {
            if (para->len)
                add_source_row(box, path, para->str);
            g_string_truncate(para, 0);
        } else if (*l != '#' && (g_str_has_prefix(l, "URIs:") ||
                   g_str_has_prefix(l, "Suites:") || g_str_has_prefix(l, "Components:"))) {
            if (para->len)
                g_string_append(para, "  ");
            g_string_append(para, l);
        }
    }
    if (para->len)
        add_source_row(box, path, para->str);
    g_string_free(para, TRUE);
    g_strfreev(lines);
    g_free(text);
}

static GtkWidget *build_sources(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(box, "lps-page");
    gtk_box_append(GTK_BOX(box), page_head(T("Sources", "소프트웨어 출처"),
        T("Where Debian packages and LP packages are downloaded from. An "
          "administrator changes these in a terminal: sudo nano "
          "/etc/apt/sources.list, then Check now on Updates.",
          "데비안 패키지와 LP 패키지를 받는 곳입니다. 바꾸려면 관리자가 터미널에서 "
          "sudo nano /etc/apt/sources.list 로 고친 뒤 업데이트에서 지금 확인을 "
          "누르세요.")));
    GtkWidget *h = gtk_label_new(T("Debian", "데비안"));
    gtk_widget_add_css_class(h, "lps-h2");
    gtk_label_set_xalign(GTK_LABEL(h), 0);
    gtk_box_append(GTK_BOX(box), h);
    scan_sources_file(box, "/etc/apt/sources.list");
    GDir *d = g_dir_open("/etc/apt/sources.list.d", 0, NULL);
    if (d) {
        const char *f;
        while ((f = g_dir_read_name(d))) {
            if (!g_str_has_suffix(f, ".list") && !g_str_has_suffix(f, ".sources"))
                continue;
            char *p = g_build_filename("/etc/apt/sources.list.d", f, NULL);
            scan_sources_file(box, p);
            g_free(p);
        }
        g_dir_close(d);
    }
    GtkWidget *h2 = gtk_label_new(T("LP packages", "LP 패키지"));
    gtk_widget_add_css_class(h2, "lps-h2");
    gtk_label_set_xalign(GTK_LABEL(h2), 0);
    gtk_widget_set_margin_top(h2, 12);
    gtk_box_append(GTK_BOX(box), h2);
    char *repo = NULL;
    if (g_file_get_contents(LP_REPO, &repo, NULL, NULL))
        add_source_row(box, LP_REPO, g_strstrip(repo));
    else
        add_source_row(box, T("built into pkg", "pkg 기본값"),
                       T("The default LP repository (pkg repo shows it)",
                         "LP 기본 저장소 (pkg repo 로 확인)"));
    g_free(repo);
    A->sources_box = box;
    return lp_kit_scroller(box, FALSE);
}

/* ── the job bar ──────────────────────────────────────────────────── */

static void on_log_toggle(GtkToggleButton *b, gpointer d)
{
    (void)d;
    gtk_revealer_set_reveal_child(GTK_REVEALER(A->job_log_rev),
                                  gtk_toggle_button_get_active(b));
}

static GtkWidget *build_jobbar(void)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "lps-job");
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *tx = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(tx, TRUE);
    A->job_title = gtk_label_new("");
    gtk_widget_add_css_class(A->job_title, "lps-job-title");
    gtk_label_set_xalign(GTK_LABEL(A->job_title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(A->job_title), PANGO_ELLIPSIZE_END);
    A->job_detail = gtk_label_new("");
    gtk_widget_add_css_class(A->job_detail, "lps-row-meta");
    gtk_label_set_xalign(GTK_LABEL(A->job_detail), 0);
    gtk_label_set_ellipsize(GTK_LABEL(A->job_detail), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(tx), A->job_title);
    gtk_box_append(GTK_BOX(tx), A->job_detail);
    gtk_box_append(GTK_BOX(top), tx);
    A->job_log_btn = gtk_toggle_button_new_with_label(T("Log", "로그"));
    gtk_widget_add_css_class(A->job_log_btn, "lps-action");
    gtk_widget_set_valign(A->job_log_btn, GTK_ALIGN_CENTER);
    g_signal_connect(A->job_log_btn, "toggled", G_CALLBACK(on_log_toggle), NULL);
    gtk_widget_set_name(A->job_log_btn, "job-log");
    gtk_box_append(GTK_BOX(top), A->job_log_btn);
    gtk_box_append(GTK_BOX(card), top);
    A->job_progress = lp_kit_progress_new();
    gtk_box_append(GTK_BOX(card), A->job_progress);

    A->job_log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(A->job_log), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(A->job_log), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(A->job_log), GTK_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(A->job_log, "lps-log");
    GtkWidget *sc = lp_kit_scroller(A->job_log, FALSE);
    gtk_widget_set_size_request(sc, -1, 200);
    /* A scroller expands, and expand propagates up through the revealer
     * into the window's main column, where a hidden job bar then took
     * half the height as empty space. The log is a fixed 200px. */
    gtk_widget_set_vexpand(sc, FALSE);
    A->job_log_rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(A->job_log_rev),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(A->job_log_rev),
        lp_spring_ms(LP_SPRING_EXPAND, FALSE));
    gtk_revealer_set_child(GTK_REVEALER(A->job_log_rev), sc);
    gtk_box_append(GTK_BOX(card), A->job_log_rev);

    A->jobbar = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(A->jobbar),
        GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration(GTK_REVEALER(A->jobbar),
        lp_spring_ms(LP_SPRING_SHEET, FALSE));
    gtk_revealer_set_child(GTK_REVEALER(A->jobbar), card);
    gtk_widget_set_vexpand(A->jobbar, FALSE);
    return A->jobbar;
}

/* ── sidebar ──────────────────────────────────────────────────────── */

static const struct { const char *id, *icon, *en, *ko; } PAGES[] = {
    { "explore",   "system-software-install-symbolic", "Explore",   "둘러보기" },
    { "installed", "view-grid-symbolic",               "Installed", "설치됨" },
    { "updates",   "software-update-available-symbolic", "Updates", "업데이트" },
    { "sources",   "network-server-symbolic",          "Sources",   "소프트웨어 출처" },
};

static void go_page(const char *id)
{
    g_free(A->page);
    A->page = g_strdup(id);
    if (*gtk_editable_get_text(GTK_EDITABLE(A->search)))
        gtk_editable_set_text(GTK_EDITABLE(A->search), "");
    lp_kit_stack_show(GTK_STACK(A->stack), id);
    g_key_file_set_string(A->state, "software", "page", id);
    save_state();
}

static void on_side_row(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    (void)lb; (void)d;
    if (!row)
        return;
    go_page(PAGES[gtk_list_box_row_get_index(row)].id);
}

static GtkWidget *build_sidebar(void)
{
    GtkWidget *lb = gtk_list_box_new();
    gtk_widget_add_css_class(lb, "lp-kit-side");
    for (guint i = 0; i < G_N_ELEMENTS(PAGES); i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_box_append(GTK_BOX(row), gtk_image_new_from_icon_name(PAGES[i].icon));
        GtkWidget *l = gtk_label_new(T(PAGES[i].en, PAGES[i].ko));
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_box_append(GTK_BOX(row), l);
        if (strcmp(PAGES[i].id, "updates") == 0) {
            A->updates_badge = gtk_label_new("");
            gtk_widget_add_css_class(A->updates_badge, "lp-kit-badge");
            gtk_widget_set_visible(A->updates_badge, FALSE);
            gtk_box_append(GTK_BOX(row), A->updates_badge);
        }
        gtk_list_box_append(GTK_LIST_BOX(lb), row);
        char *nm = g_strdup_printf("side-%s", PAGES[i].id);
        gtk_widget_set_name(GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(lb), (int)i)), nm);
        g_free(nm);
    }
    g_signal_connect(lb, "row-activated", G_CALLBACK(on_side_row), NULL);
    GtkWidget *sc = lp_kit_scroller(lb, FALSE);
    gtk_widget_add_css_class(sc, "lp-kit-sidebar");
    gtk_widget_set_size_request(sc, SIDEBAR_W, -1);
    gtk_widget_set_hexpand(sc, FALSE);
    A->sidebar = lb;
    return sc;
}

static void select_side(const char *id)
{
    for (guint i = 0; i < G_N_ELEMENTS(PAGES); i++)
        if (strcmp(PAGES[i].id, id) == 0)
            gtk_list_box_select_row(GTK_LIST_BOX(A->sidebar),
                gtk_list_box_get_row_at_index(GTK_LIST_BOX(A->sidebar), (int)i));
}

/* ── who we are ───────────────────────────────────────────────────── */

static void on_ping(gboolean ok, const char *why, const char *text, gpointer d)
{
    (void)why; (void)d;
    A->admin = ok && text && strstr(text, "admin=yes");
    if (!ok) {
        gtk_label_set_text(GTK_LABEL(A->banner_label),
            T("The system service that installs software (lp-privd) is not "
              "running. You can look, but not change anything.",
              "소프트웨어를 설치하는 시스템 서비스(lp-privd)가 실행 중이 아닙니다. "
              "볼 수는 있지만 바꿀 수는 없습니다."));
        gtk_widget_set_visible(A->banner_button, FALSE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(A->banner), TRUE);
    } else if (!A->admin) {
        gtk_label_set_text(GTK_LABEL(A->banner_label),
            T("Only an administrator can install or remove software. Ask an "
              "administrator of this computer.",
              "소프트웨어 설치와 제거는 관리자만 할 수 있습니다. 이 컴퓨터의 "
              "관리자에게 부탁하세요."));
        gtk_widget_set_visible(A->banner_button, FALSE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(A->banner), TRUE);
    }
    for (guint i = 0; i < A->featured->len; i++)
        pkg_changed(g_ptr_array_index(A->featured, i));
    refresh_store(A->apps_store);
    load_updates();
    /* Flathub's catalogue - the popular apps' icons, and what search finds
     * there - is fetched when there is none or it is a day old. Any
     * account may ask for it; it asks for no password. */
    GStatBuf st;
    if (g_stat(FLATHUB_AS "/appstream.xml.gz", &st) != 0 ||
        g_get_real_time() / G_USEC_PER_SEC - st.st_mtime > 24 * 3600)
        queue_op(OP_FLAT_REFRESH, NULL);
}

static void on_banner_button(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    gtk_revealer_set_reveal_child(GTK_REVEALER(A->banner), FALSE);
    queue_op(OP_REFRESH, NULL);
    queue_op(OP_FLAT_REFRESH, NULL);
}

/* ═══════════════════════════════════════════════════════════════════
 * Putting it together
 * ═══════════════════════════════════════════════════════════════════ */

static const char APP_CSS[] =
    ".lps-main { background-color: #161616; }\n"
    ".lps-page { padding: 22px 26px 18px 26px; }\n"
    ".lps-h1 { font-size: 22px; font-weight: 600; letter-spacing: -0.02em; }\n"
    ".lps-h2 { font-size: 15px; font-weight: 600; color: #e8e8e8; }\n"
    ".lps-sub { font-size: 13px; color: #8a8a8a; }\n"
    ".lps-card { background-color: #232323; border-radius: 12px; padding: 16px;"
    "  box-shadow: 0 0 0 0.5px #2e2e2e; }\n"
    ".lps-card-title { font-size: 15px; font-weight: 600; }\n"
    ".lps-card-sum { font-size: 13px; color: #a8a8a8; }\n"
    ".lps-row { padding: 10px 12px; min-height: 52px; border-bottom: 0.5px solid #262626; }\n"
    ".lps-row-title { font-size: 14px; font-weight: 600; }\n"
    ".lps-row-summary { font-size: 13px; color: #a8a8a8; }\n"
    ".lps-row-meta { font-size: 12px; color: #6a6a6a; font-feature-settings: 'tnum'; }\n"
    ".lps-list { background: none; }\n"
    ".lps-list > row { padding: 0; background: none; }\n"
    ".lps-list > row:hover { background-color: rgba(255,255,255,0.03); }\n"
    ".lps-action { min-height: 40px; min-width: 96px; border-radius: 8px; padding: 0 14px; }\n"
    ".lps-seg button { min-height: 40px; padding: 0 16px; }\n"
    ".lps-job { background-color: #232323; border-top: 0.5px solid #333333;"
    "  padding: 14px 22px 16px 22px; }\n"
    ".lps-job-title { font-size: 14px; font-weight: 600; }\n"
    ".lps-log { background-color: #121212; color: #b8b8b8; font-size: 12px;"
    "  padding: 8px; border-radius: 8px; }\n"
    ".lps-source { background-color: #232323; border-radius: 8px; padding: 10px 14px; }\n"
    ".lps-mono { font-family: 'D2Coding', monospace; font-size: 13px; }\n"
    ".lps-banner { background-color: #2a2113; color: #f0b350; padding: 10px 22px;"
    "  border-bottom: 0.5px solid #4a3510; }\n"
    ".lps-banner label { color: #f0b350; }\n"
    ".lps-search { min-height: 38px; min-width: 380px; border-radius: 8px; }\n";

static gboolean on_close_request(GtkWindow *w, gpointer d)
{
    (void)d;
    if (A->running || !g_queue_is_empty(&A->queue)) {
        /* Work in flight: hide, keep going, tell them at the end. */
        if (!A->held) {
            A->held = TRUE;
            g_application_hold(G_APPLICATION(A->gapp));
        }
        gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
        return TRUE;
    }
    return FALSE;
}

static void build_window(const char *start)
{
    A->win = gtk_application_window_new(A->gapp);
    gtk_window_set_title(GTK_WINDOW(A->win), T("Software", "소프트웨어"));
    lp_fit_default_size(GTK_WINDOW(A->win), WINDOW_W, WINDOW_H);
    gtk_window_set_icon_name(GTK_WINDOW(A->win), "system-software-install");
    g_signal_connect(A->win, "close-request", G_CALLBACK(on_close_request), NULL);

    GtkWidget *hb = gtk_header_bar_new();
    A->search = gtk_search_entry_new();
    g_object_set(A->search, "placeholder-text",
                 T("Search Flathub, Debian and LP", "Flathub, 데비안, LP에서 검색"), NULL);
    gtk_widget_add_css_class(A->search, "lps-search");
    gtk_widget_set_name(A->search, "search");
    g_signal_connect(A->search, "search-changed", G_CALLBACK(on_search_changed), NULL);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), A->search);
    gtk_window_set_titlebar(GTK_WINDOW(A->win), hb);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_append(GTK_BOX(outer), build_sidebar());

    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(main, "lps-main");
    gtk_widget_set_hexpand(main, TRUE);

    GtkWidget *bbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(bbox, "lps-banner");
    A->banner_label = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(A->banner_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(A->banner_label), 0);
    gtk_widget_set_hexpand(A->banner_label, TRUE);
    gtk_box_append(GTK_BOX(bbox), A->banner_label);
    A->banner_button = gtk_button_new_with_label(T("Check for updates", "업데이트 확인"));
    gtk_widget_add_css_class(A->banner_button, "lps-action");
    g_signal_connect(A->banner_button, "clicked", G_CALLBACK(on_banner_button), NULL);
    gtk_box_append(GTK_BOX(bbox), A->banner_button);
    A->banner = gtk_revealer_new();
    gtk_revealer_set_transition_duration(GTK_REVEALER(A->banner),
        lp_spring_ms(LP_SPRING_EXPAND, FALSE));
    gtk_revealer_set_child(GTK_REVEALER(A->banner), bbox);
    gtk_box_append(GTK_BOX(main), A->banner);

    A->stack = lp_kit_stack();
    gtk_widget_set_vexpand(A->stack, TRUE);
    gtk_stack_add_named(GTK_STACK(A->stack), build_explore(), "explore");
    gtk_stack_add_named(GTK_STACK(A->stack), build_installed(), "installed");
    gtk_stack_add_named(GTK_STACK(A->stack), build_updates(), "updates");
    gtk_stack_add_named(GTK_STACK(A->stack), build_sources(), "sources");
    gtk_stack_add_named(GTK_STACK(A->stack), build_search(), "search");
    gtk_box_append(GTK_BOX(main), A->stack);
    gtk_box_append(GTK_BOX(main), build_jobbar());
    gtk_box_append(GTK_BOX(outer), main);
    gtk_window_set_child(GTK_WINDOW(A->win), outer);

    char *saved = g_key_file_get_string(A->state, "software", "page", NULL);
    const char *page = start ? start : (saved ? saved : "explore");
    gboolean known = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(PAGES); i++)
        known |= strcmp(PAGES[i].id, page) == 0;
    if (!known)
        page = "explore";
    A->page = g_strdup(page);
    gtk_stack_set_transition_duration(GTK_STACK(A->stack), 0);
    gtk_stack_set_visible_child_name(GTK_STACK(A->stack), page);
    gtk_stack_set_transition_duration(GTK_STACK(A->stack), 260);
    select_side(page);
    g_free(saved);
}

static char *opt_page;

static void on_activate(GtkApplication *app, gpointer d)
{
    (void)d;
    if (A->win) {
        gtk_widget_set_visible(A->win, TRUE);
        gtk_window_present(GTK_WINDOW(A->win));
        return;
    }
    lp_kit_style(APP_CSS);
    A->gapp = app;
    build_window(opt_page);
    gtk_window_present(GTK_WINDOW(A->win));
    refresh_facts();
    load_featured_sizes();
    lp_kit_drive(NULL, NULL);
    const char *ping[] = { "ping", NULL };
    lp_priv_run(win(), ping, NULL, NULL, on_ping, NULL);
}

int main(int argc, char **argv)
{
    /* --page=NAME is ours; take it out before GApplication sees it. */
    int out = 1;
    for (int i = 1; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "--page="))
            opt_page = g_strdup(argv[i] + 7);
        else
            argv[out++] = argv[i];
    }
    argc = out;
    argv[argc] = NULL;

    A = g_new0(App, 1);
    g_queue_init(&A->queue);
    A->featured = g_ptr_array_new_with_free_func(g_object_unref);
    A->search_store = g_list_store_new(LPS_TYPE_PKG);
    A->apps_store = g_list_store_new(LPS_TYPE_PKG);
    A->all_store = g_list_store_new(LPS_TYPE_PKG);
    A->lp_store = g_list_store_new(LPS_TYPE_PKG);
    A->updates_store = g_list_store_new(LPS_TYPE_PKG);
    A->state = lp_kit_state_load(STATE_NAME);

    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return rc;
}
