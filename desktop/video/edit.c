/* g_spawn and friends want POSIX declarations -std=c11 hides. */
#define _DEFAULT_SOURCE 1

/*
 * edit.c - trimming and the other tools, as ffmpeg command lines.
 *
 * Every tool writes a NEW file. Nothing here ever touches the original:
 * on a machine this small a lost home video is not recoverable, and a
 * trim that went wrong should cost one more click, not the file.
 *
 * ── Why these particular commands ──
 *
 * "Fast" trim is a stream copy. It is the only thing a Pi Zero 2 W can
 * do to an hour of 1080p in seconds rather than an hour, and for most
 * people that is the point of trimming. The price is that the cut can
 * only begin on a key frame, which the button's tooltip says.
 *
 * Everything that changes pictures (precise trim, removing a part,
 * rotating, resizing) re-encodes to H.264 + AAC in MP4, because that is
 * the one combination every phone, browser and TV plays. -preset
 * veryfast because the slower presets buy a few percent of size at a
 * cost of several times the wait on this CPU.
 *
 * Numbers passed to ffmpeg are formatted with g_ascii_formatd: under a
 * locale with a decimal comma, printf("%f") would write "3,500" and
 * ffmpeg would read three seconds and a syntax error.
 */

#include "edit.h"
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── probing ───────────────────────────────────────────────────── */

GPtrArray *media_probe_argv(const char *path)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    const char *fixed[] = {
        "ffprobe", "-v", "error",
        "-show_entries",
        "format=duration:stream=codec_type,codec_name,width,height"
        ":stream_disposition=attached_pic",
        "-of", "default=nw=0",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(fixed); i++)
        g_ptr_array_add(a, g_strdup(fixed[i]));
    /* "file:" so that a name beginning with a protocol-looking prefix
     * ("http:..." is a legal file name) is still read as a file. */
    g_ptr_array_add(a, g_strconcat("file:", path, NULL));
    g_ptr_array_add(a, NULL);
    return a;
}

gboolean media_probe_parse(const char *text, media_info_t *mi)
{
    memset(mi, 0, sizeof *mi);
    if (!text) return FALSE;

    /* ffprobe's default writer: [STREAM] ... [/STREAM] blocks of
     * key=value, then [FORMAT]. A stream is only judged when its block
     * closes, because attached_pic comes after codec_type. */
    char type[16] = "", codec[32] = "";
    int w = 0, h = 0, pic = 0;
    gboolean any = FALSE;

    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l; l++) {
        char *s = g_strstrip(*l);
        if (strcmp(s, "[STREAM]") == 0) {
            type[0] = codec[0] = 0; w = h = pic = 0;
        } else if (strcmp(s, "[/STREAM]") == 0) {
            if (strcmp(type, "video") == 0 && !pic) {
                if (!mi->has_video) {
                    mi->has_video = TRUE;
                    mi->width = w; mi->height = h;
                    g_strlcpy(mi->vcodec, codec, sizeof mi->vcodec);
                }
                any = TRUE;
            } else if (strcmp(type, "audio") == 0) {
                if (!mi->has_audio) {
                    mi->has_audio = TRUE;
                    g_strlcpy(mi->acodec, codec, sizeof mi->acodec);
                }
                any = TRUE;
            }
        } else if (g_str_has_prefix(s, "codec_type=")) {
            g_strlcpy(type, s + 11, sizeof type);
        } else if (g_str_has_prefix(s, "codec_name=")) {
            g_strlcpy(codec, s + 11, sizeof codec);
        } else if (g_str_has_prefix(s, "width=")) {
            w = atoi(s + 6);
        } else if (g_str_has_prefix(s, "height=")) {
            h = atoi(s + 7);
        } else if (g_str_has_prefix(s, "DISPOSITION:attached_pic=")) {
            pic = atoi(s + 25);
        } else if (g_str_has_prefix(s, "duration=")) {
            double d = g_ascii_strtod(s + 9, NULL);
            if (d > 0) mi->duration = d;
        }
    }
    g_strfreev(lines);
    return any;
}

gboolean media_probe_sync(const char *path, media_info_t *mi)
{
    GPtrArray *a = media_probe_argv(path);
    char *out = NULL;
    int status = 0;
    gboolean ok = g_spawn_sync(NULL, (char **)a->pdata, NULL,
                               G_SPAWN_SEARCH_PATH |
                               G_SPAWN_STDERR_TO_DEV_NULL,
                               NULL, NULL, &out, NULL, &status, NULL)
                  && g_spawn_check_wait_status(status, NULL);
    ok = ok && media_probe_parse(out, mi);
    if (!ok) memset(mi, 0, sizeof *mi);
    g_free(out);
    g_ptr_array_unref(a);
    return ok;
}

/* ── what applies to what ──────────────────────────────────────── */

gboolean edit_op_available(edit_op_t op, const media_info_t *mi)
{
    switch (op) {
    case OP_TRIM_FAST: case OP_TRIM_PRECISE: case OP_CUT:
        return mi->has_video || mi->has_audio;
    case OP_AUDIO_MP3: case OP_AUDIO_M4A:
        return mi->has_audio;
    case OP_NO_AUDIO:
        return mi->has_video && mi->has_audio;
    case OP_FRAME: case OP_GIF_480: case OP_GIF_720:
    case OP_ROT_LEFT: case OP_ROT_RIGHT:
    case OP_SIZE_1080: case OP_SIZE_720: case OP_SIZE_480:
        return mi->has_video;
    default:
        return FALSE;
    }
}

gboolean edit_op_ranged(edit_op_t op)
{
    return op == OP_TRIM_FAST || op == OP_TRIM_PRECISE || op == OP_CUT ||
           op == OP_AUDIO_MP3 || op == OP_AUDIO_M4A ||
           op == OP_GIF_480 || op == OP_GIF_720;
}

/* ── names ─────────────────────────────────────────────────────── */

/* The extension including the dot, lower-cased; "" if none. */
static char *ext_of(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base) return g_strdup("");
    return g_ascii_strdown(dot, -1);
}

static gboolean audio_ext_known(const char *ext)
{
    static const char *known[] = {
        ".mp3", ".m4a", ".aac", ".flac", ".wav", ".ogg", ".oga", ".opus",
    };
    for (size_t i = 0; i < G_N_ELEMENTS(known); i++)
        if (strcmp(ext, known[i]) == 0) return TRUE;
    return FALSE;
}

/* The container a re-encoded result goes into. Video always becomes
 * MP4 (see the top of the file); audio keeps its own kind where ffmpeg
 * can write it, so trimming a FLAC gives a FLAC. */
static char *reencode_ext(const char *in, const media_info_t *mi)
{
    if (mi->has_video) return g_strdup(".mp4");
    char *e = ext_of(in);
    if (audio_ext_known(e)) return e;
    g_free(e);
    return g_strdup(".m4a");
}

static void op_name(edit_op_t op, const char *in, const media_info_t *mi,
                    const char **suffix, char **ext)
{
    switch (op) {
    case OP_TRIM_FAST: {
        *suffix = "trim";
        *ext = ext_of(in);
        if (!**ext) { g_free(*ext); *ext = g_strdup(".mkv"); }
        break;
    }
    case OP_TRIM_PRECISE: *suffix = "trim";    *ext = reencode_ext(in, mi); break;
    case OP_CUT:          *suffix = "cut";     *ext = reencode_ext(in, mi); break;
    case OP_AUDIO_MP3:    *suffix = "audio";   *ext = g_strdup(".mp3"); break;
    case OP_AUDIO_M4A:    *suffix = "audio";   *ext = g_strdup(".m4a"); break;
    case OP_FRAME:        *suffix = "frame";   *ext = g_strdup(".png"); break;
    case OP_GIF_480:
    case OP_GIF_720:      *suffix = "clip";    *ext = g_strdup(".gif"); break;
    case OP_ROT_LEFT:
    case OP_ROT_RIGHT:    *suffix = "rotated"; *ext = g_strdup(".mp4"); break;
    case OP_NO_AUDIO: {
        *suffix = "noaudio";
        *ext = ext_of(in);
        if (!**ext) { g_free(*ext); *ext = g_strdup(".mkv"); }
        break;
    }
    case OP_SIZE_1080:    *suffix = "1080p";   *ext = g_strdup(".mp4"); break;
    case OP_SIZE_720:     *suffix = "720p";    *ext = g_strdup(".mp4"); break;
    case OP_SIZE_480:     *suffix = "480p";    *ext = g_strdup(".mp4"); break;
    default:              *suffix = "edit";    *ext = g_strdup(".mp4"); break;
    }
}

char *edit_default_output(edit_op_t op, const char *in,
                          const media_info_t *mi)
{
    const char *suffix;
    char *ext;
    op_name(op, in, mi, &suffix, &ext);

    char *dir  = g_path_get_dirname(in);
    char *base = g_path_get_basename(in);
    char *dot  = strrchr(base, '.');
    if (dot && dot != base) *dot = 0;

    /* Never overwrite: the first free name among name-trim.mp4,
     * name-trim-1.mp4, name-trim-2.mp4 ... */
    char *path = NULL;
    for (int n = 0; n < 10000; n++) {
        char *leaf = n == 0
            ? g_strdup_printf("%s-%s%s", base, suffix, ext)
            : g_strdup_printf("%s-%s-%d%s", base, suffix, n, ext);
        g_free(path);
        path = g_build_filename(dir, leaf, NULL);
        g_free(leaf);
        if (!g_file_test(path, G_FILE_TEST_EXISTS)) break;
    }
    g_free(dir); g_free(base); g_free(ext);
    return path;
}

/* ── command lines ─────────────────────────────────────────────── */

static void add(GPtrArray *a, const char *s) { g_ptr_array_add(a, g_strdup(s)); }

static void add_time(GPtrArray *a, const char *opt, double t)
{
    char buf[G_ASCII_DTOSTR_BUF_SIZE];
    add(a, opt);
    add(a, g_ascii_formatd(buf, sizeof buf, "%.3f", t < 0 ? 0 : t));
}

static gboolean whole(const media_info_t *mi, double start, double end)
{
    return start <= 0.01 && (mi->duration <= 0 || end >= mi->duration - 0.01);
}

/* Input-side -ss/-to: ffmpeg seeks before decoding, which is fast, and
 * when it re-encodes it still discards up to the exact time. */
static void add_range_in(GPtrArray *a, const media_info_t *mi,
                         double start, double end)
{
    if (whole(mi, start, end)) return;
    if (start > 0.01) add_time(a, "-ss", start);
    if (mi->duration <= 0 || end < mi->duration - 0.01)
        add_time(a, "-to", end);
}

static void add_input(GPtrArray *a, const char *in)
{
    add(a, "-i");
    g_ptr_array_add(a, g_strconcat("file:", in, NULL));
}

/* H.264 in yuv420p refuses odd sizes; the scale rounds them down. Only
 * added when needed, so the common case has no extra filter. */
static char *even_filter(const media_info_t *mi)
{
    if ((mi->width & 1) || (mi->height & 1))
        return g_strdup("scale=trunc(iw/2)*2:trunc(ih/2)*2");
    return NULL;
}

static void add_h264(GPtrArray *a, const char *crf)
{
    add(a, "-c:v"); add(a, "libx264");
    add(a, "-crf"); add(a, crf);
    add(a, "-preset"); add(a, "veryfast");
    add(a, "-pix_fmt"); add(a, "yuv420p");
}

static void add_aac(GPtrArray *a, const char *rate)
{
    add(a, "-c:a"); add(a, "aac");
    add(a, "-b:a"); add(a, rate);
}

/* Audio encoder for an audio-only result, chosen by its extension. */
static void add_audio_codec_for(GPtrArray *a, const char *out)
{
    char *e = ext_of(out);
    if (strcmp(e, ".mp3") == 0) {
        add(a, "-c:a"); add(a, "libmp3lame"); add(a, "-b:a"); add(a, "192k");
    } else if (strcmp(e, ".m4a") == 0 || strcmp(e, ".aac") == 0 ||
               strcmp(e, ".mp4") == 0) {
        add_aac(a, "192k");
    } else if (strcmp(e, ".flac") == 0) {
        add(a, "-c:a"); add(a, "flac");
    } else if (strcmp(e, ".wav") == 0) {
        add(a, "-c:a"); add(a, "pcm_s16le");
    } else if (strcmp(e, ".ogg") == 0 || strcmp(e, ".oga") == 0) {
        add(a, "-c:a"); add(a, "libvorbis"); add(a, "-q:a"); add(a, "5");
    } else if (strcmp(e, ".opus") == 0) {
        add(a, "-c:a"); add(a, "libopus"); add(a, "-b:a"); add(a, "128k");
    }
    /* anything else: ffmpeg's default for that container */
    g_free(e);
}

static void add_mp4_tail(GPtrArray *a, const char *out)
{
    char *e = ext_of(out);
    if (strcmp(e, ".mp4") == 0 || strcmp(e, ".m4a") == 0 ||
        strcmp(e, ".mov") == 0) {
        /* The index at the front, so the file starts playing at once
         * when it is opened over a network share. */
        add(a, "-movflags"); add(a, "+faststart");
    }
    g_free(e);
}

/* Remove start..end and join what is left. One ffmpeg run with trim and
 * concat filters: no temporary files, and the join is exact. */
static void build_cut(GPtrArray *a, const char *in, const char *out,
                      const media_info_t *mi, double start, double end)
{
    double dur = mi->duration;
    gboolean head = start > 0.05;
    gboolean tail = dur <= 0 || end < dur - 0.05;
    gboolean v = mi->has_video, au = mi->has_audio;
    char s[G_ASCII_DTOSTR_BUF_SIZE], e[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(s, sizeof s, "%.3f", start);
    g_ascii_formatd(e, sizeof e, "%.3f", end);

    if (head && tail) {
        add_input(a, in);
        GString *f = g_string_new(NULL);
        if (v) g_string_append_printf(f,
            "[0:v:0]trim=end=%s,setpts=PTS-STARTPTS[v0];"
            "[0:v:0]trim=start=%s,setpts=PTS-STARTPTS[v1];", s, e);
        if (au) g_string_append_printf(f,
            "[0:a:0]atrim=end=%s,asetpts=PTS-STARTPTS[a0];"
            "[0:a:0]atrim=start=%s,asetpts=PTS-STARTPTS[a1];", s, e);
        g_string_append_printf(f, "%s%s%s%sconcat=n=2:v=%d:a=%d",
                               v ? "[v0]" : "", au ? "[a0]" : "",
                               v ? "[v1]" : "", au ? "[a1]" : "",
                               v ? 1 : 0, au ? 1 : 0);
        if (v) g_string_append(f, "[v]");
        if (au) g_string_append(f, "[a]");
        if (v) {
            char *ev = even_filter(mi);
            if (ev) {
                /* Rename the concat output and scale it into [v]. */
                char *p = strstr(f->str, "[v]");
                g_string_insert(f, p - f->str + 2, "c");  /* [vc] */
                g_string_append_printf(f, ";[vc]%s[v]", ev);
                g_free(ev);
            }
        }
        add(a, "-filter_complex"); add(a, f->str);
        g_string_free(f, TRUE);
        if (v)  { add(a, "-map"); add(a, "[v]"); }
        if (au) { add(a, "-map"); add(a, "[a]"); }
    } else {
        /* The removed part touches an end: what is left is one piece,
         * and a plain precise trim makes it. */
        if (head) add_time(a, "-to", start);
        else if (tail) add_time(a, "-ss", end);
        add_input(a, in);
        if (v) {
            char *ev = even_filter(mi);
            if (ev) { add(a, "-vf"); add(a, ev); g_free(ev); }
            add(a, "-map"); add(a, "0:v:0");
            if (au) { add(a, "-map"); add(a, "0:a:0"); }
        } else {
            add(a, "-vn");
        }
    }
    if (v) { add_h264(a, "20"); if (au) add_aac(a, "160k"); }
    else add_audio_codec_for(a, out);
}

GPtrArray *edit_build_argv(edit_op_t op, const char *in, const char *out,
                           const media_info_t *mi,
                           double start, double end, double pos)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    add(a, "ffmpeg");
    add(a, "-hide_banner");
    add(a, "-nostdin");
    add(a, "-y");
    /* Only errors on stderr: that is what the failure banner shows, and
     * a page of codec chatter would bury the one line that matters. */
    add(a, "-loglevel"); add(a, "error");
    add(a, "-progress"); add(a, "pipe:1");
    add(a, "-nostats");

    char *ev = NULL;
    switch (op) {
    case OP_TRIM_FAST:
        add_range_in(a, mi, start, end);
        add_input(a, in);
        add(a, "-c"); add(a, "copy");
        add(a, "-avoid_negative_ts"); add(a, "make_zero");
        break;

    case OP_TRIM_PRECISE:
        add_range_in(a, mi, start, end);
        add_input(a, in);
        if (mi->has_video) {
            if ((ev = even_filter(mi))) { add(a, "-vf"); add(a, ev); }
            add(a, "-map"); add(a, "0:v:0");
            if (mi->has_audio) { add(a, "-map"); add(a, "0:a:0"); }
            add_h264(a, "20");
            if (mi->has_audio) add_aac(a, "160k");
        } else {
            add(a, "-vn");
            add_audio_codec_for(a, out);
        }
        break;

    case OP_CUT:
        build_cut(a, in, out, mi, start, end);
        break;

    case OP_AUDIO_MP3:
    case OP_AUDIO_M4A:
        add_range_in(a, mi, start, end);
        add_input(a, in);
        add(a, "-vn");
        add(a, "-map"); add(a, "0:a:0");
        add_audio_codec_for(a, out);
        break;

    case OP_FRAME:
        add_time(a, "-ss", pos);
        add_input(a, in);
        add(a, "-map"); add(a, "0:v:0");
        add(a, "-frames:v"); add(a, "1");
        add(a, "-update"); add(a, "1");
        break;

    case OP_GIF_480:
    case OP_GIF_720: {
        add_range_in(a, mi, start, end);
        add_input(a, in);
        /* The palette is made from this clip's own colours; one global
         * 256-colour palette is what makes naive GIFs look posterised.
         * min(...,iw) so a small video is not blown up. */
        int w = op == OP_GIF_480 ? 480 : 720;
        char *vf = g_strdup_printf(
            "fps=12,scale='min(%d,iw)':-2:flags=lanczos,split[g0][g1];"
            "[g0]palettegen=stats_mode=diff[gp];"
            "[g1][gp]paletteuse=dither=bayer:bayer_scale=5", w);
        add(a, "-vf"); add(a, vf);
        g_free(vf);
        add(a, "-an");
        add(a, "-loop"); add(a, "0");
        break;
    }

    case OP_ROT_LEFT:
    case OP_ROT_RIGHT: {
        add_input(a, in);
        /* transpose=1 turns clockwise, 2 counter-clockwise. The size
         * swaps, so an odd height would become an odd width. */
        const char *tr = op == OP_ROT_LEFT ? "transpose=2" : "transpose=1";
        ev = even_filter(mi);
        char *vf = ev ? g_strdup_printf("%s,%s", tr, ev) : g_strdup(tr);
        add(a, "-vf"); add(a, vf);
        g_free(vf);
        add(a, "-map"); add(a, "0:v:0");
        if (mi->has_audio) { add(a, "-map"); add(a, "0:a:0"); }
        add_h264(a, "20");
        if (mi->has_audio) add_aac(a, "160k");
        break;
    }

    case OP_NO_AUDIO:
        add_input(a, in);
        add(a, "-map"); add(a, "0:v:0");
        add(a, "-c:v"); add(a, "copy");
        add(a, "-an");
        break;

    case OP_SIZE_1080:
    case OP_SIZE_720:
    case OP_SIZE_480: {
        add_input(a, in);
        int p = op == OP_SIZE_1080 ? 1080 : op == OP_SIZE_720 ? 720 : 480;
        /* "720p" means the short side. A portrait phone video is
         * 720 wide, not 720 tall. Never enlarge. */
        char *vf = (mi->width && mi->height && mi->height > mi->width)
            ? g_strdup_printf("scale='min(%d,iw)':-2", p)
            : g_strdup_printf("scale=-2:'min(%d,ih)'", p);
        add(a, "-vf"); add(a, vf);
        g_free(vf);
        add(a, "-map"); add(a, "0:v:0");
        if (mi->has_audio) { add(a, "-map"); add(a, "0:a:0"); }
        add_h264(a, "23");
        if (mi->has_audio) add_aac(a, "128k");
        break;
    }

    default:
        break;
    }
    g_free(ev);

    add_mp4_tail(a, out);
    g_ptr_array_add(a, g_strconcat("file:", out, NULL));
    g_ptr_array_add(a, NULL);
    return a;
}

double edit_expected_length(edit_op_t op, const media_info_t *mi,
                            double start, double end)
{
    if (op == OP_FRAME) return 0;
    if (op == OP_CUT) {
        double d = mi->duration - (end - start);
        return d > 0 ? d : 0;
    }
    if (edit_op_ranged(op)) return end > start ? end - start : 0;
    return mi->duration;
}

/* ── self-test ─────────────────────────────────────────────────── */

typedef struct {
    edit_op_t   op;
    const char *what;
    double      want_len;   /* < 0: do not check */
    double      tol;
    int         want_v;     /* -1 any, 0 none, 1 present */
    int         want_a;
    int         want_w, want_h;  /* 0 = do not check */
} case_t;

static gboolean run_argv(GPtrArray *a, char **err)
{
    int status = 0;
    char *e = NULL;
    gboolean ok = g_spawn_sync(NULL, (char **)a->pdata, NULL,
                               G_SPAWN_SEARCH_PATH |
                               G_SPAWN_STDOUT_TO_DEV_NULL,
                               NULL, NULL, NULL, &e, &status, NULL)
                  && g_spawn_check_wait_status(status, NULL);
    if (err) *err = e; else g_free(e);
    return ok;
}

int edit_selftest(const char *path)
{
    media_info_t mi;
    if (!media_probe_sync(path, &mi)) {
        printf("FAIL probe %s\n", path);
        return 1;
    }
    printf("input %s: %.3f s, video=%d (%s %dx%d) audio=%d (%s)\n", path,
           mi.duration, mi.has_video, mi.vcodec, mi.width, mi.height,
           mi.has_audio, mi.acodec);

    double S = mi.duration * 0.25, E = mi.duration * 0.5, L = E - S;
    double P = mi.duration * 0.4;
    int W = mi.width, H = mi.height;
    int A = mi.has_audio ? 1 : 0;

    const case_t cases[] = {
        /* Fast trim snaps to the key frame before S, so it may be
         * longer by up to a GOP; only a lower bound matters. */
        { OP_TRIM_FAST,    "trim fast",    L, -1, -1, A, 0, 0 },
        { OP_TRIM_PRECISE, "trim precise", L, 0.25, -1, A, 0, 0 },
        { OP_CUT,          "remove part",  mi.duration - L, 0.3, -1, A, 0, 0 },
        { OP_AUDIO_MP3,    "audio mp3",    L, 0.3, 0, 1, 0, 0 },
        { OP_AUDIO_M4A,    "audio m4a",    L, 0.3, 0, 1, 0, 0 },
        { OP_FRAME,        "frame png",    -1, 0, 1, 0, W, H },
        { OP_GIF_480,      "gif 480",      L, 0.3, 1, 0, W < 480 ? W : 480, 0 },
        { OP_GIF_720,      "gif 720",      L, 0.3, 1, 0, W < 720 ? W : 720, 0 },
        { OP_ROT_LEFT,     "rotate left",  mi.duration, 0.3, 1, A, H, W },
        { OP_ROT_RIGHT,    "rotate right", mi.duration, 0.3, 1, A, H, W },
        { OP_NO_AUDIO,     "remove audio", mi.duration, 0.3, 1, 0, W, H },
        { OP_SIZE_1080,    "size 1080p",   mi.duration, 0.3, 1, A, 0, H < 1080 ? H : 1080 },
        { OP_SIZE_720,     "size 720p",    mi.duration, 0.3, 1, A, 0, H < 720 ? H : 720 },
        { OP_SIZE_480,     "size 480p",    mi.duration, 0.3, 1, A, 0, H < 480 ? H : 480 },
    };

    int fails = 0, runs = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(cases); i++) {
        const case_t *c = &cases[i];
        if (!edit_op_available(c->op, &mi)) {
            printf("skip %-13s (not applicable)\n", c->what);
            continue;
        }
        runs++;
        char *out = edit_default_output(c->op, path, &mi);
        GPtrArray *a = edit_build_argv(c->op, path, out, &mi, S, E, P);
        char *cmd = g_strjoinv(" ", (char **)a->pdata);
        char *err = NULL;
        gint64 t0 = g_get_monotonic_time();
        gboolean ok = run_argv(a, &err);
        double secs = (g_get_monotonic_time() - t0) / 1e6;

        media_info_t r;
        GStatBuf st;
        gboolean have = g_stat(out, &st) == 0 && st.st_size > 0;
        gboolean probed = have && media_probe_sync(out, &r);
        if (!probed) memset(&r, 0, sizeof r);

        GString *why = g_string_new(NULL);
        if (!ok) g_string_append(why, " ffmpeg-failed");
        if (!have) g_string_append(why, " no-output");
        if (have && !probed) g_string_append(why, " unprobeable");
        if (probed) {
            if (c->want_len >= 0) {
                if (c->tol < 0 ? r.duration < c->want_len - 0.2
                               : fabs(r.duration - c->want_len) > c->tol)
                    g_string_append_printf(why, " duration=%.3f(want %.3f)",
                                           r.duration, c->want_len);
            }
            if (c->want_v >= 0 && r.has_video != c->want_v)
                g_string_append_printf(why, " video=%d", r.has_video);
            if (c->want_a >= 0 && r.has_audio != c->want_a)
                g_string_append_printf(why, " audio=%d", r.has_audio);
            if (c->want_w && r.width != c->want_w)
                g_string_append_printf(why, " width=%d(want %d)",
                                       r.width, c->want_w);
            if (c->want_h && r.height != c->want_h)
                g_string_append_printf(why, " height=%d(want %d)",
                                       r.height, c->want_h);
        }
        gboolean pass = why->len == 0;
        if (!pass) fails++;
        printf("%s %-13s %6.2fs  -> %s\n"
               "     result: %.3f s, v=%d %s %dx%d, a=%d %s, %lld bytes%s\n",
               pass ? "PASS" : "FAIL", c->what, secs, out,
               r.duration, r.has_video, r.vcodec, r.width, r.height,
               r.has_audio, r.acodec, have ? (long long)st.st_size : 0LL,
               why->str);
        if (!pass) {
            printf("     cmd: %s\n", cmd);
            if (err && *err) printf("     stderr: %s\n", err);
        }
        g_string_free(why, TRUE);
        g_free(err); g_free(cmd); g_free(out);
        g_ptr_array_unref(a);
    }

    /* The naming rule: name-trim.<ext> is taken by now, so the next
     * proposal must be a free name-trim-N.<ext>. */
    char *n1 = edit_default_output(OP_TRIM_FAST, path, &mi);
    gboolean named = strstr(n1, "-trim-") != NULL &&
                     !g_file_test(n1, G_FILE_TEST_EXISTS);
    printf("%s next free name -> %s\n", named ? "PASS" : "FAIL", n1);
    if (!named) fails++;
    g_free(n1);

    printf("%s: %d run, %d failed\n", fails ? "FAILED" : "ALL PASSED",
           runs, fails);
    return fails ? 1 : 0;
}
