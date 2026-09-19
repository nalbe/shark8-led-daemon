/*
 * mods/dialer.c - missed-call / call rainbow verification + [missed] LED.
 *
 * Claims every com.google.android.dialer notification via REGISTER_HANDLER.
 * No dedicated kernel/eventlog event exists for a missed call, so pkg alone
 * cannot distinguish "incoming ringing" / "ongoing" from an actual missed
 * call. The live-call rainbow itself is armed by the NLS bridge (RING_ON,
 * see mods/ring.c): it classifies the dialer's live-call notification
 * before the ping reaches us, so dialer_handle never sees an armed call
 * and only runs missed-call verification: it queries
 * content://call_log/calls (root) for a fresh MISSED row (type=3, new=1).
 * A couple of rechecks absorb the provider/db race. Dedup by call_log _id.
 *
 * Missed-call LED: owns its own [missed] section in led.conf, independent
 * of [notify] and [ring]:
 *   color            r,g,b                          (def 0,0,255 blue)
 *   max_sec          LED max duration (s), 0=unlim   (def 1800)
 *   mode + [missed.solid/breath/wave] chip sections  (mode; each chip
 *            owns its own rise/hold/fall/offt keys)
 *
 * REGISTER_HANDLER: any mod can claim a package's notifications and fully
 * own the response (see the dispatch in notify.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../chgd.h"

#define DIALER_PKG     "com.google.android.dialer"
#define MISSED_PKG     "missed.call"
#define CALL_FRESH_MS  120000LL  /* ignore missed rows older than this */
#define CALL_CHECKS    4         /* verification attempts per ping */

/* missed-call verification state */
long long g_last_missed_id   = -1;
int       g_call_checks_left = 0;
time_t    g_next_call_check  = 0;

const char *dialer_pkg_id(void)
{
    return DIALER_PKG;
}

/* ---------------- [missed] config readers ---------------- */

static void missed_rgb(int *r, int *g, int *b)
{
    *r = 0; *g = 0; *b = 255;   /* default blue */
    const char *c = conf_get_str("missed", "color");
    if (c && c[0] &&
        sscanf(c, "%d,%d,%d", r, g, b) == 3) {
        if (*r < 0) *r = 0; if (*r > 255) *r = 255;
        if (*g < 0) *g = 0; if (*g > 255) *g = 255;
        if (*b < 0) *b = 0; if (*b > 255) *b = 255;
    }
}

static long missed_max_sec(void)
{
    return conf_get_int("missed", "max_sec", 1800);
}

/* ---------------- missed-call LED paint ---------------- */

static void missed_paint(void)
{
    int r, g, b;
    missed_rgb(&r, &g, &b);

    LOGI("missed arm: rgb=%d,%d,%d", r, g, b);
    g_applied_band[0] = '\0';       /* charge leds must reapply after */

    /* led_event() resolves the active [missed] mode and paints via
     * [missed.*] chip sections; timing comes from the chip sections. */
    const char *engine = led_event("missed", r, g, b);

    snprintf(g_st.cur_pkg, sizeof(g_st.cur_pkg), "%s", MISSED_PKG);
    snprintf(g_st.owner_pkg, sizeof(g_st.owner_pkg), "%s", DIALER_PKG);
    g_st.armed_at = time(NULL);
    g_st.test = 0;
    status_write("missed", "", MISSED_PKG, r, g, b, engine);
    retune_timer();
}

/* ---------------- mode: owns "missed.call" channel ---------------- */

int missed_is_active(void)
{
    return g_st.cur_pkg[0] && !strcmp(g_st.cur_pkg, MISSED_PKG);
}

static int missed_owns(const char *pkg)
{
    return pkg && !strcmp(pkg, MISSED_PKG) &&
           g_st.cur_pkg[0] && !strcmp(g_st.cur_pkg, MISSED_PKG);
}

static void missed_tick(void)
{
    long cap = missed_max_sec();
    if (cap > 0 && difftime(time(NULL), g_st.armed_at) >= (double)cap) {
        LOGI("missed timeout");
        disarm_notification(&g_st, "missed timeout");
    }
}

/* adaptive wakeup: sleep until the [missed] max_sec cap expires;
 * cap=0 means permanent and disarm is purely event-driven (cancel) */
static long missed_next_wake(void)
{
    long cap = missed_max_sec();
    if (cap <= 0) return 0;
    double age = difftime(time(NULL), g_st.armed_at);
    long remain = (long)(cap - age);
    if (remain < 1) remain = 1;
    return remain * 1000L;
}

/* ---------------- call_log query ---------------- */

/* Run `content query` as root; returns 1 and the newest missed-call row
 * (rows come date DESC) if present in the output. */
static int newest_missed(long long *id, long long *date_ms)
{
    static char out[16384];
    char *argv[] = {
        (char *)"/system/bin/content", (char *)"query",
        (char *)"--uri",   (char *)"content://call_log/calls",
        (char *)"--projection", (char *)"_id:date:type:new:number",
        (char *)"--sort",  (char *)"date DESC",
        NULL
    };
    size_t got = run_capture(argv, out, sizeof(out));
    if (!got) return 0;

    /* Row: N _id=X, date=Y, type=T, new=N, number=... */
    int found = 0;
    long long best_id = -1, best_ts = -1;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        long long rid = 0, rts = 0; int type = 0, isnew = 0;
        if (sscanf(line, "Row: %*d _id=%lld, date=%lld, type=%d, new=%d",
                   &rid, &rts, &type, &isnew) != 4)
            continue;
        if (type == 3 && isnew == 1 && rid > best_id) {
            best_id = rid; best_ts = rts; found = 1;
            break;                           /* first hit = newest missed */
        }
    }
    if (found) { *id = best_id; *date_ms = best_ts; }
    return found;
}

/* ---------------- verification window ---------------- */

void dialer_reopen_window(void)
{
    g_call_checks_left = CALL_CHECKS;
    g_next_call_check = time(NULL) + 1;
}

static void check_missed_call(void)
{
    long long id = 0, ts = 0;
    int found = newest_missed(&id, &ts);
    g_call_checks_left--;
    g_next_call_check = time(NULL) + (time_t)(CALL_RECHECK_MS / 1000L);

    if (found) {
        long long age = (long long)time(NULL) * 1000 - ts;
        if (id > g_last_missed_id && age >= 0 && age < CALL_FRESH_MS) {
            g_last_missed_id = id;
            g_call_checks_left = 0;
            LOGI("missed call id=%lld age=%lldms -> led", id, age);
            missed_paint();                 /* [missed] section config */
            retune_timer();
            return;
        }
        if (id > g_last_missed_id) {
            /* stale-but-unseen entry: consume it so it can't fire later */
            g_last_missed_id = id;
        }
    }

    retune_timer();                          /* recheck window or back to idle */
}

/* Called from the core tick every heartbeat. */
void maybe_call_check(void)
{
    if (g_call_checks_left <= 0 || time(NULL) < g_next_call_check) return;
    check_missed_call();
}

/* ---------------- dialer notification handler ---------------- */

/* Dedup guard against Android notification reposts. The dialer re-posts
 * its notification on every refresh, so one that just sits in the shade
 * (e.g. a stale missed row) fires ENQ over and over; every ENQ used to
 * reopen the 4x2s verification window -> repeated fork+exec of `content
 * query`. A window is reopened only when the notification id CHANGES, or
 * when the same id returns after it aged past CALL_FRESH_MS (a cleared
 * and reused id slot). Same-id reposts inside the window are silent
 * no-ops. */
static long long g_last_dialer_id = -1;
static time_t    g_last_dialer_ts = 0;

/* Claims every dialer notification. The live-call rainbow is armed by the
 * NLS bridge before this ping (RING_ON); if we still got a ping while it
 * is up, keep it. Otherwise the notification is a missed-call row: open a
 * short verification window over call_log. */
static int dialer_handle(const char *pkg, int id)
{
    (void)pkg;
    if (ring_is_active())
        return 1;                          /* rainbow already running */
    if (id >= 0 && id == g_last_dialer_id &&
        difftime(time(NULL), g_last_dialer_ts) <
            (double)(CALL_FRESH_MS / 1000LL))
        return 1;                          /* stale repost: skip window */
    g_last_dialer_id = id;
    g_last_dialer_ts = time(NULL);
    LOGI("dialer ping");
    /* not a live call: maybe the missed row lands slightly later -> open
     * a short verification window */
    g_call_checks_left = CALL_CHECKS;
    g_next_call_check = 0;               /* first check right away */
    maybe_call_check();
    return 1;
}

REGISTER_HANDLER(DIALER_PKG, dialer_handle);
REGISTER_MODE_WAKE("missed", 1000, missed_owns, missed_tick, missed_next_wake);

/* missed-call pseudo-package colour lives in [missed] color and is
 * painted directly by missed_paint(); the rule below keeps the link-time
 * chgd_rules section populated so the core/config registries stay alive. */
REGISTER_RULE(MISSED_PKG, 0, 0, 255);
