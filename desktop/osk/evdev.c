#define _GNU_SOURCE 1
/*
 * evdev.c - what kind of device a person used last: the touchscreen, a
 * keyboard, or a mouse/touchpad.
 *
 * The keyboard shows itself when a text field gets focus, but only if the
 * field was touched: someone who clicked it with the touchpad has a
 * keyboard under their hands, and a third of the screen sliding up every
 * time they click a search box is the reason people switch on-screen
 * keyboards off. Wayland tells a client nothing about input that went to
 * another client, and text-input-v3 does not say what caused the focus,
 * so the only place to learn it is the kernel's input devices.
 *
 * This reads them without grabbing - libinput and the compositor see
 * everything exactly as before - and asks the kernel (EVIOCSMASK) to
 * deliver only EV_KEY events: BTN_TOUCH from touchscreens, the buttons
 * of mice and touchpads, and key presses from keyboards. Motion, which
 * arrives hundreds of times a second, never reaches this process, so a
 * moving finger or mouse costs no wakeups here; only a touch-down, a
 * click or a key press does. Key codes are looked at only to tell a key
 * from a button and are never kept: this records WHEN and WHAT KIND,
 * nothing about which key.
 *
 * The presses of mice, touchpads and touchscreens also go to type.c, with
 * the kernel's time on the monotonic clock (EVIOCSCLOCKID, per open file,
 * so nobody else's timestamps change): a press may take the focus away,
 * so a Hangul syllable shown as a preedit becomes text at once, and one
 * already typed live ends at a press that was not on our keys, because
 * GTK does not say when a click moves the cursor (type.c, head comment 1.).
 *
 * Reading /dev/input needs the permission start-desktop gives the session
 * user (or membership of group input). Without it this reports
 * "no access" and the keyboard falls back to the button and the gesture;
 * nothing else breaks.
 */
#include "osk.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <linux/input.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define MAXDEV 32
#define BITS_PER_LONG (8 * sizeof(unsigned long))
#define NLONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(a, b) (((a)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1)

typedef struct {
    int   fd;
    int   kind;        /* INPUT_* */
    guint watch;
    char  node[32];
} Dev;

static Dev    devs[MAXDEV];
static int    ndevs;
static int    last_kind = INPUT_UNKNOWN;
static gint64 last_us;
static int    n_denied, n_seen;
static int    ino_fd = -1;

static int classify(int fd)
{
    unsigned long ev[NLONGS(EV_CNT)] = {0}, keys[NLONGS(KEY_CNT)] = {0};
    unsigned long abs[NLONGS(ABS_CNT)] = {0}, rel[NLONGS(REL_CNT)] = {0};
    unsigned long props[NLONGS(INPUT_PROP_CNT)] = {0};
    if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0)
        return INPUT_UNKNOWN;
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
    ioctl(fd, EVIOCGPROP(sizeof props), props);

    gboolean has_abs = TEST_BIT(ev, EV_ABS), has_key = TEST_BIT(ev, EV_KEY);
    if (has_abs && TEST_BIT(abs, ABS_MT_POSITION_X) &&
        TEST_BIT(props, INPUT_PROP_DIRECT))
        return INPUT_TOUCH;          /* a touchscreen: the finger is on the picture */
    if (has_key && (TEST_BIT(keys, BTN_LEFT) || TEST_BIT(keys, BTN_TOUCH)) &&
        (TEST_BIT(ev, EV_REL) || has_abs))
        return INPUT_POINTER;        /* mouse, touchpad, trackpoint */
    if (has_key && TEST_BIT(keys, KEY_A) && TEST_BIT(keys, KEY_Z) &&
        TEST_BIT(keys, KEY_SPACE))
        return INPUT_KEY;            /* a keyboard with letters, not a power button */
    (void)rel;
    return INPUT_UNKNOWN;
}

/* Only EV_KEY, and of that only what tells the kinds apart. EV_SYN cannot
 * be masked, but the kernel drops the empty SYN_REPORTs that masking
 * leaves behind, so a masked-out motion event wakes nobody. */
static void set_mask(int fd, int kind)
{
    unsigned long types[NLONGS(EV_CNT)] = {0};
    unsigned long keys[NLONGS(KEY_CNT)] = {0};
    types[EV_KEY / BITS_PER_LONG] |= 1ul << (EV_KEY % BITS_PER_LONG);
#define SETK(k) (keys[(k) / BITS_PER_LONG] |= 1ul << ((k) % BITS_PER_LONG))
    if (kind == INPUT_TOUCH) {
        SETK(BTN_TOUCH);
    } else if (kind == INPUT_POINTER) {
        SETK(BTN_LEFT); SETK(BTN_RIGHT); SETK(BTN_MIDDLE); SETK(BTN_TOUCH);
    } else {
        for (int k = 1; k < BTN_MISC; k++)
            SETK(k);
    }
#undef SETK
    struct input_mask m = { .type = EV_SYN, .codes_size = sizeof types,
                            .codes_ptr = (uint64_t)(uintptr_t)types };
    ioctl(fd, EVIOCSMASK, &m);
    m.type = EV_KEY;
    m.codes_size = sizeof keys;
    m.codes_ptr = (uint64_t)(uintptr_t)keys;
    ioctl(fd, EVIOCSMASK, &m);
}

static void drop(Dev *d)
{
    if (d->watch)
        g_source_remove(d->watch);
    if (d->fd >= 0)
        close(d->fd);
    *d = devs[--ndevs];
}

static gboolean on_dev(gint fd, GIOCondition cond, gpointer data)
{
    (void)data;
    Dev *d = NULL;
    for (int i = 0; i < ndevs; i++)
        if (devs[i].fd == fd)
            d = &devs[i];
    if (!d)
        return G_SOURCE_REMOVE;
    if (cond & (G_IO_ERR | G_IO_HUP)) {
        d->watch = 0;
        drop(d);
        return G_SOURCE_REMOVE;
    }
    struct input_event ev[32];
    ssize_t n;
    gboolean pressed = FALSE;
    while ((n = read(fd, ev, sizeof ev)) > 0) {
        for (size_t i = 0; i < (size_t)n / sizeof ev[0]; i++)
            if (ev[i].type == EV_KEY && ev[i].value == 1) {
                pressed = TRUE;
                if (d->kind != INPUT_KEY)
                    type_pointer_press((uint32_t)((uint64_t)ev[i].input_event_sec * 1000u +
                                                  (uint64_t)ev[i].input_event_usec / 1000u));
            }
    }
    if (n < 0 && errno == ENODEV) {
        d->watch = 0;
        drop(d);
        return G_SOURCE_REMOVE;
    }
    if (pressed) {
        last_kind = d->kind;
        last_us = g_get_monotonic_time();
        if (d->kind == INPUT_KEY)
            osk_hardware_key();
    }
    return G_SOURCE_CONTINUE;
}

static void add_node(const char *name)
{
    if (strncmp(name, "event", 5) || ndevs >= MAXDEV)
        return;
    char path[64];
    g_snprintf(path, sizeof path, "/dev/input/%s", name);
    for (int i = 0; i < ndevs; i++)
        if (!strcmp(devs[i].node, name))
            return;
    n_seen++;
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM)
            n_denied++;
        return;
    }
    int kind = classify(fd);
    if (kind == INPUT_UNKNOWN) {
        close(fd);
        return;
    }
    set_mask(fd, kind);
    int clk = CLOCK_MONOTONIC;
    ioctl(fd, EVIOCSCLOCKID, &clk);
    Dev *d = &devs[ndevs++];
    d->fd = fd;
    d->kind = kind;
    g_strlcpy(d->node, name, sizeof d->node);
    d->watch = g_unix_fd_add(fd, G_IO_IN | G_IO_ERR | G_IO_HUP, on_dev, NULL);
}

static gboolean on_inotify(gint fd, GIOCondition cond, gpointer data)
{
    (void)cond; (void)data;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n = read(fd, buf, sizeof buf);
    for (char *p = buf; n > 0 && p < buf + n;) {
        struct inotify_event *e = (struct inotify_event *)p;
        if (e->len && (e->mask & (IN_CREATE | IN_ATTRIB)))
            add_node(e->name);    /* a keyboard plugged in, or udev chmod'ed it */
        p += sizeof *e + e->len;
    }
    return G_SOURCE_CONTINUE;
}

void lastinput_init(void)
{
    DIR *dir = opendir("/dev/input");
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)))
            add_node(e->d_name);
        closedir(dir);
    }
    ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino_fd >= 0) {
        if (inotify_add_watch(ino_fd, "/dev/input", IN_CREATE | IN_ATTRIB) >= 0)
            g_unix_fd_add(ino_fd, G_IO_IN, on_inotify, NULL);
        else {
            close(ino_fd);
            ino_fd = -1;
        }
    }
}

int lastinput_query(gint64 *age_ms)
{
    if (age_ms)
        *age_ms = last_us ? (g_get_monotonic_time() - last_us) / 1000 : -1;
    return last_kind;
}

void lastinput_force(int kind)
{
    last_kind = kind;
    last_us = g_get_monotonic_time();
}

const char *lastinput_status(void)
{
    static char buf[96];
    int t = 0, k = 0, p = 0;
    for (int i = 0; i < ndevs; i++)
        t += devs[i].kind == INPUT_TOUCH, k += devs[i].kind == INPUT_KEY,
        p += devs[i].kind == INPUT_POINTER;
    if (!ndevs && n_denied)
        return "no access";
    g_snprintf(buf, sizeof buf, "%d touch, %d keyboard, %d pointer%s", t, k, p,
               n_denied ? " (some denied)" : "");
    return buf;
}
