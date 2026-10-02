/*
 * osk.h - what the parts of lp-osk say to each other.
 *
 * lp-osk is four files around one piece of state:
 *
 *   lp-osk.c   the command line, the single-instance socket, the settings
 *              file, and the policy of when the keyboard shows itself
 *   ui.c       the layer-shell window, the keys, and every touch
 *   type.c     where text goes: the input method (live text or preedit),
 *              the virtual keyboards, and the laptop keyboard's grab
 *   evdev.c    whether the last thing a person touched was the screen
 *   hangul.c   the 2-beolsik automaton (no GTK; hangul-test.c drives it)
 *
 * Nothing here is thread-shared; everything runs on the GTK main loop.
 */
#ifndef LP_OSK_H
#define LP_OSK_H

#include <gtk/gtk.h>
#include <stdint.h>

#include "hangul.h"

enum { LANG_EN = 0, LANG_KO = 1 };
enum { AUTO_TOUCH = 0, AUTO_ALWAYS = 1, AUTO_OFF = 2 };
enum { INPUT_UNKNOWN = 0, INPUT_TOUCH, INPUT_KEY, INPUT_POINTER };

/* The layouts a person has chosen (cfg.layouts). English cannot be
 * turned off: it is the machine's default and the password layout. */
#define LAYOUT_EN 1u
#define LAYOUT_KO 2u

/* The keys that switch 한/영 besides the Hangul key itself, which always
 * does (osk.ini switch-keys; Settings > Keyboard turns each on or off). */
#define SWITCH_RALT       1u   /* Right Alt tapped on its own */
#define SWITCH_SHIFTSPACE 2u
#define SWITCH_CTRLSPACE  4u

/* Modifier bits the keys hand to type.c. type.c turns them into whatever
 * mask the keymap in use gives those modifiers. */
#define MOD_SHIFT 1u
#define MOD_CTRL  4u
#define MOD_ALT   8u

typedef struct {
    /* kept in ~/.config/lp/osk.ini */
    double  height_frac;   /* of the monitor's logical height */
    int     auto_mode;     /* AUTO_* */
    int     lang;          /* LANG_*, shared by screen and laptop keyboard */
    unsigned layouts;      /* LAYOUT_* */
    gboolean ime;          /* compose Hangul from the laptop keyboard too */
    unsigned switch_keys;  /* SWITCH_* */
} OskConfig;

extern OskConfig cfg;

/* lp-osk.c */
void osk_config_save(void);
void osk_state_changed(void);          /* tell `lp-osk watch` listeners */
void osk_im_activated(gboolean fresh); /* a text field asked for input */
void osk_im_deactivated(void);
void osk_dismissed(void);              /* the person closed it */
void osk_hardware_key(void);           /* a real keyboard was typed on */

/* ui.c */
void     ui_init(void);                /* geometry only; no surface yet */
void     ui_show(void);
void     ui_hide(void);
gboolean ui_visible(void);
void     ui_relabel(void);             /* language or field kind changed */
void     ui_field_purpose(uint32_t purpose);
/* Test hook: drive the touch handler with a synthesized touch event.
 * phase 0 down, 1 move (dx, dy from the down point), 2 up. */
gboolean ui_test_touch(int id, int phase, const char *key, double dx,
                       double dy, char *err, size_t errlen);
void     ui_describe(GString *out);    /* key geometry, for tests */
const char *ui_page_name(void);        /* "letters" or "symbols" */

/* type.c */
gboolean type_init(void);
void     type_shutdown(void);
int      type_lang(void);              /* cfg.lang, but English in passwords */
void     type_set_lang(int lang);
void     type_toggle_lang(void);       /* 한/영, among cfg.layouts */
uint32_t type_ko_jamo(char letter, gboolean shift);
void     type_jamo(uint32_t jamo);
void     type_backspace(uint32_t mods);
void     type_text(const char *utf8);
void     type_key(uint32_t evdev_code, uint32_t mods);
/* An ASCII character as the key a US keyboard types it with (Shift
 * added when the character needs it). FALSE: no such key. */
gboolean type_ascii(char c, uint32_t mods);
void     type_flush(void);
void     type_refresh_keymap(void);
gboolean type_composing(void);
const char *type_im_status(void);      /* "active", "inactive", ... */
uint32_t type_purpose(void);
/* Presses, by CLOCK_MONOTONIC ms: any mouse/touchpad button or touch
 * (evdev.c), and those on our own keys (ui.c). A live syllable ends at a
 * press that was not ours (type.c, head comment 1.). */
void     type_pointer_press(uint32_t ms);
void     type_osk_press(uint32_t ms);

/* evdev.c */
void        lastinput_init(void);
int         lastinput_query(gint64 *age_ms);
const char *lastinput_status(void);
void        lastinput_force(int kind);  /* tests: pretend this came last */

#endif
