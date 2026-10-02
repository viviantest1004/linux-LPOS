/*
 * lp-json.h - just enough JSON to read `lp-net status --json` and
 * `lp-tune status --json`.
 *
 * json-glib is not in the Debian base this is built against, and those
 * two commands are the only JSON the shell ever reads: small objects,
 * written by programs in this tree, a few hundred bytes each. A parser
 * for that is a page of C; a new library dependency is a package the
 * image has to carry and keep patched.
 *
 * Lookups take a dotted path - "wifi.ssid", "battery.percent" - and a
 * default, so that a field a newer lp-net adds or an older one lacks is
 * never a crash, only the default.
 */
#ifndef LP_JSON_H
#define LP_JSON_H

typedef struct LpJson LpJson;

LpJson *lp_json_parse(const char *text);     /* NULL if malformed */
void lp_json_free(LpJson *j);

LpJson *lp_json_get(LpJson *j, const char *path);
const char *lp_json_str(LpJson *j, const char *path, const char *def);
double lp_json_num(LpJson *j, const char *path, double def);
int lp_json_bool(LpJson *j, const char *path, int def);

/* Arrays. */
int lp_json_len(LpJson *j);
LpJson *lp_json_at(LpJson *j, int i);

#endif /* LP_JSON_H */
