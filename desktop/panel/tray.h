/*
 * tray.h - what is running without a window, in the top bar.
 *
 * One small button per program that is still running but has no
 * window: tray icons (StatusNotifierItem, the D-Bus standard KDE,
 * AppIndicator, Qt, Electron and Chromium speak) and the applications
 * that do not speak it and simply stayed up after their last window
 * closed. Each one can be opened again or quit from its button.
 * tray.c says how, and why it is done this way.
 */
#ifndef LP_TRAY_H
#define LP_TRAY_H

#include <gtk/gtk.h>

/* Once, in main, after lp_shell_init: takes the tray watcher's name on
 * the session bus (or registers with the one already there) and starts
 * looking for windowless applications every few seconds. */
void       lp_tray_init(void);

/* A horizontal box of tray buttons, one per bar. Every box shows the
 * same items; a destroyed box is forgotten. It hides itself while it
 * is empty, so the bar gives it no space - gtk_widget_show_all on the
 * bar leaves that to it. */
GtkWidget *lp_tray_new(void);

/* The window list changed; call it from the lp_toplevels_watch
 * callback. Windowless applications are looked for again a second
 * later. Until this has been called once they are not looked for at
 * all: an empty window list would read as every application running
 * in the background. */
void       lp_tray_toplevels_changed(void);

#endif /* LP_TRAY_H */
