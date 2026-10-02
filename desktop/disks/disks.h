/* disks.h - what the pieces of lp-disks share: the disks as lp-diskd
 * describes them, the queued plan, and the application's one state.
 *
 * The files: disks.c is the window; model.c turns the daemon's records
 * into Disk/Part and plays the queued plan on a copy of them (so the
 * map can show the disk as it WILL be); map.c draws the partition map
 * and the range bar the resize and create sheets drag; dialogs.c is
 * every sheet, the apply-confirm-progress sequence included. diskd.c
 * is the socket.
 */
#ifndef LP_DISKS_H
#define LP_DISKS_H

#include <gtk/gtk.h>
#include "lp-kit.h"
#include "diskd.h"

G_BEGIN_DECLS

#define MIB ((guint64)1048576)
#define GIB (1024 * MIB)

/* A partition, a filesystem written straight on a disk (num 0), or a
 * gap (is_free). As listed, or as the plan will leave it. */
typedef struct {
    gboolean is_free;
    char    *disk, *name;              /* loop2, loop2p3 ("" when planned) */
    int      num;
    guint64  start, size;              /* bytes */
    char    *type, *typename, *typedesc, *partuuid, *partlabel, *flags;
    char    *fs, *label, *uuid, *state, *mount, *inuse, *why, *protect;
    char    *inner, *innerfs, *innerlabel, *innermount;
    guint64  fssize, used;
    gboolean used_known, aligned, logical, kernel;
    /* The plan: created by step N (1-based), or changed by it. */
    int      created_by;
    gboolean changed;
    char    *key;                      /* stable id for the map's springs */
} Part;

typedef struct {
    char     *name, *model, *transport, *table, *id, *err;
    guint64   size, first, end, lss;
    gboolean  removable, rotational, ro, system;
    GPtrArray *parts;                  /* Part*, partitions only */
    char     *health, *health_reasons; /* from `smart`, NULL until known */
} Disk;

typedef enum {
    OP_MKLABEL, OP_CREATE, OP_DELETE, OP_RESIZE, OP_MOVE, OP_FORMAT,
    OP_LABEL, OP_NAME, OP_TYPE, OP_FLAGS, OP_CHECK, OP_REPAIR, OP_WIPE
} OpKind;

typedef struct {
    OpKind   kind;
    char    *disk;
    char    *ref;          /* PARTUUID, "@N", or the disk's name */
    guint64  a, b;         /* create: start,size; resize: size; move: start */
    char    *fs, *label, *ptype, *flags;
    gboolean gpt;
    char    *pass;         /* LUKS passphrase, scrubbed when the plan ends */
    char    *confirm;      /* the typed confirmation token (ESP ...) */
} Op;

typedef struct {
    GtkApplication *gapp;
    GtkWidget  *win;
    GPtrArray  *disks;             /* Disk* as listed */
    GPtrArray  *plan;              /* Op* */
    char       *sel_disk;          /* name */
    char       *sel_key;           /* Part key in the selected disk's view */
    GPtrArray  *fstab;             /* GHashTable* records */
    char       *journal;           /* an interrupted move's partuuid, or NULL */
    gboolean    admin, is_root;
    gboolean    busy;              /* a plan is running */
    gboolean    have_cryptsetup, have_btrfs, have_fatresize;
    /* widgets */
    GtkWidget  *side, *stack, *map, *details, *actions, *hint, *hint_label,
               *hint_button, *head_title, *head_sub, *health_chip,
               *plan_rev, *plan_list, *plan_title, *apply_btn, *banner,
               *banner_label, *banner_btn, *empty, *disk_menu_btn;
    GKeyFile   *state;
    char       *sys_names;         /* last /sys/class/block listing */
} App;

extern App *A;

/* model.c */
void    part_free(Part *p);
Part   *part_copy(const Part *p);
void    disk_free(Disk *d);
Disk   *disk_find(const char *name);
Part   *disk_part_by_uuid(Disk *d, const char *uuid);
/* The partitions and gaps of d with every queued step for it played,
 * sorted by start. The array owns its Parts. */
GPtrArray *view_of(Disk *d);
Part   *view_find(GPtrArray *view, const char *key);
void    list_begin(void);
void    list_line(const char *kind, const char *rest);
void    list_end(void);
Op     *op_new(OpKind k, const char *disk, const char *ref);
void    op_free(Op *o);
char   *op_describe(const Op *o, int index);
/* The plan as request fields: "plan", then steps separated by "|".
 * pre: "plan" or "preview". NULL-terminated; g_strfreev it (after
 * scrubbing - it holds passphrases). */
char  **plan_fields(const char *verb);
void    plan_scrub(void);
const char *fs_name(const char *fs);         /* "vfat" -> "FAT32" ... */
void    fs_color(const char *fs, gboolean is_free, double rgb[3]);
gboolean part_is_vital(const Part *p);
gboolean part_busy(const Part *p);           /* mounted, swap, holder */
char   *part_title(const Part *p);           /* "loop2p2 · DATA" */
/* The room a partition can use (bytes): from the end of the one before
 * it (or the disk's first usable byte) to the start of the next (or the
 * end), within the view. */
void    part_room(Disk *d, GPtrArray *view, const Part *p, guint64 *lo, guint64 *hi);
char   *clean_label_for_point(const char *label, const char *fallback);

/* map.c */
GtkWidget *dk_map_new(void);
void       dk_map_set(GtkWidget *map, GPtrArray *view, const char *sel_key,
                      guint64 disk_size, gboolean animate);
typedef void (*DkMapSelect)(const char *key, gboolean context, double x,
                            double y, gpointer data);
void       dk_map_on_select(GtkWidget *map, DkMapSelect cb, gpointer data);

/* A bar for choosing a range inside [lo, hi): the block [start,
 * start+size) with a handle at each end and a draggable middle. `mode`
 * decides what the handles may do. Values are bytes, MiB-aligned. */
typedef enum { RANGE_CREATE, RANGE_RESIZE } RangeMode;
GtkWidget *dk_range_new(RangeMode mode, guint64 lo, guint64 hi,
                        guint64 start, guint64 size, guint64 min_size,
                        guint64 max_size, gboolean can_move, const char *fs);
void       dk_range_get(GtkWidget *r, guint64 *start, guint64 *size);
void       dk_range_set(GtkWidget *r, guint64 start, guint64 size, gboolean animate);
void       dk_range_on_change(GtkWidget *r, GCallback cb, gpointer data);

/* dialogs.c */
void dlg_create(Part *free_region);
void dlg_resize(Part *p);
void dlg_format(Part *p);
void dlg_edit(Part *p);
void dlg_delete(Part *p);
void dlg_new_table(Disk *d);
void dlg_unlock(Part *p);
void dlg_smart(Disk *d);
void dlg_bench(Part *p, Disk *d);
void dlg_image(Part *p, gboolean save);
void dlg_fstab(Part *p);
void dlg_erase(Disk *d);
void dlg_recovery(Part *p);
void dlg_apply(void);
void dlg_error(const char *title, const char *text);
void run_simple(const char *const *fields, const char *busy_text,
                const char *ok_text);

/* disks.c */
void app_refresh(void);
void app_plan_changed(void);
void app_select(const char *disk, const char *key);
void app_toast(const char *text);
Disk *app_disk(void);

G_END_DECLS

#endif
