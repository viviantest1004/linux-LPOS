/* g_spawn, statvfs 등이 -std=c11 만으로는 선언이 숨는다. */
#define _DEFAULT_SOURCE 1

/*
 * shot.c - 스크린샷.
 *
 * apps-and-settings.md 3-1 은 스크린샷을 "셸에 내장, 별도 앱 불필요"
 * 라고 적어 두었고, 그 내장분은 이미 있다 - Print, Shift+Print,
 * Alt+Print 가 grim 을 불러 클립보드에 넣는다. 저장소 주인이 그것과
 * 별개로 창이 있는 앱을 요구했고, 이 파일이 그것이다.
 *
 * 둘은 겹치지 않는다. 단축키는 "지금 이 화면을 그대로" 이고, 이 앱은
 * 그것 말고 필요한 것들을 맡는다:
 *
 *   지연     메뉴를 펼쳐 놓고 찍으려면 몇 초가 필요하다. 단축키로는
 *            찍는 순간 메뉴가 닫힌다.
 *   미리보기 찍은 것이 원하던 것인지 보고 나서 저장할지 정한다.
 *   이어서   저장한 다음 그림판으로 열어 화살표를 그리는 것이,
 *           스크린샷을 찍는 사람이 실제로 하는 일의 대부분이다.
 *
 * ── 창을 숨기는 문제 ──
 *
 * 전체 화면을 찍을 때 이 창이 화면에 있으면 그것도 같이 찍힌다. 그래서
 * 찍기 전에 창을 감추는데, 감추는 것과 컴포지터가 다시 그리는 것 사이에
 * 시간이 필요하다 - 바로 grim 을 부르면 방금 사라진 창이 그대로
 * 들어간다. 400ms 를 기다린다.
 *
 * 창을 감추면 포커스는 그 전에 있던 창으로 돌아간다. '창' 모드가
 * 공짜로 얻는 것이 그것이다: 감춘 뒤에 컴포지터에게 지금 포커스된
 * 창을 물으면, 사용자가 찍고 싶었던 바로 그 창이 나온다.
 */

#include <gtk/gtk.h>
#include "lp-i18n.h"
#include "lp-fit.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* mode_t 라고 부르면 sys/types.h 의 것과 부딪힌다. 파일 권한을
 * 담는 그 타입과 이것은 아무 상관이 없다. */
typedef enum { SHOT_SCREEN, SHOT_REGION, SHOT_WINDOW } shot_mode_t;

typedef struct {
    GtkWidget *win;
    GtkWidget *preview;      /* GtkPicture */
    GtkWidget *status;
    GtkWidget *actions;      /* 저장한 뒤에만 보인다 */
    GtkWidget *delay;        /* GtkDropDown: 없음 / 3초 / 5초 */
    char      *last;         /* 마지막으로 저장한 파일 */
    shot_mode_t mode;
} app_t;

/* ── 도구 ──────────────────────────────────────────────────────── */

static char *run_out(const char *const *argv)
{
    char *out = NULL;
    int   status = 0;
    if (!g_spawn_sync(NULL, (char **)argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, &out, NULL, &status, NULL)) {
        g_free(out);
        return NULL;
    }
    if (!g_spawn_check_wait_status(status, NULL)) {
        g_free(out);
        return NULL;
    }
    return out;
}

static gboolean run_quiet(const char *const *argv)
{
    int status = 0;
    if (!g_spawn_sync(NULL, (char **)argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                      G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL, &status, NULL))
        return FALSE;
    return g_spawn_check_wait_status(status, NULL);
}

/* 저장할 자리. 사진 폴더가 어디인지는 XDG 가 답한다 - 이름이
 * Pictures 든 무엇이든. */
static char *shot_path(void)
{
    const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    char *base = dir ? g_strdup(dir)
                     : g_build_filename(g_get_home_dir(), "Pictures", NULL);
    g_mkdir_with_parents(base, 0755);

    /* 이름은 찍은 시각이다. 파일이 늘어나도 순서가 이름으로 남고,
     * 같은 초에 두 번 찍는 일은 사람 손으로는 일어나지 않는다. */
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", &tmv);

    char *name = g_strdup_printf("%s %s.png",
                                 T("Screenshot", "스크린샷"), stamp);
    char *full = g_build_filename(base, name, NULL);
    g_free(name); g_free(base);
    return full;
}

/* 컴포지터에게 지금 포커스된 창의 자리를 묻는다. */
static char *focused_geometry(void)
{
    const char *argv[] = { "swaymsg", "-t", "get_tree", "-r", NULL };
    char *json = run_out(argv);
    if (!json) return NULL;

    /* "focused":true 를 찾고, 그 앞쪽에서 가장 가까운 "rect" 를 읽는다.
     * sway 의 출력에서 rect 는 항상 focused 보다 앞에 온다. */
    char *f = strstr(json, "\"focused\": true");
    if (!f) f = strstr(json, "\"focused\":true");

    char *geo = NULL;
    if (f) {
        char *r = NULL, *p = json;
        while (p < f) {
            char *q = strstr(p, "\"rect\"");
            if (!q || q >= f) break;
            r = q;
            p = q + 6;
        }
        if (r) {
            int x = 0, y = 0, w = 0, h = 0;
            char *xp = strstr(r, "\"x\"");
            char *yp = strstr(r, "\"y\"");
            char *wp = strstr(r, "\"width\"");
            char *hp = strstr(r, "\"height\"");
            if (xp) sscanf(xp, "\"x\": %d", &x);
            if (yp) sscanf(yp, "\"y\": %d", &y);
            if (wp) sscanf(wp, "\"width\": %d", &w);
            if (hp) sscanf(hp, "\"height\": %d", &h);
            if (w > 0 && h > 0)
                geo = g_strdup_printf("%d,%d %dx%d", x, y, w, h);
        }
    }
    g_free(json);
    return geo;
}

static void say(app_t *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->status), text);
}

/* ── 찍기 ──────────────────────────────────────────────────────── */

static gboolean do_capture(gpointer data)
{
    app_t *app = data;
    char  *path = shot_path();
    gboolean ok = FALSE;

    if (app->mode == SHOT_REGION) {
        /* slurp 이 먼저 사각형을 받고, 그 다음 grim 이 찍는다.
         * 사용자가 Esc 를 누르면 slurp 이 아무것도 내놓지 않고,
         * 그때는 취소이지 실패가 아니다. */
        const char *sargv[] = { "slurp", NULL };
        char *geo = run_out(sargv);
        if (geo) {
            g_strchomp(geo);
            if (*geo) {
                const char *gargv[] = { "grim", "-g", geo, path, NULL };
                ok = run_quiet(gargv);
            }
            g_free(geo);
        }
        if (!ok) {
            gtk_widget_set_visible(app->win, TRUE);
            say(app, T("Cancelled.", "취소했습니다."));
            g_free(path);
            return G_SOURCE_REMOVE;
        }
    } else if (app->mode == SHOT_WINDOW) {
        char *geo = focused_geometry();
        if (geo) {
            const char *gargv[] = { "grim", "-g", geo, path, NULL };
            ok = run_quiet(gargv);
            g_free(geo);
        }
        if (!ok) {
            /* 창을 찾지 못했으면 화면 전체로 떨어진다. 아무것도 찍지
             * 않는 것보다는 낫고, 아래 문구가 그렇게 되었다고 말한다. */
            const char *gargv[] = { "grim", path, NULL };
            ok = run_quiet(gargv);
        }
    } else {
        const char *gargv[] = { "grim", path, NULL };
        ok = run_quiet(gargv);
    }

    gtk_widget_set_visible(app->win, TRUE);

    if (!ok) {
        say(app, T("Could not take the screenshot. Is grim installed?",
                   "스크린샷을 찍지 못했습니다. grim 이 있는지 확인하십시오."));
        g_free(path);
        return G_SOURCE_REMOVE;
    }

    g_free(app->last);
    app->last = path;

    gtk_picture_set_filename(GTK_PICTURE(app->preview), path);
    gtk_widget_set_visible(app->actions, TRUE);

    char *shown = g_path_get_basename(path);
    char *msg = g_strdup_printf(T("Saved as %s", "%s (으)로 저장했습니다"),
                                shown);
    say(app, msg);
    g_free(msg); g_free(shown);
    return G_SOURCE_REMOVE;
}

static void start_capture(app_t *app, shot_mode_t mode)
{
    app->mode = mode;

    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->delay));
    guint secs = (sel == 1) ? 3 : (sel == 2) ? 5 : 0;

    /* 창을 감추고, 컴포지터가 다시 그릴 시간을 준다. 이것을 건너뛰면
     * 방금 사라진 창이 사진에 그대로 남는다. */
    gtk_widget_set_visible(app->win, FALSE);
    g_timeout_add(400 + secs * 1000, do_capture, app);
}

static void on_screen(GtkButton *b, gpointer d)
{ (void)b; start_capture(d, SHOT_SCREEN); }

static void on_region(GtkButton *b, gpointer d)
{ (void)b; start_capture(d, SHOT_REGION); }

static void on_window(GtkButton *b, gpointer d)
{ (void)b; start_capture(d, SHOT_WINDOW); }

/* ── 찍은 다음 ─────────────────────────────────────────────────── */

static void on_copy(GtkButton *b, gpointer d)
{
    (void)b;
    app_t *app = d;
    if (!app->last) return;

    /* wl-copy 는 표준입력을 읽는다. 파일을 다시 열어 넘기는 것보다
     * 셸에게 리다이렉션을 시키는 편이 짧고, 이 한 줄이 셸을 거치는
     * 유일한 곳이다. */
    char *quoted = g_shell_quote(app->last);
    char *cmd = g_strdup_printf("wl-copy --type image/png < %s", quoted);
    const char *argv[] = { "/bin/sh", "-c", cmd, NULL };
    gboolean ok = run_quiet(argv);
    g_free(cmd); g_free(quoted);

    say(app, ok ? T("Copied to the clipboard.", "클립보드에 넣었습니다.")
                : T("Could not copy it.", "클립보드에 넣지 못했습니다."));
}

static void on_edit(GtkButton *b, gpointer d)
{
    (void)b;
    app_t *app = d;
    if (!app->last) return;

    /* 그림판. 없으면 이 기계가 그림에 대해 아는 다른 것으로 연다. */
    const char *drawing[] = { "drawing", app->last, NULL };
    if (g_spawn_async(NULL, (char **)drawing, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                      G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL))
        return;

    const char *open[] = { "xdg-open", app->last, NULL };
    if (!g_spawn_async(NULL, (char **)open, NULL,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                       G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL))
        say(app, T("Nothing installed can open it.",
                   "이 파일을 열 수 있는 프로그램이 없습니다."));
}

static void on_folder(GtkButton *b, gpointer d)
{
    (void)b;
    app_t *app = d;
    if (!app->last) return;
    char *dir = g_path_get_dirname(app->last);
    const char *argv[] = { "lp-files", dir, NULL };
    g_spawn_async(NULL, (char **)argv, NULL,
                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                  G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL);
    g_free(dir);
}

/* ── 창 ────────────────────────────────────────────────────────── */

static GtkWidget *big_button(const char *icon, const char *label,
                             GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(v, 12);
    gtk_widget_set_margin_bottom(v, 12);
    gtk_widget_set_margin_start(v, 18);
    gtk_widget_set_margin_end(v, 18);

    GtkWidget *im = gtk_image_new_from_icon_name(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(im), 24);
    gtk_box_append(GTK_BOX(v), im);

    GtkWidget *l = gtk_label_new(label);
    gtk_box_append(GTK_BOX(v), l);

    gtk_button_set_child(GTK_BUTTON(b), v);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

static void activate(GtkApplication *gapp, gpointer data)
{
    (void)data;
    app_t *app = g_new0(app_t, 1);

    app->win = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(app->win),
                         T("Screenshot", "스크린샷"));
    lp_fit_default_size(GTK_WINDOW(app->win), 620, 560);

    GtkWidget *head = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(app->win), head);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);

    /* 무엇을 찍을지 */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row),
        big_button("video-display-symbolic",
                   T("Whole screen", "전체 화면"),
                   G_CALLBACK(on_screen), app));
    gtk_box_append(GTK_BOX(row),
        big_button("edit-select-all-symbolic",
                   T("Select an area", "영역 선택"),
                   G_CALLBACK(on_region), app));
    gtk_box_append(GTK_BOX(row),
        big_button("window-new-symbolic",
                   T("This window", "창"),
                   G_CALLBACK(on_window), app));
    gtk_box_append(GTK_BOX(box), row);

    /* 지연 */
    GtkWidget *drow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(drow, GTK_ALIGN_CENTER);
    GtkWidget *dl = gtk_label_new(T("Delay", "지연"));
    gtk_widget_add_css_class(dl, "dim-label");
    gtk_box_append(GTK_BOX(drow), dl);

    const char *delays[] = { NULL, NULL, NULL, NULL };
    delays[0] = T("None", "없음");
    delays[1] = T("3 seconds", "3초");
    delays[2] = T("5 seconds", "5초");
    app->delay = gtk_drop_down_new_from_strings(delays);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(app->delay), 0);
    gtk_box_append(GTK_BOX(drow), app->delay);
    gtk_box_append(GTK_BOX(box), drow);

    /* 미리보기 */
    app->preview = gtk_picture_new();
    gtk_widget_set_vexpand(app->preview, TRUE);
    gtk_picture_set_can_shrink(GTK_PICTURE(app->preview), TRUE);
    gtk_widget_add_css_class(app->preview, "card");
    gtk_box_append(GTK_BOX(box), app->preview);

    app->status = gtk_label_new(
        T("The shortcuts also work: Print, Shift+Print, Alt+Print.",
          "단축키로도 찍을 수 있습니다: Print, Shift+Print, Alt+Print."));
    gtk_widget_add_css_class(app->status, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(app->status), TRUE);
    gtk_box_append(GTK_BOX(box), app->status);

    /* 찍은 다음에 할 수 있는 것. 찍기 전에는 누를 것이 없으므로
     * 회색으로 두지 않고 아예 감춘다. */
    app->actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(app->actions, GTK_ALIGN_CENTER);
    GtkWidget *b1 = gtk_button_new_with_label(T("Copy", "복사"));
    GtkWidget *b2 = gtk_button_new_with_label(T("Open in Paint", "그림판에서 열기"));
    GtkWidget *b3 = gtk_button_new_with_label(T("Show the folder", "폴더 열기"));
    g_signal_connect(b1, "clicked", G_CALLBACK(on_copy), app);
    g_signal_connect(b2, "clicked", G_CALLBACK(on_edit), app);
    g_signal_connect(b3, "clicked", G_CALLBACK(on_folder), app);
    gtk_box_append(GTK_BOX(app->actions), b1);
    gtk_box_append(GTK_BOX(app->actions), b2);
    gtk_box_append(GTK_BOX(app->actions), b3);
    gtk_widget_set_visible(app->actions, FALSE);
    gtk_box_append(GTK_BOX(box), app->actions);

    gtk_window_set_child(GTK_WINDOW(app->win), box);
    gtk_window_present(GTK_WINDOW(app->win));
}

int main(int argc, char **argv)
{
    GtkApplication *app = gtk_application_new("org.lpzero.Screenshot",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
