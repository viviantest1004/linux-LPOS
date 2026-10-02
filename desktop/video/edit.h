/*
 * edit.h - the editing half of lp-video: what ffmpeg is asked to do.
 *
 * Kept apart from the player so that it can be driven without a
 * window: LP_VIDEO_SELFTEST=1 runs every operation on a file and
 * checks the results with ffprobe, using exactly the argument lists
 * the buttons use.
 */

#ifndef LP_VIDEO_EDIT_H
#define LP_VIDEO_EDIT_H

#include <glib.h>

typedef enum {
    OP_TRIM_FAST,       /* keep the selection, stream copy */
    OP_TRIM_PRECISE,    /* keep the selection, re-encode   */
    OP_CUT,             /* remove the selection, keep the rest */
    OP_AUDIO_MP3,
    OP_AUDIO_M4A,
    OP_FRAME,           /* current position as PNG */
    OP_GIF_480,
    OP_GIF_720,
    OP_ROT_LEFT,
    OP_ROT_RIGHT,
    OP_NO_AUDIO,
    OP_SIZE_1080,
    OP_SIZE_720,
    OP_SIZE_480,
    OP_COUNT
} edit_op_t;

typedef struct {
    double   duration;       /* seconds, 0 if unknown */
    gboolean has_video;      /* a real picture stream, not cover art */
    gboolean has_audio;
    int      width, height;
    char     vcodec[32];
    char     acodec[32];
} media_info_t;

/* ffprobe's argument list, and the parser for what it prints. The UI
 * runs the first asynchronously; the self-test uses media_probe_sync. */
GPtrArray *media_probe_argv(const char *path);
gboolean   media_probe_parse(const char *text, media_info_t *mi);
gboolean   media_probe_sync(const char *path, media_info_t *mi);

gboolean   edit_op_available(edit_op_t op, const media_info_t *mi);
/* Does the operation look at the selection (start..end)? */
gboolean   edit_op_ranged(edit_op_t op);

/* A new, unused file name next to the original: name-trim.mp4,
 * name-trim-1.mp4, ... */
char      *edit_default_output(edit_op_t op, const char *in,
                               const media_info_t *mi);

/* The whole ffmpeg command line, argv[0] included, NULL-terminated. */
GPtrArray *edit_build_argv(edit_op_t op, const char *in, const char *out,
                           const media_info_t *mi,
                           double start, double end, double pos);

/* How long the result will be, for the progress bar. 0 = unknown. */
double     edit_expected_length(edit_op_t op, const media_info_t *mi,
                                double start, double end);

int        edit_selftest(const char *path);

#endif /* LP_VIDEO_EDIT_H */
