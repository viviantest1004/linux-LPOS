/*
 * mpv-min.h - the part of libmpv's client API that lp-video uses.
 *
 * The image carries libmpv.so.2 (mpv 0.35, client API 2.x) but not its
 * headers: libmpv-dev would drag a compiler-side package into an image
 * that has no use for it. So the declarations are written out here,
 * copied from mpv's include/mpv/client.h, render.h and render_gl.h for
 * client API 2.0. Every enum value below is ABI - it is the number the
 * library compares against - so they are spelt out explicitly, gaps
 * and all, rather than left to the compiler to count.
 *
 * The functions are not declared as functions but as pointer types.
 * video.c opens the library with dlopen() at run time: a machine
 * without libmpv should still start, show a clear message, and let the
 * ffmpeg-based editing tools work.
 */

#ifndef LP_MPV_MIN_H
#define LP_MPV_MIN_H

#include <stdint.h>
#include <stddef.h>

/* ── client.h ──────────────────────────────────────────────────── */

typedef struct mpv_handle mpv_handle;

typedef enum mpv_error {
    MPV_ERROR_SUCCESS              = 0,
    MPV_ERROR_EVENT_QUEUE_FULL     = -1,
    MPV_ERROR_NOMEM                = -2,
    MPV_ERROR_UNINITIALIZED        = -3,
    MPV_ERROR_INVALID_PARAMETER    = -4,
    MPV_ERROR_OPTION_NOT_FOUND     = -5,
    MPV_ERROR_OPTION_FORMAT        = -6,
    MPV_ERROR_OPTION_ERROR         = -7,
    MPV_ERROR_PROPERTY_NOT_FOUND   = -8,
    MPV_ERROR_PROPERTY_FORMAT      = -9,
    MPV_ERROR_PROPERTY_UNAVAILABLE = -10,
    MPV_ERROR_PROPERTY_ERROR       = -11,
    MPV_ERROR_COMMAND              = -12,
    MPV_ERROR_LOADING_FAILED       = -13,
    MPV_ERROR_AO_INIT_FAILED       = -14,
    MPV_ERROR_VO_INIT_FAILED       = -15,
    MPV_ERROR_NOTHING_TO_PLAY      = -16,
    MPV_ERROR_UNKNOWN_FORMAT       = -17,
    MPV_ERROR_UNSUPPORTED          = -18,
    MPV_ERROR_NOT_IMPLEMENTED      = -19,
    MPV_ERROR_GENERIC              = -20
} mpv_error;

typedef enum mpv_format {
    MPV_FORMAT_NONE       = 0,
    MPV_FORMAT_STRING     = 1,
    MPV_FORMAT_OSD_STRING = 2,
    MPV_FORMAT_FLAG       = 3,
    MPV_FORMAT_INT64      = 4,
    MPV_FORMAT_DOUBLE     = 5,
    MPV_FORMAT_NODE       = 6,
    MPV_FORMAT_NODE_ARRAY = 7,
    MPV_FORMAT_NODE_MAP   = 8,
    MPV_FORMAT_BYTE_ARRAY = 9
} mpv_format;

typedef enum mpv_event_id {
    MPV_EVENT_NONE               = 0,
    MPV_EVENT_SHUTDOWN           = 1,
    MPV_EVENT_LOG_MESSAGE        = 2,
    MPV_EVENT_GET_PROPERTY_REPLY = 3,
    MPV_EVENT_SET_PROPERTY_REPLY = 4,
    MPV_EVENT_COMMAND_REPLY      = 5,
    MPV_EVENT_START_FILE         = 6,
    MPV_EVENT_END_FILE           = 7,
    MPV_EVENT_FILE_LOADED        = 8,
    /* 9, 10, 12, 13, 15, 19, 23 were removed in client API 2.0. */
    MPV_EVENT_IDLE               = 11,
    MPV_EVENT_TICK               = 14,
    MPV_EVENT_CLIENT_MESSAGE     = 16,
    MPV_EVENT_VIDEO_RECONFIG     = 17,
    MPV_EVENT_AUDIO_RECONFIG     = 18,
    MPV_EVENT_SEEK               = 20,
    MPV_EVENT_PLAYBACK_RESTART   = 21,
    MPV_EVENT_PROPERTY_CHANGE    = 22,
    MPV_EVENT_QUEUE_OVERFLOW     = 24,
    MPV_EVENT_HOOK               = 25
} mpv_event_id;

typedef struct mpv_event_property {
    const char *name;
    mpv_format  format;
    void       *data;
} mpv_event_property;

typedef enum mpv_end_file_reason {
    MPV_END_FILE_REASON_EOF      = 0,
    MPV_END_FILE_REASON_STOP     = 2,
    MPV_END_FILE_REASON_QUIT     = 3,
    MPV_END_FILE_REASON_ERROR    = 4,
    MPV_END_FILE_REASON_REDIRECT = 5
} mpv_end_file_reason;

typedef struct mpv_event_end_file {
    mpv_end_file_reason reason;
    int     error;
    int64_t playlist_entry_id;
    int64_t playlist_insert_id;
    int     playlist_insert_num_entries;
} mpv_event_end_file;

typedef struct mpv_event {
    mpv_event_id event_id;
    int          error;
    uint64_t     reply_userdata;
    void        *data;
} mpv_event;

typedef unsigned long (*mpv_client_api_version_fn)(void);
typedef const char *(*mpv_error_string_fn)(int error);
typedef void        (*mpv_free_fn)(void *data);
typedef mpv_handle *(*mpv_create_fn)(void);
typedef int         (*mpv_initialize_fn)(mpv_handle *ctx);
typedef void        (*mpv_terminate_destroy_fn)(mpv_handle *ctx);
typedef int         (*mpv_set_option_string_fn)(mpv_handle *ctx,
                                                const char *name,
                                                const char *data);
typedef int         (*mpv_command_fn)(mpv_handle *ctx, const char **args);
typedef int         (*mpv_command_async_fn)(mpv_handle *ctx,
                                            uint64_t reply_userdata,
                                            const char **args);
typedef int         (*mpv_set_property_fn)(mpv_handle *ctx, const char *name,
                                           mpv_format format, void *data);
typedef int         (*mpv_set_property_string_fn)(mpv_handle *ctx,
                                                  const char *name,
                                                  const char *data);
typedef int         (*mpv_get_property_fn)(mpv_handle *ctx, const char *name,
                                           mpv_format format, void *data);
typedef int         (*mpv_observe_property_fn)(mpv_handle *mpv,
                                               uint64_t reply_userdata,
                                               const char *name,
                                               mpv_format format);
typedef mpv_event  *(*mpv_wait_event_fn)(mpv_handle *ctx, double timeout);
typedef void        (*mpv_set_wakeup_callback_fn)(mpv_handle *ctx,
                                                  void (*cb)(void *d),
                                                  void *d);

/* ── render.h ──────────────────────────────────────────────────── */

typedef struct mpv_render_context mpv_render_context;

typedef enum mpv_render_param_type {
    MPV_RENDER_PARAM_INVALID               = 0,
    MPV_RENDER_PARAM_API_TYPE              = 1,
    MPV_RENDER_PARAM_OPENGL_INIT_PARAMS    = 2,
    MPV_RENDER_PARAM_OPENGL_FBO            = 3,
    MPV_RENDER_PARAM_FLIP_Y                = 4,
    MPV_RENDER_PARAM_DEPTH                 = 5,
    MPV_RENDER_PARAM_ICC_PROFILE           = 6,
    MPV_RENDER_PARAM_AMBIENT_LIGHT         = 7,
    MPV_RENDER_PARAM_X11_DISPLAY           = 8,
    MPV_RENDER_PARAM_WL_DISPLAY            = 9,
    MPV_RENDER_PARAM_ADVANCED_CONTROL      = 10,
    MPV_RENDER_PARAM_NEXT_FRAME_INFO       = 11,
    MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME = 12,
    MPV_RENDER_PARAM_SKIP_RENDERING        = 13,
    MPV_RENDER_PARAM_DRM_DISPLAY           = 14,
    MPV_RENDER_PARAM_DRM_DRAW_SURFACE_SIZE = 15,
    MPV_RENDER_PARAM_DRM_DISPLAY_V2        = 16,
    MPV_RENDER_PARAM_SW_SIZE               = 17,
    MPV_RENDER_PARAM_SW_FORMAT             = 18,
    MPV_RENDER_PARAM_SW_STRIDE             = 19,
    MPV_RENDER_PARAM_SW_POINTER            = 20
} mpv_render_param_type;

#define MPV_RENDER_API_TYPE_OPENGL "opengl"
#define MPV_RENDER_API_TYPE_SW     "sw"

typedef struct mpv_render_param {
    enum mpv_render_param_type type;
    void *data;
} mpv_render_param;

typedef enum mpv_render_update_flag {
    MPV_RENDER_UPDATE_FRAME = 1 << 0
} mpv_render_update_flag;

typedef void (*mpv_render_update_fn)(void *cb_ctx);

typedef int      (*mpv_render_context_create_fn)(mpv_render_context **res,
                                                 mpv_handle *mpv,
                                                 mpv_render_param *params);
typedef void     (*mpv_render_context_set_update_callback_fn)(
                        mpv_render_context *ctx,
                        mpv_render_update_fn callback,
                        void *callback_ctx);
typedef uint64_t (*mpv_render_context_update_fn)(mpv_render_context *ctx);
typedef int      (*mpv_render_context_render_fn)(mpv_render_context *ctx,
                                                 mpv_render_param *params);
typedef void     (*mpv_render_context_report_swap_fn)(mpv_render_context *ctx);
typedef void     (*mpv_render_context_free_fn)(mpv_render_context *ctx);

/* ── render_gl.h ───────────────────────────────────────────────── */

/* Client API 2.0 dropped the third member (extra_exts) that 1.x had. */
typedef struct mpv_opengl_init_params {
    void *(*get_proc_address)(void *ctx, const char *name);
    void *get_proc_address_ctx;
} mpv_opengl_init_params;

typedef struct mpv_opengl_fbo {
    int fbo;
    int w, h;
    int internal_format;
} mpv_opengl_fbo;

#endif /* LP_MPV_MIN_H */
