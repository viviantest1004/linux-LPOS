/* lp-motion.h - the springs of design/feel.md §2, for GTK 3 and GTK 4.
 *
 * Every moving thing on the desktop is a spring, not a timeline:
 *
 *     a = -k (x - target) - c v          (mass 1)
 *
 * A spring has no duration. It has constants, and the time falls out of
 * them. That is the whole point: change the target half way and the
 * spring bends towards the new one carrying the velocity it already
 * had, where a timed curve stops dead and starts over. A panel that is
 * opening and gets closed again turns round; it does not jerk.
 *
 * The constants are the table in feel.md, solved by gen-springs.py for
 * "settles in N ms at damping ratio zeta" - settled meaning within 0.5%
 * of the distance and slower than 5% of it per second. Exits use the
 * same damping ratio with 0.7x the settle time: the decision is already
 * made and the thing is on its way out.
 *
 * Use:
 *
 *     LpSpring open;                        // 0 = hidden, 1 = shown
 *     lp_spring_init(&open, LP_SPRING_SHEET, 0.0);
 *     m = lp_motion_new(widget, on_frame, self);
 *     lp_motion_add(m, &open);
 *     ...
 *     lp_spring_set_target(&open, 1.0);     // show
 *     lp_motion_kick(m);
 *     ...
 *     lp_spring_set_target_out(&open, 0.0); // hide: 0.7x the time
 *     lp_motion_kick(m);
 *
 * on_frame runs once per display frame while any spring moves, after
 * every spring has been stepped; draw from open.x there (queue_draw,
 * set_opacity). When the last spring rests the tick callback is removed,
 * so a still desktop costs no wakeups at all.
 *
 * Reduced motion (Settings -> 접근성 -> 움직임 줄이기, or
 * gtk-enable-animations=false): lp_motion_reduced() is TRUE and every
 * spring settles in under 100ms without overshoot. Callers should also
 * trade translation and scale for a plain fade in that mode - the state
 * must still change visibly, it just must not travel.
 *
 * LP_MOTION_TRACE=1 in the environment prints one line per frame to
 * stderr - "lp-motion: frame dt_ms=16.7 springs=2" - which is how the
 * frame gaps in the track reports are measured.
 */
#ifndef LP_MOTION_H
#define LP_MOTION_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
    LP_SPRING_WINDOW,   /* window open/close, 340ms, no overshoot     */
    LP_SPRING_SLIDE,    /* workspace, big panels, 340ms               */
    LP_SPRING_SHEET,    /* dialogs, quick settings, keyboard, 260ms   */
    LP_SPRING_EXPAND,   /* accordion/height, 240ms, critically damped */
    LP_SPRING_INSERT,   /* list item in/out, 220ms, 4.5% overshoot    */
    LP_SPRING_KNOB,     /* switch knob, 200ms, 6.7% overshoot         */
    LP_SPRING_MENU,     /* menus, popovers, key preview, 180ms        */
    LP_SPRING_PRESS,    /* pressed control scale 0.97, 130ms          */
    LP_SPRING_RUBBER,   /* scroll end snapping back, 420ms            */
    LP_SPRING_COUNT
} LpSpringKind;

typedef struct {
    double       x;       /* where it is now                          */
    double       v;       /* how fast, in units of x per second       */
    double       target;  /* where it is going                        */
    double       k, c;    /* the constants in use for this leg        */
    double       span;    /* distance this leg set out to cover       */
    LpSpringKind kind;
    gboolean     moving;
} LpSpring;

/* Start at rest at x. */
void     lp_spring_init(LpSpring *s, LpSpringKind kind, double x);
/* Go somewhere, keeping the current velocity. Entrances. */
void     lp_spring_set_target(LpSpring *s, double target);
/* Same, with the exit constants: 0.7x the settle time. */
void     lp_spring_set_target_out(LpSpring *s, double target);
/* A finger let go at velocity (units/s): the spring takes it from there. */
void     lp_spring_fling(LpSpring *s, double target, double velocity);
/* Be at x now, at rest. Drags set position with this every event. */
void     lp_spring_jump(LpSpring *s, double x);
/* Advance by dt seconds. TRUE while still moving. Exact for any dt. */
gboolean lp_spring_step(LpSpring *s, double dt);
/* Settle time of a kind in ms, as published in the table. */
guint    lp_spring_ms(LpSpringKind kind, gboolean out);
/* Where a fling at this velocity would carry x if nothing stopped it -
 * the usual way to decide whether a flick opens or closes a panel. */
double   lp_spring_project(double x, double velocity);

/* One frame-clock tick callback per widget drives any number of springs. */
typedef void (*LpMotionFrame)(GtkWidget *widget, gpointer data);
typedef struct _LpMotion LpMotion;

LpMotion *lp_motion_new(GtkWidget *widget, LpMotionFrame frame, gpointer data);
void      lp_motion_add(LpMotion *m, LpSpring *s);
void      lp_motion_remove(LpMotion *m, LpSpring *s);
/* Start ticking if any spring has somewhere to go. Call after set_target. */
void      lp_motion_kick(LpMotion *m);
gboolean  lp_motion_running(LpMotion *m);
void      lp_motion_free(LpMotion *m);

/* TRUE when the person asked for less motion. Cheap; call it freely. */
gboolean  lp_motion_reduced(void);

/* Finger velocity over the last 100ms, for lp_spring_fling(). Feed it
 * every motion event with the event's own timestamp (microseconds, e.g.
 * gdk_event_get_time() * 1000 or g_get_monotonic_time()), and once more
 * at touch-end with the release position and time: a finger that stopped
 * before lifting then has nothing in the window and flings at zero,
 * which is what it meant. */
typedef struct {
    gint64 t[16];
    double x[16];
    int    n, head;
} LpVelocity;

void   lp_velocity_reset(LpVelocity *vt);
void   lp_velocity_add(LpVelocity *vt, gint64 time_us, double x);
double lp_velocity_get(const LpVelocity *vt);

G_END_DECLS

#endif
