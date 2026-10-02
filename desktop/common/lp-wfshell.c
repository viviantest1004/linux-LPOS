/*
 * lp-wfshell.c - the client side of wayfire-shell-unstable-v2
 * (lp-wfshell.h says why). Shares GDK's Wayland connection, so its events
 * arrive on the GTK main loop.
 */
#include "lp-wfshell.h"

#include <gdk/gdkwayland.h>
#include <string.h>

#include "wayfire-shell-unstable-v2-client-protocol.h"

static struct zwf_shell_manager_v2 *manager;

typedef struct {
    GdkMonitor *mon;
    LpFullscreenFn fs_fn;
    LpHotspotFn hs_fn;
    gpointer data;
} Watch;

static void r_global(void *d, struct wl_registry *r, uint32_t name,
                     const char *iface, uint32_t version)
{
    (void)d; (void)version;
    if (strcmp(iface, zwf_shell_manager_v2_interface.name) == 0)
        manager = wl_registry_bind(r, name, &zwf_shell_manager_v2_interface, 1);
}

static void r_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void)d; (void)r; (void)name; }

static const struct wl_registry_listener registry_listener = {
    .global = r_global,
    .global_remove = r_remove,
};

gboolean lp_wfshell_init(void)
{
    static gboolean done;
    if (done)
        return manager != NULL;
    done = TRUE;
    GdkDisplay *gd = gdk_display_get_default();
    if (!GDK_IS_WAYLAND_DISPLAY(gd))
        return FALSE;
    struct wl_display *wd = gdk_wayland_display_get_wl_display(gd);
    struct wl_registry *reg = wl_display_get_registry(wd);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(wd);
    return manager != NULL;
}

static struct zwf_output_v2 *wf_output(GdkMonitor *mon)
{
    if (!manager || !mon)
        return NULL;
    struct wl_output *o = gdk_wayland_monitor_get_wl_output(mon);
    return o ? zwf_shell_manager_v2_get_wf_output(manager, o) : NULL;
}

static void fs_enter(void *d, struct zwf_output_v2 *o)
{
    (void)o;
    Watch *w = d;
    w->fs_fn(w->mon, TRUE, w->data);
}

static void fs_leave(void *d, struct zwf_output_v2 *o)
{
    (void)o;
    Watch *w = d;
    w->fs_fn(w->mon, FALSE, w->data);
}


static const struct zwf_output_v2_listener output_listener = {
    .enter_fullscreen = fs_enter,
    .leave_fullscreen = fs_leave,
};

void lp_wfshell_watch_fullscreen(GdkMonitor *mon, LpFullscreenFn fn, gpointer data)
{
    struct zwf_output_v2 *o = wf_output(mon);
    if (!o)
        return;
    Watch *w = g_new0(Watch, 1);
    w->mon = mon;
    w->fs_fn = fn;
    w->data = data;
    zwf_output_v2_add_listener(o, &output_listener, w);
}

static void hs_enter(void *d, struct zwf_hotspot_v2 *h)
{
    (void)h;
    Watch *w = d;
    w->hs_fn(w->mon, TRUE, w->data);
}

static void hs_leave(void *d, struct zwf_hotspot_v2 *h)
{
    (void)h;
    Watch *w = d;
    w->hs_fn(w->mon, FALSE, w->data);
}

static const struct zwf_hotspot_v2_listener hotspot_listener = {
    .enter = hs_enter,
    .leave = hs_leave,
};

void lp_wfshell_hotspot(GdkMonitor *mon, guint edges, guint threshold, guint timeout_ms,
                        LpHotspotFn fn, gpointer data)
{
    struct zwf_output_v2 *o = wf_output(mon);
    if (!o)
        return;
    struct zwf_hotspot_v2 *h = zwf_output_v2_create_hotspot(o, edges, threshold, timeout_ms);
    Watch *w = g_new0(Watch, 1);
    w->mon = mon;
    w->hs_fn = fn;
    w->data = data;
    zwf_hotspot_v2_add_listener(h, &hotspot_listener, w);
}
