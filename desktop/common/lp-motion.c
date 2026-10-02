/* lp-motion.c - springs, a frame-clock driver and a finger velocity
 * tracker. See lp-motion.h for what and why; this file is the how.
 *
 * Builds against GTK 3 (the layer-shell clients) and GTK 4 (the apps):
 * the only GTK it touches is gtk_widget_add_tick_callback, the frame
 * clock and GtkSettings, which are the same in both.
 */
#include "lp-motion.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* feel.md §2-2, solved by gen-springs.py: stiffness, damping, settle ms. */
static const struct {
    double k, c;
    guint  ms;
} TABLE[LP_SPRING_COUNT] = {
    [LP_SPRING_WINDOW] = {  602.0,  49.1, 340 },
    [LP_SPRING_SLIDE]  = {  602.0,  49.1, 340 },
    [LP_SPRING_SHEET]  = {  856.0,  55.6, 260 },
    [LP_SPRING_EXPAND] = { 1355.0,  73.6, 240 },
    [LP_SPRING_INSERT] = { 1435.0,  53.0, 220 },
    [LP_SPRING_KNOB]   = { 1614.0,  52.2, 200 },
    [LP_SPRING_MENU]   = { 1599.0,  73.6, 180 },
    [LP_SPRING_PRESS]  = { 5005.0, 120.3, 130 },
    [LP_SPRING_RUBBER] = {  368.0,  38.4, 420 },
};

/* Exits settle in 0.7x the time at the same damping ratio. Time scales
 * as 1/sqrt(k) for a fixed ratio, so k grows by 1/0.49 and c by 1/0.7. */
#define OUT_K (1.0 / 0.49)
#define OUT_C (1.0 / 0.70)

/* Reduced motion: critically damped, settled in about 90ms - short
 * enough not to read as movement, long enough not to read as a flicker. */
#define REDUCED_K 11500.0
#define REDUCED_C   214.5

/* The settle rule gen-springs.py publishes durations against. */
#define SETTLE_X 0.005
#define SETTLE_V 0.05

/* Longest step a frame may take. After a stall (a GC, a page fault
 * storm) the spring resumes from where it was rather than teleporting. */
#define MAX_DT (1.0 / 20.0)

static void
constants(LpSpring *s, gboolean out)
{
    if (lp_motion_reduced()) {
        s->k = REDUCED_K;
        s->c = REDUCED_C;
        return;
    }
    s->k = TABLE[s->kind].k * (out ? OUT_K : 1.0);
    s->c = TABLE[s->kind].c * (out ? OUT_C : 1.0);
}

static void
begin_leg(LpSpring *s, double target, gboolean out)
{
    s->target = target;
    constants(s, out);
    /* The rest test is relative to the distance, so a leg that starts on
     * its target but moving (a fling back to where it was) still needs a
     * yardstick: what the velocity would carry it in 50ms. */
    double d = fabs(target - s->x);
    double carry = fabs(s->v) * 0.05;
    s->span = MAX(MAX(d, carry), 1e-3);
    s->moving = d > 0.0 || s->v != 0.0;
}

void
lp_spring_init(LpSpring *s, LpSpringKind kind, double x)
{
    memset(s, 0, sizeof *s);
    s->kind = kind < LP_SPRING_COUNT ? kind : LP_SPRING_SHEET;
    s->x = s->target = x;
    constants(s, FALSE);
    s->span = 1e-3;
}

void
lp_spring_set_target(LpSpring *s, double target)
{
    begin_leg(s, target, FALSE);
}

void
lp_spring_set_target_out(LpSpring *s, double target)
{
    begin_leg(s, target, TRUE);
}

void
lp_spring_fling(LpSpring *s, double target, double velocity)
{
    s->v = isfinite(velocity) ? velocity : 0.0;
    begin_leg(s, target, FALSE);
}

void
lp_spring_jump(LpSpring *s, double x)
{
    s->x = s->target = x;
    s->v = 0.0;
    s->moving = FALSE;
}

/* The damped oscillator has a closed form, so a step is exact for any
 * dt - 8ms at 120Hz or 50ms after a stall - rather than an integration
 * whose error depends on the frame rate. u is the displacement from the
 * target, w0 the natural frequency, z the damping ratio. */
gboolean
lp_spring_step(LpSpring *s, double dt)
{
    if (!s->moving)
        return FALSE;
    if (dt <= 0.0)
        return TRUE;
    if (dt > MAX_DT)
        dt = MAX_DT;

    double u0 = s->x - s->target, v0 = s->v, u, v;
    double w0 = sqrt(s->k), z = s->c / (2.0 * w0);

    if (fabs(z - 1.0) < 1e-4) {                 /* critically damped */
        double b = v0 + w0 * u0, e = exp(-w0 * dt);
        u = (u0 + b * dt) * e;
        v = (b - w0 * (u0 + b * dt)) * e;
    } else if (z < 1.0) {                        /* under-damped */
        double a = z * w0, wd = w0 * sqrt(1.0 - z * z);
        double b = (v0 + a * u0) / wd, e = exp(-a * dt);
        double cs = cos(wd * dt), sn = sin(wd * dt);
        u = e * (u0 * cs + b * sn);
        v = e * (v0 * cs + (-a * b - u0 * wd) * sn);
    } else {                                     /* over-damped */
        double r = sqrt(z * z - 1.0);
        double r1 = -w0 * (z - r), r2 = -w0 * (z + r);
        double c2 = (v0 - r1 * u0) / (r2 - r1), c1 = u0 - c2;
        double e1 = exp(r1 * dt), e2 = exp(r2 * dt);
        u = c1 * e1 + c2 * e2;
        v = r1 * c1 * e1 + r2 * c2 * e2;
    }

    if (fabs(u) < SETTLE_X * s->span && fabs(v) < SETTLE_V * s->span) {
        s->x = s->target;
        s->v = 0.0;
        s->moving = FALSE;
        return FALSE;
    }
    s->x = s->target + u;
    s->v = v;
    return TRUE;
}

guint
lp_spring_ms(LpSpringKind kind, gboolean out)
{
    if (kind >= LP_SPRING_COUNT)
        kind = LP_SPRING_SHEET;
    if (lp_motion_reduced())
        return 90;
    return out ? (guint)(TABLE[kind].ms * 0.7 + 0.5) : TABLE[kind].ms;
}

/* How far a flick would coast. 0.15s is the distance a hand expects a
 * panel to travel after letting go; it decides open-or-closed, it does
 * not animate anything. */
double
lp_spring_project(double x, double velocity)
{
    return x + velocity * 0.15;
}

/* ── the driver ─────────────────────────────────────────────────── */

struct _LpMotion {
    GtkWidget     *widget;
    LpMotionFrame  frame;
    gpointer       data;
    GPtrArray     *springs;
    guint          tick;       /* 0 when idle */
    gint64         last_us;    /* frame time of the previous tick */
    gint64         kicked_us;
};

static gboolean
trace_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = g_getenv("LP_MOTION_TRACE");
        on = e && *e && strcmp(e, "0") != 0;
    }
    return on;
}

static gboolean
on_tick(GtkWidget *w, GdkFrameClock *clock, gpointer user)
{
    LpMotion *m = user;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    gint64 from = m->last_us ? m->last_us : m->kicked_us;
    double dt = (now - from) / 1e6;
    if (dt < 0.0)
        dt = 0.0;
    /* The first frame after a kick can land a whole frame later than the
     * kick; stepping by that is correct and keeps the first frame moving. */
    m->last_us = now;

    gboolean any = FALSE;
    for (guint i = 0; i < m->springs->len; i++)
        any |= lp_spring_step(g_ptr_array_index(m->springs, i), dt);

    if (trace_on())
        fprintf(stderr, "lp-motion: frame dt_ms=%.1f springs=%u moving=%d\n",
                dt * 1000.0, m->springs->len, any);

    if (m->frame)
        m->frame(w, m->data);

    if (!any) {
        m->tick = 0;
        m->last_us = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

LpMotion *
lp_motion_new(GtkWidget *widget, LpMotionFrame frame, gpointer data)
{
    LpMotion *m = g_new0(LpMotion, 1);
    m->widget = widget;
    m->frame = frame;
    m->data = data;
    m->springs = g_ptr_array_new();
    /* The widget may go before its owner frees us; forget it then. */
    g_object_add_weak_pointer(G_OBJECT(widget), (gpointer *)&m->widget);
    return m;
}

void
lp_motion_add(LpMotion *m, LpSpring *s)
{
    if (!g_ptr_array_find(m->springs, s, NULL))
        g_ptr_array_add(m->springs, s);
}

void
lp_motion_remove(LpMotion *m, LpSpring *s)
{
    g_ptr_array_remove(m->springs, s);
}

void
lp_motion_kick(LpMotion *m)
{
    if (!m || !m->widget || m->tick)
        return;
    gboolean any = FALSE;
    for (guint i = 0; i < m->springs->len; i++)
        any |= ((LpSpring *)g_ptr_array_index(m->springs, i))->moving;
    if (!any) {
        /* Nothing to animate, but the caller changed state: draw it. */
        if (m->frame)
            m->frame(m->widget, m->data);
        return;
    }
    m->last_us = 0;
    m->kicked_us = g_get_monotonic_time();
    m->tick = gtk_widget_add_tick_callback(m->widget, on_tick, m, NULL);
}

gboolean
lp_motion_running(LpMotion *m)
{
    return m && m->tick != 0;
}

void
lp_motion_free(LpMotion *m)
{
    if (!m)
        return;
    if (m->widget) {
        if (m->tick)
            gtk_widget_remove_tick_callback(m->widget, m->tick);
        g_object_remove_weak_pointer(G_OBJECT(m->widget), (gpointer *)&m->widget);
    }
    g_ptr_array_free(m->springs, TRUE);
    g_free(m);
}

/* ── reduced motion ─────────────────────────────────────────────── */

/* Three ways to ask, any one is enough: the environment (for tests),
 * the flag file the Settings switch writes, and GTK's own setting that
 * the same switch turns off. Looked up at most once a second - this is
 * called on every set_target. */
gboolean
lp_motion_reduced(void)
{
    static gint64 checked;
    static gboolean cached;
    gint64 now = g_get_monotonic_time();
    if (checked && now - checked < G_USEC_PER_SEC)
        return cached;
    checked = now;

    const char *e = g_getenv("LP_REDUCE_MOTION");
    if (e && *e) {
        cached = strcmp(e, "0") != 0;
        return cached;
    }
    char *flag = g_build_filename(g_get_user_config_dir(), "lp", "reduce-motion", NULL);
    cached = g_file_test(flag, G_FILE_TEST_EXISTS);
    g_free(flag);
    if (!cached) {
        GtkSettings *gs = gtk_settings_get_default();
        gboolean anim = TRUE;
        if (gs)
            g_object_get(gs, "gtk-enable-animations", &anim, NULL);
        cached = !anim;
    }
    return cached;
}

/* ── finger velocity ────────────────────────────────────────────── */

void
lp_velocity_reset(LpVelocity *vt)
{
    memset(vt, 0, sizeof *vt);
}

void
lp_velocity_add(LpVelocity *vt, gint64 time_us, double x)
{
    vt->t[vt->head] = time_us;
    vt->x[vt->head] = x;
    vt->head = (vt->head + 1) % 16;
    if (vt->n < 16)
        vt->n++;
}

/* Least-squares slope over the samples of the last 100ms. A two-point
 * difference is at the mercy of one late event; a finger that paused
 * before lifting has no samples in the window and so no velocity, which
 * is what it meant. */
double
lp_velocity_get(const LpVelocity *vt)
{
    if (vt->n < 2)
        return 0.0;
    int newest = (vt->head + 15) % 16;
    gint64 tn = vt->t[newest];
    double st = 0, sx = 0, stt = 0, stx = 0;
    int cnt = 0;
    for (int i = 0; i < vt->n; i++) {
        int j = (newest - i + 16) % 16;
        gint64 age = tn - vt->t[j];
        if (age > 100000 || age < 0)
            break;
        double t = -age / 1e6;
        st += t; sx += vt->x[j]; stt += t * t; stx += t * vt->x[j];
        cnt++;
    }
    if (cnt < 2)
        return 0.0;
    double den = cnt * stt - st * st;
    if (fabs(den) < 1e-12)
        return 0.0;
    return (cnt * stx - st * sx) / den;
}
