/*
 * config.c - runtime INI config for the LED policies.
 *
 * Reads /data/adb/modules/led_hal_root/led.conf at runtime and merges it
 * over the link-time registry (internal pseudo-packages only, e.g.
 * missed.call) so the user can tweak blacklists / colours / timings
 * WITHOUT recompiling.
 *
 * SECTIONS:
 *   [suppress]  one package per line - never lights the LED
 *   [rules]     pkg=r,g,b   (0-255 per channel; no more bit masks)
 *   [charge]    first_threshold / second_threshold (%) ONLY - each band
 *               owns its renderer: [charge.lower|middle|upper] mode=
 *               color= plus the [charge.<band>.solid/breath/wave] chip
 *               sections (timing = chip-owned)
 *   [notify]    SHARED behavior for every app:
 *               notif_max_sec (0 = unlimited), default_color r,g,b for
 *               apps without a [rules] entry (per-app color = [rules])
 *   [ring]      incoming-call rainbow: max_sec + v3 color
 *   [voip]      messenger-call rainbow: max_sec + v3 color
 *
 * v3 per-event renderer sections (owned by led.c through the generic kv
 * table, one per event sec = charge|notify|missed|alarm|ring|voip):
 *   [sec]          mode=off|solid|breath|wave
 *   [sec.solid]    cur=r,g,b (0..15)
 *   [sec.breath]   repeat, cur_r/cur_g/cur_b + rise/hold/fall/offt (owned
 *                  by the chip section itself)
 *   [sec.wave]     t0=r,g,b phase offsets, repeat + rise/hold/fall/offt
 *   [led]          chip/daemon globals only: logging, imax
 *                  imax
 *
 * Every other key=value pair anywhere in the file lands in a generic
 * key-value table (conf_get_str / conf_get_int). That is how mods own
 * their config: a mod (e.g. mods/ring.c) reads its own [ring] section
 * by name without config.c knowing the key exists at all.
 *
 * The file is reloaded lazily: every lookup calls conf_maybe_reload(),
 * which normally just consumes an inotify flag set by the core when the
 * directory holding led.conf changed (see conf_watch_init/conf_watch_handle).
 * No I/O happens in the steady loop unless the file was actually edited;
 * a stat()-based probe survives only as a fallback for when inotify is
 * unavailable.
 *
 * Registry merge rules:
 *   - suppress list = file [suppress] entries ONLY (runtime; no link-time
 *     blacklist anymore - suppress.c was removed)
 *   - rules        = file [rules] entries override the link-time registry,
 *     which carries ONLY internal pseudo-packages (missed.call)
 *   - charge/notify colours come from the file, falling back to builtins
 *
 * All config text and this file are ASCII-only (no non-ASCII in artifacts).
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
 * [charge.solid], [charge.breath], [charge.wave], ...) */
#define MAX_KV   256
struct kv { char sec[24]; char key[32]; char val[48]; };

/* builtin fallbacks */
#define DEF_FIRST_AT 90
#define DEF_SECOND_AT 95
#define DEF_NOTIF_MAX 1800
/* builtin notify default: breathing white */
#define DEF_NTF_R 255
#define DEF_NTF_G 255
#define DEF_NTF_B 255

static char    g_supp[MAX_SUPP][96];
static int     g_nsupp = 0;
static struct rule { char pkg[96]; int r, g, b; } g_rules[MAX_RULES];
static int     g_nrules = 0;
static struct kv g_kv[MAX_KV];
static int     g_nkv = 0;

static int  g_first_at = DEF_FIRST_AT;
static int  g_second_at = DEF_SECOND_AT;
static int  g_ntf_r = DEF_NTF_R, g_ntf_g = DEF_NTF_G, g_ntf_b = DEF_NTF_B;
static long g_notif_max = DEF_NOTIF_MAX;

static time_t g_last_mtime = 0;

/* [led] logging: runtime switch for the on-disk log. Applied to the
 * logger via log_set_enabled() on every config (re)load so the GUI can
 * flip it live (write led.conf, SIGALRM). Defaults to on. */
static int g_logging = 1;

/* ---------------- helpers ---------------- */

static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1])) s[--l] = '\0';
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

/* ---------------- registry seeding ---------------- */

/* terminator keeps the link-time chgd_rules section non-empty so
 * __start/__stop stay defined even with zero built-in rules; consumers
 * stop at .pkg == NULL. No hidden rule is defined at build time - every
 * real color must come from led.conf [rules]. */
static const struct led_rule chgd_rules_terminator
    __attribute__((used, section("chgd_rules"))) = { NULL, 0, 0, 0 };

/* seed the rule table from the link-time registry (extension point for
 * future built-ins, currently only the terminator). Real app colors come
 * from led.conf [rules] and win over any built-in. */
static void seed_rules(void)
{
    const struct led_rule *r;
    int rr = 0, gg = 0, bb = 0;
    for (r = __start_chgd_rules; r->pkg; r++) {
        if (g_nrules >= MAX_RULES) break;
        if (rule_rgb(r->pkg, &rr, &gg, &bb) == 0) {
            snprintf(g_rules[g_nrules].pkg, sizeof(g_rules[0].pkg), "%s", r->pkg);
            g_rules[g_nrules].r = r->r;
            g_rules[g_nrules].g = r->g;
            g_rules[g_nrules].b = r->b;
            g_nrules++;
        }
    }
}

/* ---------------- ---------------- */

static void reset_dynamic(void)
{
    g_nsupp = 0;
    g_nrules = 0;
    g_nkv = 0;
    seed_rules();
    g_first_at = DEF_FIRST_AT;
    g_second_at = DEF_SECOND_AT;
    g_ntf_r = DEF_NTF_R; g_ntf_g = DEF_NTF_G; g_ntf_b = DEF_NTF_B;
    g_notif_max = DEF_NOTIF_MAX;
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
    if (g_nkv >= MAX_KV) return;
    snprintf(g_kv[g_nkv].sec, sizeof(g_kv[g_nkv].sec), "%s", sec);
    snprintf(g_kv[g_nkv].key, sizeof(g_kv[g_nkv].key), "%s", key);
    snprintf(g_kv[g_nkv].val, sizeof(g_kv[g_nkv].val), "%s", val);
    g_nkv++;
}

/* parse one key=value line into section (built-in sections only);
 * everything else already lives in the generic kv table */
static void parse_value(const char *sec, const char *key, const char *val)
{
    int r, g, b;
    if (!strcmp(sec, "charge")) {
        if      (!strcmp(key, "first_threshold")) {
            g_first_at = atoi(val);
            /* order-free: keep first <= second */
            if (g_first_at > g_second_at) {
                int t = g_first_at; g_first_at = g_second_at; g_second_at = t;
            }
        }
        else if (!strcmp(key, "second_threshold")) {
            g_second_at = atoi(val);
            if (g_first_at > g_second_at) {
                int t = g_first_at; g_first_at = g_second_at; g_second_at = t;
            }
        }
        return;
    }
    if (!strcmp(sec, "notify")) {
        if      (!strcmp(key, "default_color")) {
            if (parse_rgb(val, &r, &g, &b)) {
                g_ntf_r = r; g_ntf_g = g; g_ntf_b = b;
            } else {
                LOGI("conf: bad [notify] default_color=%s (want r,g,b)", val);
            }
        }
        else if (!strcmp(key, "notif_max_sec")) g_notif_max = atol(val);
        return;
    }
    if (!strcmp(sec, "rules")) {
        if (!parse_rgb(val, &r, &g, &b)) {
            LOGI("conf: bad [rules] %s=%s (want r,g,b)", key, val);
            return;
        }
        /* file wins over builtin for the same package */
        for (int i = 0; i < g_nrules; i++) {
            if (!strcmp(g_rules[i].pkg, key)) {
                g_rules[i].r = r; g_rules[i].g = g; g_rules[i].b = b;
                return;
            }
        }
        if (g_nrules < MAX_RULES) {
            snprintf(g_rules[g_nrules].pkg, sizeof(g_rules[0].pkg), "%s", key);
            g_rules[g_nrules].r = r;
            g_rules[g_nrules].g = g;
            g_rules[g_nrules].b = b;
            g_nrules++;
        }
    }
}

static void load_file(void)
{
    reset_dynamic();
    FILE *f = fopen(CONF_PATH, "r");
    if (!f) {
        LOGI("conf: no %s, using builtins", CONF_PATH);
        return;
    }
    char sec[32] = "";
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!*line || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            char *p = strchr(line, ']');
            if (!p) continue;
            *p = '\0';
            snprintf(sec, sizeof(sec), "%s", line + 1);
            continue;
        }
        if (!strcmp(sec, "suppress")) {
            if (!in_supp(line) && g_nsupp < MAX_SUPP)
                snprintf(g_supp[g_nsupp++], sizeof(g_supp[0]), "%s", line);
            continue;
        }
        char *eq = strchr(line, '=');
        if (eq && (eq > line)) {
            *eq = '\0';
            trim(line);
            char *val = eq + 1;
            trim(val);
            kv_put(sec, line, val);      /* every key lands in the table */
            parse_value(sec, line, val); /* built-in sections apply it    */
        }
    }
    /* record the mtime this load saw so a later identical write (a saved
     * config is rewritten with a new mtime, so a true "same content"
     * rewrite still differs here) can be deduped against by the reload
     * guard in core.c */
    struct stat st;
    if (fstat(fileno(f), &st) == 0) g_last_mtime = st.st_mtime;
    fclose(f);
    /* [led] logging -> logger toggle. The switch only applies here, on a
     * real reload (inotify event or SIGALRM from the GUI); log_line() does
     * NOT re-read config itself. The LOGI below intentionally comes after
     * the toggle applies. */
    g_logging = (conf_get_int("led", "logging", 1) != 0);
    log_set_enabled(g_logging);
    LOGI("conf: loaded %s (%d suppressed, %d rules) logging=%d",
         CONF_PATH, g_nsupp, g_nrules, g_logging);
}

/* ---------------- public early hooks (called from core) ---------------- */

/* Set when the core's inotify watch on CONF_DIR saw led.conf change
 * (any writer: GUI, a root shell, OK-file browsers). The only readers of
 * this are conf_maybe_reload() and the signal path, single-threaded. */
static int  g_conf_dirty  = 0;
/* load_file() has run at least once (mirrors the old "mtime != 0" probe:
 * a fresh daemon must load on its FIRST lookup without waiting for an
 * event, and a config that vanishes at runtime must fall back to builtins). */
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
        /* event-driven path: no stat() in the hot loop at all */
        if (!g_conf_dirty && g_conf_loaded) return;
        g_conf_dirty = 0;
        g_conf_loaded = 1;
        load_file();        /* resets to builtins; logs when file is gone */
        return;
    }
    /* fallback (inotify init failed): old stat()-based lazy probe */
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
 * which always comes after the write). Every flag used to load the file,
 * so one save printed a burst of 2-4 identical "conf: loaded" lines. The
 * guard admits only the first flag for a given mtime - N writes now mean
 * exactly N loads. Sees the file as changed on the very first use too
 * (g_last_mtime starts at 0). */
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

/* returns 1 and the r,g,b colour if the package has a rule (builtin or
 * config overridden), 0 otherwise (core falls back to the default colour) */
int conf_pkg_rgb(const char *pkg, int *r, int *g, int *b)
{
    conf_maybe_reload();
    return rule_rgb(pkg, r, g, b);
}


int conf_first_threshold(void)
{
	conf_maybe_reload(); return g_first_at;
}

int conf_second_threshold(void)
{
	conf_maybe_reload(); return g_second_at;
}


void conf_notif_rgb(int *r, int *g, int *b)
{
    conf_maybe_reload(); *r = g_ntf_r; *g = g_ntf_g; *b = g_ntf_b;
}


long conf_notif_max_sec(void)
{
	conf_maybe_reload(); return g_notif_max;
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

/* integer value for (sec,key), def if absent or unparsable */
long conf_get_int(const char *sec, const char *key, long def)
{
    const char *v = conf_get_str(sec, key);
    if (!v) return def;
    long x = atol(v);
    if (!x && (v[0] < '0' || v[0] > '9') && v[0] != '-') return def;
    return x;
}