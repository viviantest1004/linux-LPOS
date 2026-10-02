/*
 * lp-json.c - a recursive-descent JSON reader, sized for status output.
 *
 * It accepts the whole grammar (objects, arrays, strings with escapes,
 * numbers, true/false/null) because a status line containing an SSID
 * with a quote in it must not be the day the top bar goes blank. \u
 * escapes are decoded to UTF-8, surrogate pairs included - an SSID is
 * 32 arbitrary bytes and people do put emoji in them.
 */
#include "lp-json.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct LpJson {
    int type;
    char *key;          /* when this is a member of an object */
    char *str;
    double num;
    GPtrArray *kids;    /* array elements or object members */
};

typedef struct { const char *p; } Cur;

static LpJson *parse_value(Cur *c, int depth);

static void ws(Cur *c)
{
    while (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')
        c->p++;
}

static LpJson *node(int type)
{
    LpJson *j = g_new0(LpJson, 1);
    j->type = type;
    return j;
}

void lp_json_free(LpJson *j)
{
    if (!j)
        return;
    if (j->kids)
        g_ptr_array_free(j->kids, TRUE);
    g_free(j->key);
    g_free(j->str);
    g_free(j);
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int d = g_ascii_xdigit_value(p[i]);
        if (d < 0)
            return 0;
        v = v * 16 + (unsigned)d;
    }
    *out = v;
    return 1;
}

static char *parse_string(Cur *c)
{
    if (*c->p != '"')
        return NULL;
    c->p++;
    GString *s = g_string_new(NULL);
    while (*c->p && *c->p != '"') {
        if (*c->p != '\\') {
            g_string_append_c(s, *c->p++);
            continue;
        }
        c->p++;
        switch (*c->p) {
        case 'n': g_string_append_c(s, '\n'); break;
        case 't': g_string_append_c(s, '\t'); break;
        case 'r': g_string_append_c(s, '\r'); break;
        case 'b': g_string_append_c(s, '\b'); break;
        case 'f': g_string_append_c(s, '\f'); break;
        case 'u': {
            unsigned u;
            if (!hex4(c->p + 1, &u))
                goto bad;
            c->p += 4;
            if (u >= 0xd800 && u < 0xdc00 && c->p[1] == '\\' && c->p[2] == 'u') {
                unsigned lo;
                if (hex4(c->p + 3, &lo) && lo >= 0xdc00 && lo < 0xe000) {
                    u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                    c->p += 6;
                }
            }
            g_string_append_unichar(s, (gunichar)u);
            break;
        }
        case '\0':
            goto bad;
        default:
            g_string_append_c(s, *c->p);
        }
        c->p++;
    }
    if (*c->p != '"')
        goto bad;
    c->p++;
    return g_string_free(s, FALSE);
bad:
    g_string_free(s, TRUE);
    return NULL;
}

static LpJson *parse_value(Cur *c, int depth)
{
    if (depth > 32)
        return NULL;
    ws(c);
    if (*c->p == '{' || *c->p == '[') {
        int obj = *c->p == '{';
        char close = obj ? '}' : ']';
        LpJson *j = node(obj ? J_OBJ : J_ARR);
        j->kids = g_ptr_array_new_with_free_func((GDestroyNotify)lp_json_free);
        c->p++;
        ws(c);
        if (*c->p == close) {
            c->p++;
            return j;
        }
        for (;;) {
            char *key = NULL;
            ws(c);
            if (obj) {
                key = parse_string(c);
                ws(c);
                if (!key || *c->p != ':') {
                    g_free(key);
                    lp_json_free(j);
                    return NULL;
                }
                c->p++;
            }
            LpJson *v = parse_value(c, depth + 1);
            if (!v) {
                g_free(key);
                lp_json_free(j);
                return NULL;
            }
            v->key = key;
            g_ptr_array_add(j->kids, v);
            ws(c);
            if (*c->p == ',') {
                c->p++;
                continue;
            }
            if (*c->p == close) {
                c->p++;
                return j;
            }
            lp_json_free(j);
            return NULL;
        }
    }
    if (*c->p == '"') {
        char *s = parse_string(c);
        if (!s)
            return NULL;
        LpJson *j = node(J_STR);
        j->str = s;
        return j;
    }
    if (strncmp(c->p, "true", 4) == 0 || strncmp(c->p, "false", 5) == 0) {
        LpJson *j = node(J_BOOL);
        j->num = *c->p == 't';
        c->p += j->num ? 4 : 5;
        return j;
    }
    if (strncmp(c->p, "null", 4) == 0) {
        c->p += 4;
        return node(J_NULL);
    }
    char *end = NULL;
    double d = g_ascii_strtod(c->p, &end);
    if (end == c->p)
        return NULL;
    c->p = end;
    LpJson *j = node(J_NUM);
    j->num = d;
    return j;
}

LpJson *lp_json_parse(const char *text)
{
    if (!text)
        return NULL;
    Cur c = { text };
    LpJson *j = parse_value(&c, 0);
    ws(&c);
    if (j && *c.p) {
        lp_json_free(j);
        return NULL;
    }
    return j;
}

LpJson *lp_json_get(LpJson *j, const char *path)
{
    if (!j || !path || !*path)
        return j;
    const char *dot = strchr(path, '.');
    size_t n = dot ? (size_t)(dot - path) : strlen(path);
    if (j->type != J_OBJ)
        return NULL;
    for (guint i = 0; i < j->kids->len; i++) {
        LpJson *k = g_ptr_array_index(j->kids, i);
        if (strlen(k->key) == n && strncmp(k->key, path, n) == 0)
            return dot ? lp_json_get(k, dot + 1) : k;
    }
    return NULL;
}

const char *lp_json_str(LpJson *j, const char *path, const char *def)
{
    LpJson *k = lp_json_get(j, path);
    return (k && k->type == J_STR) ? k->str : def;
}

double lp_json_num(LpJson *j, const char *path, double def)
{
    LpJson *k = lp_json_get(j, path);
    return (k && (k->type == J_NUM || k->type == J_BOOL)) ? k->num : def;
}

int lp_json_bool(LpJson *j, const char *path, int def)
{
    LpJson *k = lp_json_get(j, path);
    return (k && (k->type == J_BOOL || k->type == J_NUM)) ? k->num != 0 : def;
}

int lp_json_len(LpJson *j)
{
    return (j && j->type == J_ARR) ? (int)j->kids->len : 0;
}

LpJson *lp_json_at(LpJson *j, int i)
{
    if (!j || j->type != J_ARR || i < 0 || i >= (int)j->kids->len)
        return NULL;
    return g_ptr_array_index(j->kids, i);
}
