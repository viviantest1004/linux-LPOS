/* diskd.h - the Disks application's side of lp-diskd's socket.
 *
 * Every disk operation is lp-diskd's (userland/lp-diskd): this program
 * runs as the person, and the partition tables, the filesystems and the
 * raw devices are root's. So this is the whole of what the application
 * can do to a disk: send one line of TAB-separated fields, read lines
 * back until "done" or "fail". The daemon checks every field; nothing
 * here builds a command line or a path for it.
 *
 * The protocol, as the daemon's header states it: lines of "log",
 * "progress <pct> <text>", "plan", "describe", "step", "stepdone",
 * "state", and TAB-separated records ("disk", "part", "free", "smart",
 * "resize", "bench", "image", "fstab", "journal"), ending in exactly one
 * "done ..." or "fail <why> ...". <why> is denied, auth, invalid,
 * refused, busy, missing, cancelled or failed.
 *
 * "fail auth" is answered here: the password sheet is shown, the
 * password goes to the daemon with the `auth` verb (hex, so a Korean
 * password stays inside the protocol's ASCII), and the request is sent
 * again - so the person sees one dialog and then the job. The daemon
 * keeps that password's effect five minutes per uid; this program never
 * keeps the password itself.
 */
#ifndef LP_DISKD_CLIENT_H
#define LP_DISKD_CLIENT_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Every line before the verdict: kind is its first word ("progress",
 * "part", ...), rest what follows (for records, the TAB-separated
 * key=value fields; for progress, "<pct> <text>"). */
typedef void (*DkLine)(const char *kind, const char *rest, gpointer data);
/* The verdict. why is NULL on success; otherwise the daemon's word, or
 * "cancelled" when the person closed the password sheet, or
 * "unreachable" when the service is not running. */
typedef void (*DkDone)(gboolean ok, const char *why, const char *text,
                       gpointer data);

/* Send fields (NULL-terminated) to lp-diskd. fd >= 0 is passed along
 * with the request (SCM_RIGHTS) - how an image file the person chose
 * reaches the daemon without the daemon ever seeing its name; it is
 * duplicated here, so the caller may close its own copy at once.
 * why_text opens the password sheet ("Changing disks needs..."). */
void dk_request(GtkWindow *parent, const char *const *fields, int fd,
                const char *why_text, DkLine on_line, DkDone on_done,
                gpointer data);

/* The socket: $LP_DISKD_SOCK, or /run/lp-diskd.sock. */
const char *dk_socket(void);

/* key=value records. The table owns its strings. */
GHashTable *dk_rec_parse(const char *rest);
const char *dk_rec_str(GHashTable *r, const char *key);
guint64     dk_rec_u64(GHashTable *r, const char *key);
gint64      dk_rec_i64(GHashTable *r, const char *key);

/* A label for the protocol: itself when it is plain ASCII letters,
 * digits, space, _ . -; otherwise "x:" and the hex of its UTF-8 (how a
 * Korean label crosses an ASCII protocol). "-" for the empty label. */
char *dk_label_field(const char *label);
/* A passphrase: "k:" and its hex. The caller scrubs and frees it. */
char *dk_key_field(const char *pass);

/* "1.5 GiB" - binary units, one decimal below 100, rounded. */
char *dk_size(guint64 bytes);

G_END_DECLS

#endif
