/*
 * lp-toplevel.c - the client side of wlr-foreign-toplevel-management.
 *
 * The protocol sends a window's properties one event at a time - title,
 * app_id, state - and then `done`. Nothing is reported to the callers
 * until `done`, because a window whose app_id has arrived and whose
 * state has not is a window drawn as "not running" for one frame and
 * then corrected, which on a dock is a dot that blinks.
 *
 * Changes are coalesced into one idle callback per main-loop turn: a
 * burst of twenty events when a browser opens is one redraw, not twenty.
 *
 * The shell builds this against GTK 3 and Task Manager (desktop/tasks)
 * against GTK 4. Every GDK call below exists under the same name in
 * both; only the header moved, and GTK 3 has no gdk/wayland/gdkwayland.h.
 */
#include "lp-toplevel.h"

#if __has_include(<gdk/wayland/gdkwayland.h>)
#include <gdk/wayland/gdkwayland.h>
#else
#include <gdk/gdkwayland.h>
#endif
#include <string.h>

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

typedef struct {
    LpToplevelsFn fn;
    gpointer data;
} Watch;

static struct zwlr_foreign_toplevel_manager_v1 *manager;
static struct wl_seat *seat;
static GList *toplevels;
static GList *watches;
static guint notify_id;

static gboolean notify_now(gpointer d)
{
    (void)d;
    notify_id = 0;
    for (GList *l = watches; l; l = l->next) {
        Watch *w = l->data;
        w->fn(w->data);
    }
    return G_SOURCE_REMOVE;
}

static void notify(void)
{
    if (!notify_id)
        notify_id = g_idle_add(notify_now, NULL);
}

static void h_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                    const char *title)
{
    (void)h;
    LpToplevel *t = d;
    g_free(t->title);
    t->title = g_strdup(title);
}

static void h_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                     const char *id)
{
    (void)h;
    LpToplevel *t = d;
    g_free(t->app_id);
    t->app_id = g_strdup(id);
}

static void h_output_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                           struct wl_output *o)
{ (void)d; (void)h; (void)o; }

static void h_output_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                           struct wl_output *o)
{ (void)d; (void)h; (void)o; }

static void h_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                    struct wl_array *state)
{
    (void)h;
    LpToplevel *t = d;
    t->activated = t->minimized = t->maximized = t->fullscreen = FALSE;
    uint32_t *s;
    wl_array_for_each(s, state) {
        switch (*s) {
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:  t->activated = TRUE; break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:  t->minimized = TRUE; break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:  t->maximized = TRUE; break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN: t->fullscreen = TRUE; break;
        }
    }
}

static void h_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)h;
    LpToplevel *t = d;
    t->done = TRUE;
    notify();
}

static void h_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h)
{
    LpToplevel *t = d;
    toplevels = g_list_remove(toplevels, t);
    zwlr_foreign_toplevel_handle_v1_destroy(h);
    g_free(t->app_id);
    g_free(t->title);
    g_free(t);
    notify();
}

static void h_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
                     struct zwlr_foreign_toplevel_handle_v1 *p)
{ (void)d; (void)h; (void)p; }

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    .title = h_title,
    .app_id = h_app_id,
    .output_enter = h_output_enter,
    .output_leave = h_output_leave,
    .state = h_state,
    .done = h_done,
    .closed = h_closed,
    .parent = h_parent,
};

static void m_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m,
                       struct zwlr_foreign_toplevel_handle_v1 *h)
{
    (void)d; (void)m;
    LpToplevel *t = g_new0(LpToplevel, 1);
    t->handle = h;
    toplevels = g_list_append(toplevels, t);
    zwlr_foreign_toplevel_handle_v1_add_listener(h, &handle_listener, t);
}

static void m_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m)
{
    (void)d;
    zwlr_foreign_toplevel_manager_v1_destroy(m);
    manager = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener manager_listener = {
    .toplevel = m_toplevel,
    .finished = m_finished,
};

static void r_global(void *d, struct wl_registry *r, uint32_t name,
                     const char *iface, uint32_t version)
{
    (void)d;
    if (strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
        /* Version 3 adds `parent`, which the listener above answers;
         * a compositor offering less is asked for what it has. */
        uint32_t v = version < 3 ? version : 3;
        manager = wl_registry_bind(r, name,
                                   &zwlr_foreign_toplevel_manager_v1_interface, v);
        zwlr_foreign_toplevel_manager_v1_add_listener(manager,
                                                      &manager_listener, NULL);
    }
}

static void r_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void)d; (void)r; (void)name; }

static const struct wl_registry_listener registry_listener = {
    .global = r_global,
    .global_remove = r_remove,
};

gboolean lp_toplevels_init(void)
{
    GdkDisplay *gd = gdk_display_get_default();
    if (!GDK_IS_WAYLAND_DISPLAY(gd))
        return FALSE;
    struct wl_display *wd = gdk_wayland_display_get_wl_display(gd);
    struct wl_registry *reg = wl_display_get_registry(wd);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    /* One round trip binds the manager, the second delivers the windows
     * that already exist, so the first draw is already right. */
    wl_display_roundtrip(wd);
    if (manager)
        wl_display_roundtrip(wd);
    GdkSeat *gs = gdk_display_get_default_seat(gd);
    if (gs)
        seat = gdk_wayland_seat_get_wl_seat(gs);
    return manager != NULL;
}

void lp_toplevels_watch(LpToplevelsFn fn, gpointer data)
{
    Watch *w = g_new0(Watch, 1);
    w->fn = fn;
    w->data = data;
    watches = g_list_append(watches, w);
}

GList *lp_toplevels(void)
{
    return toplevels;
}

void lp_toplevel_activate(LpToplevel *t)
{
    if (!t || !seat)
        return;
    /* A minimised window has to be brought back first: activate alone
     * gives it focus and leaves it invisible, which on a touch screen
     * looks exactly like the tap did nothing. */
    if (t->minimized)
        zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
    zwlr_foreign_toplevel_handle_v1_activate(t->handle, seat);
}

void lp_toplevel_close(LpToplevel *t)
{
    if (t)
        zwlr_foreign_toplevel_handle_v1_close(t->handle);
}

void lp_toplevel_set_minimized(LpToplevel *t, gboolean min)
{
    if (!t)
        return;
    if (min)
        zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
    else
        zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
}
