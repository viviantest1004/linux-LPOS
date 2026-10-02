/* lp-csd.c - every GTK 3 window draws its own title bar.
 *
 * Preloaded into the session (session-run sets LD_PRELOAD). GTK 3 on
 * Wayland decides between drawing its own title bar - the minimise,
 * maximise and close buttons of the LP theme - and asking the
 * compositor for one by a single test: does the compositor offer the
 * KDE server-decoration protocol with "server" as its default? sway
 * does, and GTK_CSD=1 does not change GTK's mind on Wayland. So a GTK 3
 * window without a header bar (LibreOffice, Geany, GParted) got sway's
 * title bar, which has no buttons at all, beside apps with the LP
 * buttons.
 *
 * This hides that one protocol from the client: every wl_registry the
 * process makes gets a listener that passes each global through except
 * org_kde_kwin_server_decoration_manager. Nothing else is touched.
 * setuid programs ignore an LD_PRELOAD path with a slash in it, quietly.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <wayland-client-core.h>

struct registry_listener {
    void (*global)(void *data, struct wl_proxy *registry, uint32_t name,
                   const char *interface, uint32_t version);
    void (*global_remove)(void *data, struct wl_proxy *registry, uint32_t name);
};

#define MAX_REGISTRIES 32
static struct {
    struct wl_proxy *proxy;
    const struct registry_listener *orig;
} regs[MAX_REGISTRIES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static const struct registry_listener *orig_for(struct wl_proxy *p)
{
    const struct registry_listener *o = NULL;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_REGISTRIES; i++)
        if (regs[i].proxy == p) {
            o = regs[i].orig;
            break;
        }
    pthread_mutex_unlock(&lock);
    return o;
}

static void on_global(void *data, struct wl_proxy *reg, uint32_t name,
                      const char *interface, uint32_t version)
{
    if (interface && strcmp(interface, "org_kde_kwin_server_decoration_manager") == 0)
        return;
    const struct registry_listener *o = orig_for(reg);
    if (o && o->global)
        o->global(data, reg, name, interface, version);
}

static void on_global_remove(void *data, struct wl_proxy *reg, uint32_t name)
{
    const struct registry_listener *o = orig_for(reg);
    if (o && o->global_remove)
        o->global_remove(data, reg, name);
}

static const struct registry_listener ours = { on_global, on_global_remove };

int wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data)
{
    static int (*real)(struct wl_proxy *, void (**)(void), void *);
    if (!real)
        real = (int (*)(struct wl_proxy *, void (**)(void), void *))
            dlsym(RTLD_NEXT, "wl_proxy_add_listener");
    const char *cls = wl_proxy_get_class(proxy);
    if (cls && strcmp(cls, "wl_registry") == 0) {
        int slot = -1;
        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_REGISTRIES; i++)
            if (!regs[i].proxy || regs[i].proxy == proxy) {
                slot = i;
                break;
            }
        if (slot >= 0) {
            regs[slot].proxy = proxy;
            regs[slot].orig = (const struct registry_listener *)implementation;
        }
        pthread_mutex_unlock(&lock);
        if (slot >= 0)
            return real(proxy, (void (**)(void))&ours, data);
    }
    return real(proxy, implementation, data);
}
