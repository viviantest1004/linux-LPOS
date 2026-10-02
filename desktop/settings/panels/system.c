/*
 * system.c - System: what this machine is (the logo, the OS and its
 * version, the hardware), its name on the network, the full report from
 * `info`, and restarting into Recovery.
 *
 * ── What is read, from where ──
 *
 *   OS name and version   /etc/os-release (PRETTY_NAME), the branding
 *                         track's file - never a string in this program
 *   kernel                uname
 *   processor             /proc/cpuinfo's model name, and how many
 *   memory                /proc/meminfo
 *   graphics              `lspci -mm` display controllers (both GPUs of
 *                         the XPS's Optimus pair); /sys/class/drm's
 *                         drivers when lspci is not installed
 *   disk                  statvfs of /
 *   everything            `info`, our own command, shown whole in a
 *                         dialog with a button that copies it - the
 *                         text to paste under a question
 *
 * ── The name ──
 *
 * `hostnamectl set-hostname NAME`, as administrator. The name is checked
 * here first against what a host name may be (RFC 1123: letters, digits
 * and hyphens, not starting or ending with a hyphen, 63 at most), so a
 * typo is answered in the dialog rather than by a tool's error.
 *
 * ── Recovery ──
 *
 * `lp-reboot-recovery` (the boot-recovery track's) sets the one-shot
 * boot entry and restarts; it needs root, so it runs through the
 * administrator password dialog like everything else here. The row is
 * there only when the command is installed.
 */
#include "core.h"

#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static char *cpu_words(void)
{
    char *ci = lp_slurp("/proc/cpuinfo");
    if (!ci) return NULL;
    char model[256] = "";
    int n = 0;
    char **l = g_strsplit(ci, "\n", -1);
    for (int i = 0; l[i]; i++) {
        if (g_str_has_prefix(l[i], "processor")) n++;
        if (!model[0] && g_str_has_prefix(l[i], "model name")) {
            const char *c = strchr(l[i], ':');
            if (c) g_strlcpy(model, c + 1 + strspn(c + 1, " \t"), sizeof model);
        }
    }
    g_strfreev(l);
    g_free(ci);
    return g_strdup_printf(T("%s · %d threads", "%s · %d 스레드"), model[0] ? model : T("Unknown", "알 수 없음"), n);
}

static char *mem_words(void)
{
    char *mi = lp_slurp("/proc/meminfo");
    unsigned long kb = 0;
    if (mi) sscanf(mi, "MemTotal: %lu kB", &kb);
    g_free(mi);
    if (!kb) return NULL;
    /* Memory is sold in powers of two: 16 GB, not 16.6 GB. */
    double gib = kb / 1048576.0;
    return g_strdup_printf("%.1f GB", gib);
}

static char *gpu_words(void)
{
    static const char *const v[] = { "lspci", "-mm", NULL };
    char *out = lp_run(v);
    GString *s = g_string_new(NULL);
    if (out) {
        char **l = g_strsplit(out, "\n", -1);
        for (int i = 0; l[i]; i++) {
            if (!strstr(l[i], "\"VGA compatible controller\"") && !strstr(l[i], "\"3D controller\"") &&
                !strstr(l[i], "\"Display controller\""))
                continue;
            /* slot "class" "vendor" "device" ... */
            char **f = g_strsplit(l[i], "\"", -1);
            if (g_strv_length(f) >= 6) {
                const char *vendor = f[3], *dev = f[5];
                const char *short_v = strstr(vendor, "Intel") ? "Intel" : strstr(vendor, "NVIDIA") ? "NVIDIA"
                                    : strstr(vendor, "AMD") || strstr(vendor, "ATI") ? "AMD" : vendor;
                if (s->len) g_string_append(s, " + ");
                g_string_append_printf(s, "%s %s", short_v, dev);
            }
            g_strfreev(f);
        }
        g_strfreev(l);
        g_free(out);
    }
    if (!s->len) {
        GDir *d = g_dir_open("/sys/class/drm", 0, NULL);
        const char *n;
        while (d && (n = g_dir_read_name(d))) {
            if (!g_str_has_prefix(n, "card") || strchr(n, '-')) continue;
            char *p = g_strdup_printf("/sys/class/drm/%s/device/driver", n);
            char *t = g_file_read_link(p, NULL);
            if (t) {
                char *b = g_path_get_basename(t);
                if (s->len) g_string_append(s, " + ");
                g_string_append(s, b);
                g_free(b); g_free(t);
            }
            g_free(p);
        }
        if (d) g_dir_close(d);
    }
    if (!s->len) { g_string_free(s, TRUE); return NULL; }
    return g_string_free(s, FALSE);
}

/* ── the name ───────────────────────────────────────────────────────── */

static gboolean valid_hostname(const char *h)
{
    size_t n = strlen(h);
    if (!n || n > 63 || h[0] == '-' || h[n - 1] == '-') return FALSE;
    for (const char *p = h; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '-') return FALSE;
    return TRUE;
}

static void rename_done(int st, const char *out, const char *err, gpointer p)
{
    lp_dialog_t *d = p;
    lp_dialog_busy(d, FALSE);
    if (st == 0) {
        lp_toast(FALSE, T("This machine is now called %s", "이 기기의 이름은 이제 %s 입니다"),
                 (char *)lp_dialog_get_data(d, "lp-new"));
        lp_dialog_close(d);
        lp_refresh();
    } else if (st != -2) {
        char *why = st == -1 ? g_strdup(T("hostnamectl is not installed", "hostnamectl 이 설치되어 있지 않습니다"))
                             : lp_first_line(err, out);
        lp_dialog_error(d, why);
        g_free(why);
    }
}

static void rename_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    GtkWidget *e = lp_dialog_get_data(d, "lp-entry");
    char *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(e))));
    if (!valid_hostname(name)) {
        lp_dialog_error(d, T("Use letters, digits and hyphens (-), up to 63, not starting or ending with a hyphen.",
                             "영문자, 숫자, 하이픈(-)으로 63자까지 쓰고, 하이픈으로 시작하거나 끝나지 않게 하십시오."));
        g_free(name);
        return;
    }
    lp_dialog_set_data(d, "lp-new", name, g_free);
    lp_dialog_busy(d, TRUE);
    const char *v[] = { "hostnamectl", "set-hostname", name, NULL };
    lp_admin_run(v, NULL, T("Renaming this machine needs an administrator.",
                            "기기 이름을 바꾸려면 관리자 권한이 필요합니다."),
                 lp_dialog_window(d), rename_done, d);
}

static void on_rename(GtkButton *b, gpointer p)
{
    (void)b;
    const char *cur = p;
    lp_dialog_t *d = lp_dialog_new(T("Rename this machine", "기기 이름 바꾸기"), T("Rename", "바꾸기"),
                                   FALSE, rename_ok, NULL);
    lp_dialog_text(d, T("Other computers on the network see this name.", "네트워크의 다른 컴퓨터에 보이는 이름입니다."), NULL);
    GtkWidget *e = lp_dialog_entry(d, T("Name", "이름"), cur, FALSE);
    lp_dialog_set_data(d, "lp-entry", e, NULL);
    lp_dialog_present(d);
}

/* ── info ───────────────────────────────────────────────────────────── */

static void on_copy(GtkButton *b, gpointer p)
{
    (void)p;
    GtkWidget *label = g_object_get_data(G_OBJECT(b), "lp-text");
    gdk_clipboard_set_text(gtk_widget_get_clipboard(label), gtk_label_get_text(GTK_LABEL(label)));
    lp_toast(FALSE, T("Copied", "복사했습니다"));
}

static void info_done(int st, const char *out, const char *err, gpointer p)
{
    GtkWidget *label = p;
    char *t = st == 0 ? g_strdup(out) : g_strdup_printf("info: %s", st == -1 ? T("not installed", "설치되어 있지 않음") : err);
    gtk_label_set_text(GTK_LABEL(label), t);
    g_free(t);
}

/* ── usage, live ─────────────────────────────────────────────────── */

typedef struct {
    GtkWidget *cpu, *mem, *disk, *up;
    guint64 idle0, total0;
    int ncpu;
    guint timer;
} Usage;

static void usage_free(gpointer p)
{
    Usage *u = p;
    if (u->timer)
        g_source_remove(u->timer);
    g_free(u);
}

static gboolean usage_tick(gpointer p)
{
    Usage *u = p;
    char *stat = lp_slurp("/proc/stat");
    if (stat) {
        guint64 v[10] = { 0 };
        if (sscanf(stat, "cpu %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
                   " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT
                   " %" G_GUINT64_FORMAT " %" G_GUINT64_FORMAT,
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 4) {
            guint64 idle = v[3] + v[4], total = 0;
            for (int i = 0; i < 8; i++) total += v[i];
            if (u->total0 && total > u->total0) {
                double busy = 1.0 - (double)(idle - u->idle0) / (double)(total - u->total0);
                char *t = g_strdup_printf(T("%.0f%% of %d cores", "%.0f%% (코어 %d개)"),
                                          CLAMP(busy, 0.0, 1.0) * 100.0, u->ncpu);
                row_set_value(u->cpu, t);
                g_free(t);
            }
            u->idle0 = idle;
            u->total0 = total;
        }
        g_free(stat);
    }
    char *mi = lp_slurp("/proc/meminfo");
    if (mi) {
        guint64 tot = 0, avail = 0;
        const char *a = strstr(mi, "MemTotal:"), *b = strstr(mi, "MemAvailable:");
        if (a) sscanf(a, "MemTotal: %" G_GUINT64_FORMAT, &tot);
        if (b) sscanf(b, "MemAvailable: %" G_GUINT64_FORMAT, &avail);
        if (tot) {
            char *used = lp_human((tot - avail) * 1024), *all = lp_human(tot * 1024);
            char *t = g_strdup_printf(T("%s of %s (%.0f%%)", "%s / %s (%.0f%%)"), used, all,
                                      100.0 * (double)(tot - avail) / (double)tot);
            row_set_value(u->mem, t);
            g_free(t); g_free(used); g_free(all);
        }
        g_free(mi);
    }
    struct statvfs sv;
    if (statvfs("/", &sv) == 0 && sv.f_blocks) {
        guint64 all = (guint64)sv.f_blocks * sv.f_frsize;
        guint64 freeb = (guint64)sv.f_bavail * sv.f_frsize;
        char *used = lp_human(all - freeb), *tot = lp_human(all);
        char *t = g_strdup_printf(T("%s of %s (%.0f%%)", "%s / %s (%.0f%%)"), used, tot,
                                  100.0 * (double)(all - freeb) / (double)all);
        row_set_value(u->disk, t);
        g_free(t); g_free(used); g_free(tot);
    }
    char *up = lp_slurp("/proc/uptime");
    if (up) {
        long s = atol(up);
        long d = s / 86400, h = (s % 86400) / 3600, m = (s % 3600) / 60;
        char *t = d ? g_strdup_printf(T("%ld days, %ld h %ld min", "%ld일 %ld시간 %ld분"), d, h, m)
                    : g_strdup_printf(T("%ld h %ld min", "%ld시간 %ld분"), h, m);
        row_set_value(u->up, t);
        g_free(t);
        g_free(up);
    }
    return G_SOURCE_CONTINUE;
}

static void on_info(GtkWidget *row, gpointer p)
{
    (void)row; (void)p;
    lp_dialog_t *d = lp_dialog_new(T("About this machine", "이 기기에 대해"), NULL, FALSE, NULL, NULL);
    GtkWidget *l = gtk_label_new(T("Reading…", "읽는 중…"));
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_widget_add_css_class(l, "lp-mono");
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_widget_set_size_request(sw, 640, lp_dialog_fit(460, 290));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), l);
    gtk_box_append(GTK_BOX(lp_dialog_body(d)), sw);
    GtkWidget *b = gtk_button_new_with_label(T("Copy", "복사"));
    gtk_widget_set_halign(b, GTK_ALIGN_START);
    g_object_set_data(G_OBJECT(b), "lp-text", l);
    g_signal_connect(b, "clicked", G_CALLBACK(on_copy), NULL);
    gtk_box_append(GTK_BOX(lp_dialog_body(d)), b);
    static const char *const v[] = { "info", NULL };
    lp_run_async(v, NULL, l, info_done, l);
    lp_dialog_present(d);
}

/* ── recovery ───────────────────────────────────────────────────────── */

static void recovery_done(int st, const char *out, const char *err, gpointer p)
{
    (void)p;
    if (st == 0) {
        lp_toast(FALSE, T("Restarting into Recovery…", "복구 모드로 다시 시작합니다…"));
    } else if (st != -2) {
        char *why = lp_first_line(err, out);
        lp_toast(TRUE, T("Could not restart into Recovery: %s", "복구 모드로 다시 시작하지 못했습니다: %s"), why);
        g_free(why);
    }
}

static void recovery_ok(lp_dialog_t *d, gpointer p)
{
    (void)p;
    lp_dialog_close(d);
    static const char *const v[] = { "lp-reboot-recovery", NULL };
    lp_admin_run(v, NULL, T("Restarting into Recovery needs an administrator.",
                            "복구 모드로 다시 시작하려면 관리자 권한이 필요합니다."),
                 NULL, recovery_done, NULL);
}

static void on_recovery(GtkButton *b, gpointer p)
{
    (void)b; (void)p;
    lp_dialog_t *d = lp_dialog_new(T("Restart into Recovery?", "복구 모드로 다시 시작할까요?"),
                                   T("Restart", "다시 시작"), TRUE, recovery_ok, NULL);
    lp_dialog_text(d, T("The machine restarts now, once, into the recovery system. There you can check "
                        "and repair the disks, reinstall LP (keeping your files, or not), or open an "
                        "administrator shell. Save your work first.",
                        "기기가 지금 한 번 복구 시스템으로 다시 시작합니다. 그곳에서 디스크를 검사하고 "
                        "고치거나, LP 를 다시 설치하거나(파일을 남기거나 지우거나), 관리자 셸을 열 수 "
                        "있습니다. 먼저 작업을 저장하십시오."), NULL);
    lp_dialog_present(d);
}

/* The build: lp-base's version from dpkg's own record ("1.409"). The
 * name above says "linux-LP 1.0" on every build, so this is the line
 * that tells two of them apart - which one a machine is running is the
 * first question when something on it does not work. */
static char *build_words(void)
{
    char *st = lp_slurp("/var/lib/dpkg/status");
    if (!st)
        return NULL;
    char *ver = NULL;
    char *p = strstr(st, "Package: lp-base\n");
    if (p) {
        char *end = strstr(p, "\n\n");
        char *v = strstr(p, "\nVersion: ");
        if (v && (!end || v < end)) {
            v += 10;
            ver = g_strndup(v, strcspn(v, "\n"));
        }
    }
    g_free(st);
    return ver;
}

/* ── building ───────────────────────────────────────────────────────── */

static GtkWidget *build(void)
{
    char *os = g_get_os_info(G_OS_INFO_KEY_PRETTY_NAME);
    if (!os) os = g_strdup("linux-LP");
    struct utsname u;
    uname(&u);
    char *host = lp_slurp("/etc/hostname");
    if (host) g_strstrip(host);
    const char *hn = host && *host ? host : u.nodename;

    GtkWidget *page = page_new(T("System", "시스템"), NULL);

    /* The logo and the name, centred, as the first thing on the page. */
    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(hero, 12);
    GtkWidget *logo = gtk_image_new_from_icon_name("distributor-logo-lp");
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 96);
    gtk_box_append(GTK_BOX(hero), logo);
    GtkWidget *name = gtk_label_new(os);
    gtk_widget_add_css_class(name, "lp-big");
    gtk_box_append(GTK_BOX(hero), name);
    gtk_box_append(GTK_BOX(page), hero);

    GtkWidget *g = group_new(page, T("This machine", "이 기기"));
    GtkWidget *nr = row_button(g, T("Device name", "기기 이름"), hn, T("Rename…", "바꾸기…"),
                               G_CALLBACK(on_rename), (gpointer)g_intern_string(hn));
    if (!lp_is_admin()) row_lock(nr);
    char *cpu = cpu_words(), *mem = mem_words(), *gpu = gpu_words();
    if (cpu) row_value(g, T("Processor", "프로세서"), NULL, cpu);
    if (mem) row_value(g, T("Memory", "메모리"), NULL, mem);
    if (gpu) row_value(g, T("Graphics", "그래픽"), NULL, gpu);
    struct statvfs sv;
    if (statvfs("/", &sv) == 0) {
        char *d = lp_human((guint64)sv.f_blocks * sv.f_frsize);
        row_value(g, T("System disk", "시스템 디스크"), NULL, d);
        g_free(d);
    }
    char *bld = build_words();
    if (bld) row_value(g, T("Build", "빌드"), NULL, bld);
    g_free(bld);
    g_free(cpu); g_free(mem); g_free(gpu);

    /* Live: what the machine is doing now, every two seconds while the
     * page is on screen (the timer stops when the page is destroyed). */
    GtkWidget *ug = group_new(page, T("Usage", "사용량"));
    Usage *us = g_new0(Usage, 1);
    us->cpu = row_value(ug, T("Processor use", "CPU 사용률"), NULL, "…");
    us->mem = row_value(ug, T("Memory use", "메모리 사용량"), NULL, "…");
    us->disk = row_value(ug, T("Disk use", "디스크 사용량"), NULL, "…");
    us->up = row_value(ug, T("Up for", "가동 시간"), NULL, "…");
    us->ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    usage_tick(us);
    us->timer = g_timeout_add_seconds(2, usage_tick, us);
    g_object_set_data_full(G_OBJECT(page), "lp-usage", us, usage_free);

    GtkWidget *sg = group_new(page, T("Software", "소프트웨어"));
    row_value(sg, T("Operating system", "운영체제"), NULL, os);
    char *k = g_strdup_printf("%s %s", u.sysname, u.release);
    row_value(sg, T("Kernel", "커널"), NULL, k);
    g_free(k);
    row_value(sg, T("Architecture", "아키텍처"), NULL, u.machine);
    row_value(sg, T("Windowing", "화면 서버"), NULL,
              g_getenv("WAYLAND_DISPLAY") ? (lp_sway() ? "Wayland (sway)" : "Wayland (wayfire)") : "—");
    row_chevron(sg, T("Everything about this machine", "이 기기의 모든 정보"),
                T("The full report from info, to copy into a question", "info 의 전체 보고서, 질문에 붙여 넣을 수 있게"),
                NULL, G_CALLBACK(on_info), NULL);

    GtkWidget *rg = group_new(page, T("Recovery", "복구"));
    if (lp_have("lp-reboot-recovery")) {
        GtkWidget *r = row_button(rg, T("Restart into Recovery", "복구 모드로 다시 시작"),
                                  T("Repair the disks, reinstall LP, or open an administrator shell",
                                    "디스크 고치기, LP 다시 설치, 관리자 셸"),
                                  T("Restart…", "다시 시작…"), G_CALLBACK(on_recovery), NULL);
        if (!lp_is_admin()) row_lock(r);
    } else {
        row_value(rg, T("Restart into Recovery", "복구 모드로 다시 시작"),
                  T("Not available: lp-reboot-recovery is not installed on this system",
                    "쓸 수 없음: 이 시스템에 lp-reboot-recovery 가 설치되어 있지 않습니다"), NULL);
    }
    char *sub = g_strdup_printf("%s · %s", hn, os);
    page_set_subtitle(page, sub);
    g_free(sub);
    g_free(os); g_free(host);
    return page;
}

static const char *const KEYS[] = {
    "Device name", "기기 이름",
    "Host name", "호스트 이름",
    "Processor", "프로세서",
    "Memory", "메모리",
    "Graphics", "그래픽",
    "Operating system", "운영체제",
    "Kernel", "커널",
    "About", "정보",
    "Restart into Recovery", "복구 모드로 다시 시작",
    "Everything about this machine", "이 기기의 모든 정보",
    NULL
};

const lp_panel_t lp_panel_system = {
    "system", "System", "시스템", "computer-symbolic", build, KEYS, NULL
};
