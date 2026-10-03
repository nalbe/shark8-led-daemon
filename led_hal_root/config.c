/*
 * config.c - runtime INI config for the LED policies.
 *
 * Reads /data/adb/modules/led_hal_root/led.conf at runtime.
 *
 * LINE SYNTAX: '#' or ';' opens a comment - a line of its own or right
 * after a setting. A value whose last character is a comma continues on
 * the next line (a trailing backslash does the same), so a long preset
 * can be spread over several lines with a comment on every one of them;
 * the trimmed fragments are joined and parsed as a single line.
 *
 * SECTIONS:
 *   [suppress]  one package per line - never lights the LED
 *   [rules]     pkg=r,g,b,cap,<preset fields>  (0-255 per channel). The
 *               optional comma tail carries that app's OWN notify preset
 *               (cap + preset line fields) and is synthesized into a
 *               [notify.<pkg>] section at load. Without the tail the app
 *               uses the shared [notify] preset.
 *   [charge]    first_threshold / second_threshold (%) ONLY - each band
 *               owns its own section: [charge.lower|middle|upper] carries
 *               its own render= line, colour included
 *   [notify]    behavior for apps WITHOUT a [rules] entry:
 *               max_sec (0 = unlimited), notify_screen_delay_ms and
 *               the render= line whose colour triple IS the shared default
 *               colour
 *   [ring] / [voip]      call rainbows: max_sec + render
 *   [missed] / [alarm]    tombstone / clock: max_sec + render
 *   [priority]  channel ranking, one key per effect (ring, voip, alarm,
 *               missed, notify, charge), BIGGER WINS. Read by channel.c
 *               through the generic kv table. Every key is required:
 *               an effect with no rank cannot be arbitrated and
 *               is dropped (logged) instead.
 *   [led]       chip/daemon globals only: logging, imax
 *
 * THE PRESET LINE - one key, render=, carries a whole renderer:
 *   render=<r,g,b>,<mode>,<cur_r,g,b>,pattern,<sync>,<repeat>,
 *          <cur_r,g,b>,<rise>,<hold>,<fall>,<offt>,<t0_r,g,b>
 *          colour triple + 17 fields = 20 comma tokens
 * A [rules] tail is those same 17 fields after its own cap, without the
 * colour - the rules line already carried it. The budget is never in the
 * line: max_sec stays a named key the owning mod reads.
 * render_synth() expands the line into the keys led.c reads ([sec] mode,
 * [sec.solid] cur, [sec.pattern] the rest), so led.c and the mods only
 * ever see one flat key per field.
 *
 * Every other key=value pair anywhere in the file lands in a generic
 * key-value table (conf_get_str / conf_get_int). That is how mods own
 * their config: a mod (e.g. mods/ring.c) reads its own [ring] section
 * by name without config.c knowing the key exists at all.
 *
 * The file is reloaded lazily: every lookup calls conf_maybe_reload(),
 * which normally just consumes an inotify flag set by the core when the
 * directory holding led.conf changed (see conf_watch_init/conf_watch_handle).
 * A stat()-based probe covers the case when inotify is unavailable.
 *
 * Registry merge rules:
 *   - suppress list = file [suppress] entries ONLY (runtime)
 *   - rules         = file [rules] entries ONLY
 *   - Every colour, threshold, cap and window this daemon paints is
 *     read from the file. A key the file does not define has no value
 *     to invent: the read fails, the caller drops the event,
 *     and conf_req_* plus the log name the key that is missing.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include "chgd.h"

#define CONF_PATH "/data/adb/modules/led_hal_root/led.conf"
#define CONF_DIR  "/data/adb/modules/led_hal_root"
#define MAX_SUPP  96
#define MAX_RULES 96

/* generic key-value store for mod-owned sections (e.g. [ring],
 * [charge.lower], ...) plus the chip sections render_synth() builds out of
 * a preset line ([<sec>.solid], [<sec>.pattern]) and the synthetic
 * per-rule [notify.<pkg>] presets. sec holds long section names
 * ("notify.<very.long.pkg>.pattern"), val holds long [rules] values. */
#define MAX_KV   2560
struct kv { char sec[128]; char key[32]; char val[192]; };

/* Absence markers: -1 (or the g_ntf_set flag) means "the file did not
 * define the key", and every reader turns that into a drop. */
#define ABSENT (-1)

static char    g_supp[MAX_SUPP][96];
static int     g_nsupp = 0;
static struct rule { char pkg[96]; int r, g, b; } g_rules[MAX_RULES];
static int     g_nrules = 0;
static struct kv g_kv[MAX_KV];
static int     g_nkv = 0;

static int  g_first_at = ABSENT;
static int  g_second_at = ABSENT;
static int  g_ntf_r = 0, g_ntf_g = 0, g_ntf_b = 0;
static int  g_ntf_set = 0;

static time_t g_last_mtime = 0;

/* Bumped every time the file-owned state is emptied (reset_dynamic), so a
 * cache built from the old file knows it is stale. The pool stamps the
 * ranks and budgets it caches with this value. */
static unsigned g_generation = 0;

unsigned conf_generation(void)
{
    return g_generation;
}

/* [led] logging: runtime switch for the on-disk log. Applied to the
 * logger via log_set_enabled() on every config (re)load so the GUI can
 * flip it live (write led.conf, SIGALRM). Defaults to on. */
static int g_logging = 1;

/* [rules] tails that could not be turned into a full preset. Counted
 * (not just logged) so the "conf: loaded" summary can report a non-zero
 * number the moment a GUI/daemon version mismatch is in play. */
static int g_nbroken = 0;

/* Required-key warnings are logged once per (sec,key), not once per
 * event. Cleared on every reload so a key that is fixed and then
 * broken again speaks up again. Single-threaded, tiny table. */
#define MAX_REQWARN 24
static char g_reqwarn[MAX_REQWARN][160];
static int  g_nreqwarn = 0;

/* ---------------- helpers ---------------- */

static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1])) s[--l] = '\0';
}

/* Cut a physical line at the first comment marker: a comment may follow a
 * setting on the very same line, not only occupy a line of its own. */
static void strip_comment(char *s)
{
    for (char *p = s; *p; p++)
        if (*p == '#' || *p == ';') { *p = '\0'; return; }
}

/* A value that ends in a comma continues on the next line, so a preset can
 * be spread out with a comment on every one of its lines; a trailing
 * backslash says the same thing and is cut off here. */
static int cut_continuation(char *s)
{
    trim(s);
    size_t l = strlen(s);
    if (!l) return 0;
    if (s[l - 1] == '\\') { s[l - 1] = '\0'; trim(s); return 1; }
    return s[l - 1] == ',';
}

/* append one fragment to the logical line being assembled; 0 = it no longer
 * fits, and the whole logical line is dropped */
static int line_append(char *dst, size_t cap, const char *src)
{
    size_t have = strlen(dst), add = strlen(src);
    if (have + add + 1 > cap) return 0;
    memcpy(dst + have, src, add + 1);
    return 1;
}

static int in_supp(const char *pkg)
{
    for (int i = 0; i < g_nsupp; i++)
        if (!strcmp(g_supp[i], pkg)) return 1;
    return 0;
}

static int rule_rgb(const char *pkg, int *r, int *g, int *b)
{
    for (int i = 0; i < g_nrules; i++)
        if (!strcmp(g_rules[i].pkg, pkg)) {
            *r = g_rules[i].r; *g = g_rules[i].g; *b = g_rules[i].b;
            return 1;
        }
    return 0;
}

/* parse "r,g,b" with 0-255 per channel; returns 1 on success */
static int parse_rgb(const char *val, int *r, int *g, int *b)
{
    int x = 0, y = 0, z = 0;
    if (sscanf(val, "%d , %d , %d", &x, &y, &z) != 3) return 0;
    if (x < 0 || x > 255 || y < 0 || y > 255 || z < 0 || z > 255) return 0;
    *r = x; *g = y; *b = z;
    return 1;
}

/* parse a whole-string decimal int; returns 1 on success. strtol alone is
 * not enough: it happily reads "90%" or "12abc" and the daemon would
 * paint a value the file never actually said. */
static int parse_int(const char *val, long *out)
{
    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (end == val) return 0;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end) return 0;
    *out = v;
    return 1;
}

/* ---------------- reset to absence ---------------- */

/* Empty every file-owned table and put every owned value back into the
 * "the file did not define this" state. */
static void reset_dynamic(void)
{
    g_generation++;
    g_nsupp = 0;
    g_nrules = 0;
    g_nkv = 0;
    g_nbroken = 0;
    g_nreqwarn = 0;
    g_first_at = ABSENT;
    g_second_at = ABSENT;
    g_ntf_r = 0; g_ntf_g = 0; g_ntf_b = 0;
    g_ntf_set = 0;
    g_logging = 1;
    log_set_enabled(1);
}

/* put/update one (sec,key)=val into the generic table */
static void kv_put(const char *sec, const char *key, const char *val)
{
    if (!sec[0] || !key[0]) return;
    for (int i = 0; i < g_nkv; i++) {
        if (!strcmp(g_kv[i].sec, sec) && !strcmp(g_kv[i].key, key)) {
            snprintf(g_kv[i].val, sizeof(g_kv[i].val), "%s", val);
            return;
        }
    }
    if (g_nkv >= MAX_KV) {
        LOGI("warn: [conf] kv table full (%d), dropped [%s] %s", MAX_KV, sec, key);
        return;
    }
    snprintf(g_kv[g_nkv].sec, sizeof(g_kv[g_nkv].sec), "%s", sec);
    snprintf(g_kv[g_nkv].key, sizeof(g_kv[g_nkv].key), "%s", key);
    snprintf(g_kv[g_nkv].val, sizeof(g_kv[g_nkv].val), "%s", val);
    g_nkv++;
}

/* internal kv-table probe (no reload guard) */
static int kv_has_sec(const char *sec)
{
    for (int i = 0; i < g_nkv; i++)
        if (!strcmp(g_kv[i].sec, sec)) return 1;
    return 0;
}

/* drop every entry of a section - used before re-synthesizing an
 * overridden [rules] rule so a shorter tail can't leak old values */
static void kv_clear_sec(const char *sec)
{
    for (int i = 0; i < g_nkv; i++) {
        if (!strcmp(g_kv[i].sec, sec)) {
            g_kv[i] = g_kv[--g_nkv];
            i--;
        }
    }
}

/* numeric kv setter for the synthesized preset */
static void kvi(const char *sec, const char *key, long v)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", v);
    kv_put(sec, key, buf);
}

/* triple kv setter for the synthesized preset */
static void kvt(const char *sec, const char *key, const long v[3])
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%ld,%ld,%ld", v[0], v[1], v[2]);
    kv_put(sec, key, buf);
}

/* ---------------- the preset line ----------------
 *
 * render=<r,g,b>,<mode>,<cur_r,g,b>,pattern,<sync>,<repeat>,
 *        <cur_r,g,b>,<rise>,<hold>,<fall>,<offt>,<t0_r,g,b>
 *
 * A [rules] tail is the same line after its own cap, minus the colour -
 * the rules line already carries it. Both are read here and expanded into
 * the flat keys led.c asks for, so there is exactly one parser for the
 * whole renderer vocabulary no matter which kind of line it arrived in. */

static const char *const g_rfield[] = {
    "mode", "cur", "cur", "cur", "pattern", "sync", "repeat",
    "cur_r", "cur_g", "cur_b", "rise", "hold", "fall", "offt",
    "t0_r", "t0_g", "t0_b"
};
#define RN_FIELDS ((int)(sizeof(g_rfield) / sizeof(g_rfield[0])))

/* one token of a preset line, cursor advanced past it. The cursor is only
 * advanced when the token really is there, so a short line stops on the
 * first field it does not have instead of reading past the end. */
static int rtok(const char *where, const char **p, const char *field,
                char *buf, size_t n)
{
    const char *s = *p;
    if (!s) {
        LOGI("warn: [conf] %s: line ends before %s", where, field);
        return 0;
    }
    const char *end = strchr(s, ',');
    size_t len = end ? (size_t)(end - s) : strlen(s);
    if (len >= n) len = n - 1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    trim(buf);
    *p = end ? end + 1 : NULL;
    return 1;
}

/* a numeric preset field: strtol alone would read "500ms" as 500 and paint
 * a timing the file never said. */
static int rnum(const char *where, const char **p, const char *field, long *out)
{
    char buf[32];
    if (!rtok(where, p, field, buf, sizeof(buf))) return 0;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf || *end) {
        LOGI("warn: [conf] %s: %s=\"%s\" is not a number", where, field, buf);
        return 0;
    }
    *out = v;
    return 1;
}

/* a literal preset field ("pattern") */
static int rword(const char *where, const char **p, const char *field,
                 const char *want)
{
    char buf[16];
    if (!rtok(where, p, field, buf, sizeof(buf))) return 0;
    if (strcmp(buf, want)) {
        LOGI("warn: [conf] %s: %s=\"%s\", want \"%s\"", where, field, buf, want);
        return 0;
    }
    return 1;
}

/* split "r,g,b,<rest>" into the colour triple and a cursor at <rest> */
static int split_color(const char *val, char col[64], const char **tail)
{
    const char *p = val;
    for (int i = 0; i < 3; i++) {
        const char *c = strchr(p, ',');
        if (!c) return 0;
        p = c + 1;
        if (i == 2) *tail = p;
    }
    size_t n = (size_t)(p - val) - 1;
    if (n >= 64) n = 63;
    memcpy(col, val, n);
    col[n] = '\0';
    return 1;
}

/* Expand one preset line into [sec] mode, [sec.solid] cur and
 * [sec.pattern] the timing. The cursor starts at the first field AFTER the
 * colour and must be empty at the end: all 17 fields or nothing, so a
 * broken line leaves the section empty and the event dark rather than
 * painting half a preset. Values are stored verbatim - their legal ranges
 * are led.c's business, which reports them by key like any other. */
static int render_synth(const char *where, const char *sec, const char **p)
{
    char s[sizeof(g_kv[0].sec)];
    char mode[8] = "";
    long sync, rep, scur[3], pcur[3], tt[4], pt0[3];

    {
        char buf[16];
        if (!rtok(where, p, g_rfield[0], buf, sizeof(buf))) return 0;
        if (strcmp(buf, "off") && strcmp(buf, "solid") &&
            strcmp(buf, "breath") && strcmp(buf, "wave")) {
            LOGI("warn: [conf] %s: mode=\"%s\", want off|solid|breath|wave",
                 where, buf);
            return 0;
        }
        snprintf(mode, sizeof(mode), "%s", buf);
    }
    for (int i = 0; i < 3; i++)
        if (!rnum(where, p, g_rfield[1 + i], &scur[i])) return 0;
    if (!rword(where, p, g_rfield[4], "pattern")) return 0;
    if (!rnum(where, p, g_rfield[5], &sync)) return 0;
    if (!rnum(where, p, g_rfield[6], &rep)) return 0;
    for (int i = 0; i < 3; i++)
        if (!rnum(where, p, g_rfield[7 + i], &pcur[i])) return 0;
    for (int i = 0; i < 4; i++)
        if (!rnum(where, p, g_rfield[10 + i], &tt[i])) return 0;
    for (int i = 0; i < 3; i++)
        if (!rnum(where, p, g_rfield[14 + i], &pt0[i])) return 0;
    if (*p) {
        LOGI("warn: [conf] %s: %s after the %d preset fields", where,
             **p ? "trailing text" : "empty field", RN_FIELDS);
        return 0;
    }

    kv_put(sec, "mode", mode);
    snprintf(s, sizeof(s), "%s.solid", sec);
    kvt(s, "cur", scur);
    snprintf(s, sizeof(s), "%s.pattern", sec);
    kvi(s, "sync", sync);
    kvi(s, "repeat", rep);
    kvi(s, "cur_r", pcur[0]);
    kvi(s, "cur_g", pcur[1]);
    kvi(s, "cur_b", pcur[2]);
    kvi(s, "rise", tt[0]);
    kvi(s, "hold", tt[1]);
    kvi(s, "fall", tt[2]);
    kvi(s, "offt", tt[3]);
    kvt(s, "t0", pt0);
    return 1;
}

/* The render= line of a section that owns a renderer: the colour triple
 * becomes the colour that section paints in (and, for the shared [notify],
 * the colour every app without a [rules] entry gets), the 17 fields after
 * it become the chip keys. */
static void preset_line(const char *sec, const char *val)
{
    char where[160], col[64];
    const char *tail = NULL;
    int r, g, b;
    snprintf(where, sizeof(where), "[%s] render", sec);
    if (!split_color(val, col, &tail)) {
        LOGI("warn: [conf] %s: want <r,g,b> then %d preset fields, got: \"%s\"",
             where, RN_FIELDS, val);
        return;
    }
    if (!parse_rgb(col, &r, &g, &b)) {
        LOGI("warn: [conf] %s: colour \"%s\" is not r,g,b in 0..255", where, col);
        return;
    }
    kv_put(sec, "color", col);
    if (!strcmp(sec, "notify")) {
        g_ntf_r = r; g_ntf_g = g; g_ntf_b = b;
        g_ntf_set = 1;
    }
    render_synth(where, sec, &tail);
}

/* An extended [rules] line carries that app's OWN notify preset: the cap
 * token, then the 17 preset fields (its colour is the first three tokens
 * of the rules line, already parsed). The whole thing is synthesized into
 * a [notify.<pkg>] section, so notify.c reads it like any other preset.
 * A tail that does not rebuild completely is dropped as a whole: the rule
 * keeps its colour and runs against the shared [notify] preset instead of
 * painting from a half-read line. */
static void rule_synth(const char *pkg, const char *tail)
{
    char where[160], b[sizeof(g_kv[0].sec)], s[sizeof(g_kv[0].sec)];
    const char *p = tail;
    long cap;

    snprintf(where, sizeof(where), "rule %s", pkg);
    snprintf(b, sizeof(b), "notify.%s", pkg);

    /* a second line for the same package must not inherit the presets of
     * the first one, so every section this preset owns starts empty */
    kv_clear_sec(b);
    snprintf(s, sizeof(s), "%s.solid", b);    kv_clear_sec(s);
    snprintf(s, sizeof(s), "%s.pattern", b);  kv_clear_sec(s);

    if (!rnum(where, &p, "max_sec", &cap) || !render_synth(where, b, &p)) {
        LOGI("warn: [conf] %s: preset dropped, the rule runs colour-only "
             "against [notify]", where);
        g_nbroken++;
        return;
    }
    kvi(b, "max_sec", cap);
}

/* parse one key=value line into section (built-in sections only);
 * everything else already lives in the generic kv table */
static void parse_value(const char *sec, const char *key, const char *val)
{
    int r, g, b;
    long v;
    if (!strcmp(sec, "charge")) {
        int *slot = NULL;
        if      (!strcmp(key, "first_threshold"))  slot = &g_first_at;
        else if (!strcmp(key, "second_threshold")) slot = &g_second_at;
        if (slot) {
            if (parse_int(val, &v) && v >= 1 && v <= 100) {
                *slot = (int)v;
            } else {
                LOGI("warn: [conf] bad [charge] %s=%s (want 1..100) - "
                     "charge stays off until it is fixed", key, val);
            }
        }
        return;
    }
    if (!strcmp(sec, "rules")) {
        /* extended line  pkg=r,g,b,cap,<17 preset fields>: the colour is
         * the FIRST THREE comma tokens; the optional preset tail is
         * everything after the 3rd comma. The triple is copied out for
         * parse_rgb, the tail is absorbed into the synthetic
         * [notify.<pkg>] section. */
        const char *orig = val;
        const char *tail = NULL;
        char col[64];
        if (split_color(val, col, &tail)) val = col;
        if (!parse_rgb(val, &r, &g, &b)) {
            LOGI("conf: bad [rules] %s=%s (want r,g,b first)", key, orig);
            return;
        }
        /* file wins over builtin for the same package */
        int present = 0;
        for (int i = 0; i < g_nrules; i++) {
            if (!strcmp(g_rules[i].pkg, key)) {
                g_rules[i].r = r; g_rules[i].g = g; g_rules[i].b = b;
                present = 1;
                break;
            }
        }
        if (!present && g_nrules < MAX_RULES) {
            snprintf(g_rules[g_nrules].pkg, sizeof(g_rules[0].pkg), "%s", key);
            g_rules[g_nrules].r = r;
            g_rules[g_nrules].g = g;
            g_rules[g_nrules].b = b;
            g_nrules++;
        } else if (!present) {
            LOGI("warn: [conf] rules table full (%d), dropped rule %s", MAX_RULES, key);
        }
        /* a colour-only line keeps the shared [notify] preset; an extended
         * line builds its own (a tail that does not rebuild falls back to
         * colour-only inside rule_synth) */
        if (tail) rule_synth(key, tail);
    }
}

/* One logical line - comments already cut, continuations already joined.
 * Blank lines are the caller's business. */
static void parse_line(char *sec, size_t seclen, char *line)
{
    trim(line);
    if (!*line) return;
    if (line[0] == '[') {
        char *p = strchr(line, ']');
        if (!p) return;
        *p = '\0';
        snprintf(sec, seclen, "%s", line + 1);
        return;
    }
    if (!strcmp(sec, "suppress")) {
        if (!in_supp(line) && g_nsupp < MAX_SUPP)
            snprintf(g_supp[g_nsupp++], sizeof(g_supp[0]), "%s", line);
        else if (!in_supp(line))
            LOGI("warn: [conf] suppress list full (%d), dropped %s", MAX_SUPP, line);
        return;
    }
    char *eq = strchr(line, '=');
    if (eq && (eq > line)) {
        *eq = '\0';
        trim(line);
        char *val = eq + 1;
        trim(val);
        /* every key lands in the table - except [rules], whose lines
         * parse_value owns (extended ones become the synthetic
         * [notify.<pkg>] sections instead), and except render=, which
         * is the expanded form of the chip keys preset_line writes */
        if (strcmp(sec, "rules") != 0) {
            if (!strcmp(line, "render")) preset_line(sec, val);
            else                          kv_put(sec, line, val);
        }
        parse_value(sec, line, val); /* built-in sections apply it    */
    }
}

static void load_file(void)
{
    reset_dynamic();
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) {
        LOGI("conf: no %s - every effect stays dark until it exists", CONF_PATH);
        return;
    }
    char sec[128] = "";
    char raw[512], logical[1024];
    logical[0] = '\0';
    int broken = 0, open = 0;
    while (fgets(raw, sizeof(raw), f)) {
        /* a physical line longer than raw[]: drain its tail so the rest
         * cannot be read as a line of its own, and drop the whole value */
        if (!strchr(raw, '\n') && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') { }
            broken = 1;
        }
        strip_comment(raw);
        int more = cut_continuation(raw);
        /* a comment or blank line inside a continuation is part of it */
        if (open && !more && !*raw) continue;
        if (!line_append(logical, sizeof(logical), raw)) broken = 1;
        open = more;
        if (more) continue;
        if (broken) LOGI("warn: [conf] over-long line dropped: \"%.32s\"", logical);
        else        parse_line(sec, sizeof(sec), logical);
        logical[0] = '\0';
        broken = 0;
    }
    /* the file ended inside a continuation: what arrived IS the line */
    if (logical[0] && !broken) parse_line(sec, sizeof(sec), logical);
/* record the mtime this load saw so a later identical write (a saved
     * config is rewritten with a new mtime, so a true "same content"
     * rewrite still differs here) can be deduped against by the reload
     * guard in core.c */
    struct stat st;
    if (fstat(fileno(f), &st) == 0) g_last_mtime = st.st_mtime;
    fclose(f);

    /* Charge is a two-key pair, so it can only be judged once both lines
     * are in. An inverted or half-written pair is a broken preset, not
     * something to repair by swapping: drop both so charge goes dark and
     * the operator sees which file line to fix. */
    if (g_first_at != ABSENT && g_second_at != ABSENT &&
        g_first_at > g_second_at) {
        LOGI("warn: [conf] [charge] first_threshold=%d > second_threshold=%d - "
             "charge stays off until the pair is fixed",
             g_first_at, g_second_at);
        g_first_at = ABSENT;
        g_second_at = ABSENT;
    }

    /* [led] logging -> logger toggle. The switch only applies here, on a
     * real reload (inotify event or SIGALRM from the GUI); log_line() does
     * NOT re-read config itself. The LOGI below intentionally comes after
     * the toggle applies. logging=1 is the only value this file decides
     * for itself: with logging off there would be no channel to report a
     * missing logging key through. */
    g_logging = (conf_get_int("led", "logging", 1) != 0);
    log_set_enabled(g_logging);
    LOGI("conf: loaded %s (%d suppressed, %d rules) logging=%d",
         CONF_PATH, g_nsupp, g_nrules, g_logging);

    /* One summary of every key this load could not resolve, so a broken
     * led.conf is diagnosable from the log alone instead of by noticing
     * that an event stays dark. */
    int nmissing = 0;
    if (g_first_at == ABSENT || g_second_at == ABSENT) nmissing++;
    if (!g_ntf_set) nmissing++;
    if (nmissing)
        LOGI("warn: [conf] %d required key(s) unresolved (charge "
             "thresholds, notify render colour) - the matching effects "
             "stay dark", nmissing);
    if (g_nbroken)
        LOGI("warn: [conf] %d rule(s) had an unreadable preset tail - "
             "they run colour-only until led.conf is rewritten", g_nbroken);
}

/* ---------------- public early hooks (called from core) ---------------- */

/* Set when the core's inotify watch on CONF_DIR saw led.conf change
 * (any writer: GUI, a root shell, OK-file browsers). The only readers of
 * this are conf_maybe_reload() and the signal path, single-threaded. */
static int  g_conf_dirty  = 0;
/* load_file() has run at least once: a fresh daemon must load on its FIRST
 * lookup without waiting for an event, and a config that vanishes at runtime
 * must fall back to every value being absent. */
static int  g_conf_loaded = 0;

void conf_note_change(void)
{
    g_conf_dirty = 1;
}

/* inotify channel: watches CONF_DIR (not the file) so sed -i's temp+rename
 * - which swaps the inode - is caught by IN_MOVED_TO on the name, the same
 * way an in-place rewrite is caught by IN_CLOSE_WRITE. -1 = unavailable. */
static int  g_cfg_fd = -1;

int conf_watch_init(void)
{
    if (g_cfg_fd >= 0) return g_cfg_fd;
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) return -1;
    if (inotify_add_watch(fd, CONF_DIR,
                          IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE |
                          IN_MOVED_FROM | IN_DELETE) < 0) {
        close(fd);
        return -1;
    }
    g_cfg_fd = fd;
    return fd;
}

/* drain everything inotify queued; flag a reload only for led.conf */
void conf_watch_handle(void)
{
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(g_cfg_fd, buf, sizeof(buf));
        if (n <= 0) break;              /* EAGAIN/EOF: drained */
        for (char *p = buf; p < buf + n; ) {
            const struct inotify_event *e =
                (const struct inotify_event *)p;
            if (e->len > 0 && !strcmp(e->name, "led.conf"))
                conf_note_change();
            p += sizeof(struct inotify_event) + e->len;
        }
    }
}

void conf_maybe_reload(void)
{
    if (g_cfg_fd >= 0) {
        /* event-driven path */
        if (!g_conf_dirty && g_conf_loaded) return;
        g_conf_dirty = 0;
        g_conf_loaded = 1;
        load_file();        /* logs when the file is gone */
        return;
    }
    /* fallback (inotify init failed): lazy probe */
    struct stat st;
    if (stat(CONF_PATH, &st) != 0) {
        if (g_last_mtime != 0) { reset_dynamic(); g_last_mtime = 0; }
        return;
    }
    if (st.st_mtime == g_last_mtime) return;
    g_last_mtime = st.st_mtime;
    load_file();
}

/* reload guard for core.c: a single file rewrite can surface as several
 * flags (one inotify burst chunked by the kernel + the GUI's SIGALRM poke,
 * which always comes after the write). The guard admits only the first flag
 * for a given mtime, so N writes mean exactly N loads. Sees the file as
 * changed on the very first use too (g_last_mtime starts at 0). */
int conf_file_changed(void)
{
    struct stat st;
    if (stat(CONF_PATH, &st) != 0) {
        if (g_last_mtime != 0) { reset_dynamic(); g_last_mtime = 0; }
        return 1;
    }
    return st.st_mtime != g_last_mtime;
}

/* returns 1 if suppressed, 0 otherwise (config [suppress] only) */
int conf_suppressed(const char *pkg)
{
    conf_maybe_reload();
    return in_supp(pkg);
}

/* returns 1 and the r,g,b colour if the package has a [rules] entry in
 * led.conf, 0 otherwise (caller falls back to the default colour) */
int conf_pkg_rgb(const char *pkg, int *r, int *g, int *b)
{
    conf_maybe_reload();
    return rule_rgb(pkg, r, g, b);
}

/* Charge thresholds. ABSENT (-1) means the file did not define the key
 * or defined it badly - charge.c drops the event and logs. Never treat
 * these as usable numbers without checking for ABSENT. */
int conf_first_threshold(void)
{
    conf_maybe_reload(); return g_first_at;
}

int conf_second_threshold(void)
{
    conf_maybe_reload(); return g_second_at;
}

/* [notify] preset colour: the colour triple of the section's render line.
 * Returns 1 and fills r,g,b, or 0 when the file has no usable one - the
 * caller must drop. */
int conf_notif_rgb(int *r, int *g, int *b)
{
    conf_maybe_reload();
    if (!g_ntf_set) return 0;
    *r = g_ntf_r; *g = g_ntf_g; *b = g_ntf_b;
    return 1;
}



/* ---------------- generic value lookups for mod-owned sections ---------------- */

/* raw string value for (sec,key), NULL if absent */
const char *conf_get_str(const char *sec, const char *key)
{
    conf_maybe_reload();
    for (int i = 0; i < g_nkv; i++)
        if (!strcmp(g_kv[i].sec, sec) && !strcmp(g_kv[i].key, key))
            return g_kv[i].val;
    return NULL;
}

/* integer value for (sec,key), def if absent or unparsable.
 * Only for the one value the daemon may decide for itself ([led]
 * logging - a missing logging key has no channel to complain through).
 * Every POLICY value must go through conf_req_int instead. */
long conf_get_int(const char *sec, const char *key, long def)
{
    const char *v = conf_get_str(sec, key);
    if (!v) return def;
    long x = atol(v);
    if (!x && (v[0] < '0' || v[0] > '9') && v[0] != '-') return def;
    return x;
}

/* ---------------- required keys: drop + log ---------------- */

/* report a required key that could not be resolved, once per reload */
static void req_warn(const char *sec, const char *key, const char *why)
{
    char id[160];
    snprintf(id, sizeof(id), "[%s] %s", sec, key);
    for (int i = 0; i < g_nreqwarn; i++)
        if (!strcmp(g_reqwarn[i], id)) return;
    if (g_nreqwarn < MAX_REQWARN)
        snprintf(g_reqwarn[g_nreqwarn++], sizeof(g_reqwarn[0]), "%s", id);
    LOGI("warn: [conf] required key %s %s - affected events are dropped",
         id, why);
}

/* Required integer. Returns 1 and fills out only when the file defines a
 * whole number inside [lo,hi]. Absent, unparsable or out of range
 * returns 0 after a once-only log; the caller must drop the event. */
int conf_req_int(const char *sec, const char *key, long lo, long hi, long *out)
{
    const char *v = conf_get_str(sec, key);
    if (!v) { req_warn(sec, key, "is not set"); return 0; }
    long x;
    if (!parse_int(v, &x)) { req_warn(sec, key, "is not a number"); return 0; }
    if (x < lo || x > hi) {
        char buf[64];
        snprintf(buf, sizeof(buf), "=%ld is outside %ld..%ld", x, lo, hi);
        req_warn(sec, key, buf);
        return 0;
    }
    *out = x;
    return 1;
}

/* Required "r,g,b" colour, 0..255 per channel: the one the section's
 * render= line carries. Same contract as conf_req_int: 1 + out, or 0 +
 * once-only log and a dropped event. */
int conf_req_color(const char *sec, int out[3])
{
    const char *v = conf_get_str(sec, "color");
    if (!v) { req_warn(sec, "render colour", "is not set"); return 0; }
    int r, g, b;
    if (!parse_rgb(v, &r, &g, &b)) {
        req_warn(sec, "render colour", "is not r,g,b in 0..255");
        return 0;
    }
    out[0] = r; out[1] = g; out[2] = b;
    return 1;
}

/* scratch buffer for conf_notify_sec's synthetic section name; returned
 * pointer stays valid until the next conf_notify_sec call */
static char conf_sec_buf[128];

/* does a section exist at all? (e.g. the synthetic [notify.<pkg>] preset
 * generated from an extended [rules] line) */
int conf_sec_exists(const char *sec)
{
    conf_maybe_reload();
    return kv_has_sec(sec);
}

/* preset section a notification for [pkg] must read:
 *   extended rule -> synthetic "notify.<pkg>" (its own preset),
 *   otherwise the shared "notify" default preset (a colour-only rule
 *   carries no timing of its own, so it inherits the shared one).
 * The returned pointer is stable until the next reload. */
const char *conf_notify_sec(const char *pkg)
{
    conf_maybe_reload();
    if (pkg && pkg[0]) {
        snprintf(conf_sec_buf, sizeof(conf_sec_buf), "notify.%s", pkg);
        if (kv_has_sec(conf_sec_buf)) return conf_sec_buf;
    }
    return "notify";
}
