/*
 * panels.c - the sidebar, in order.
 *
 * The one place a panel is added. Each panel lives in panels/<id>.c and
 * exports one lp_panel_t; this list decides the order they appear in and
 * nothing else. The order is spec 5's (apps-and-settings.md), with the
 * panels people open most often - network, Bluetooth, display, sound,
 * power - first, and Reset last, because the one screen that can undo
 * everything should be reached on purpose and not by the thumb landing
 * one row low.
 */
#include "core.h"

extern const lp_panel_t lp_panel_network, lp_panel_bluetooth,
    lp_panel_display, lp_panel_sound, lp_panel_power, lp_panel_notify,
    lp_panel_appearance, lp_panel_access, lp_panel_touch, lp_panel_keyboard,
    lp_panel_users, lp_panel_apps, lp_panel_storage, lp_panel_datetime,
    lp_panel_region, lp_panel_updates, lp_panel_system, lp_panel_reset;

const lp_panel_t *const lp_panels[] = {
    &lp_panel_network,
    &lp_panel_bluetooth,
    &lp_panel_display,
    &lp_panel_sound,
    &lp_panel_power,
    &lp_panel_notify,
    &lp_panel_appearance,
    &lp_panel_access,
    &lp_panel_touch,
    &lp_panel_keyboard,
    &lp_panel_region,
    &lp_panel_datetime,
    &lp_panel_users,
    &lp_panel_apps,
    &lp_panel_storage,
    &lp_panel_updates,
    &lp_panel_system,
    &lp_panel_reset,
    NULL
};
