/*
 * lp-apps.h - from a window's app_id to the application it belongs to.
 *
 * The compositor names a window by its app_id; the dock and the app grid
 * know applications by their .desktop files. The two agree only by
 * convention, and the convention is broken often enough to need a
 * lookup that tries several things: foot says "foot" and ships
 * foot.desktop, gedit says "org.gnome.gedit", Firefox ESR says
 * "firefox-esr", and our own GTK 4 applications say "org.lpzero.Files"
 * from lp-files.desktop, which is what StartupWMClass is for.
 */
#ifndef LP_APPS_H
#define LP_APPS_H

#include <gio/gdesktopappinfo.h>

/* A new reference, or NULL when no .desktop file claims this app_id. */
GDesktopAppInfo *lp_app_for_id(const char *app_id);

/* TRUE when a window with this app_id belongs to info. */
gboolean lp_app_owns(GDesktopAppInfo *info, const char *app_id);

/* Launch it, detached, with the display's launch context. */
void lp_app_launch(GAppInfo *info);

/* The application's own name in the current language, else its id. */
const char *lp_app_name(GAppInfo *info);

#endif /* LP_APPS_H */
