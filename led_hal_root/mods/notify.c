/*
 * mods/notify.c - ordinary notifications: one kind, many entries.
 *
 * The ENQ path of the bridge lands here: a route that pins a category
 * ("call", "missed_call", "alarm") goes to its own kind.
 * All scheduling lives in channel.c, the one pool: this file only gates
 * (suppress) and paints (rgb + [sec] renderer).
 *
 * The entry IS the state: the pool owns the ranking and the show clocks,
 * and every event goes through pool_push(), so an outranking effect takes
 * the LEDs and the notification takes them back the moment that effect
 * lets go.
 */

#include <stdio.h>
#include <string.h>
#include "../chgd.h"

/* ---------------- registry lookups ---------------- */

/* colour for a package: conf_pkg_rgb() looks up [rules] from led.conf,
 * else the shared colour - the one in the [notify] render line. Returns 1
 * when a per-package rule supplied the color (the "app" preset is used),
 * 0 for the shared default color (the "default" preset).
 *
 * Both sources are REQUIRED, so the function reports whether a colour was
 * resolved at all: with neither a rule nor a usable [notify] render colour
 * there is nothing to paint and the caller drops the entry.
 * config.c already named the unresolved key once per reload. */
static int rgb_for(const char *pkg, int *r, int *g, int *b, int *app)
{
    if (conf_pkg_rgb(pkg, r, g, b)) {
        *app = 1;
        return 1;
    }
    *app = 0;
    return conf_notif_rgb(r, g, b);
}

/* the budget: [notify.<pkg>] max_sec for a package whose rule carries
 * its own preset, else the shared [notify] max_sec. 0 = unlimited,
 * the CAN edge is the only end. A required key that is missing cannot be
 * invented into a lifetime, so the pool drops the entry. */
static long notify_cap_ms(struct evt *e)
{
    int r, g, b, app;
    const char *sec = rgb_for(e->pkg, &r, &g, &b, &app) && app
                    ? conf_notify_sec(e->pkg) : "notify";
    long secs;
    if (!conf_req_int(sec, "max_sec", 0, 86400, &secs)) return -1L;
    return secs * 1000L;
}

/* One trace line per package per daemon run. What this file has to say
 * about a package (suppressed, or nothing to paint) is a decision about the
 * package, and a background emitter repeats its ENQ forever - a VPN
 * foreground notification, the telephony app - so every later repost would
 * repeat a line nobody decided. A reload that changes [suppress] or a
 * [rules] colour is a new decision, and the next run reports it. */
#define TRACE_SLOTS 32
static char s_traced[TRACE_SLOTS][96];

/* 1 = this package was not traced yet (and is now), 0 = already reported */
static int pkg_traced(const char *pkg)
{
    for (int i = 0; i < TRACE_SLOTS; i++) {
        if (!s_traced[i][0]) {
            snprintf(s_traced[i], sizeof(s_traced[i]), "%s", pkg);
            return 1;
        }
        if (!strcmp(s_traced[i], pkg)) return 0;
    }
    return 0;      /* more packages than slots: the trace stays quiet */
}

/* ---------------- the kind ---------------- */

/* the filter runs before the entry exists, so a suppressed package never
 * occupies a pool slot and never logs once per heartbeat */
static int notify_accept(struct evt *e)
{
    if (!conf_suppressed(e->pkg)) return 1;
    if (pkg_traced(e->pkg))
        LOGI("suppressed: %s", e->pkg);
    return 0;
}

static int notify_paint(struct evt *e, struct evt_paint *p)
{
    int r, g, b, app;
    /* rule-matched packages run their OWN preset when the rule carries an
     * extended tail (synthetic [notify.<pkg>]), otherwise the shared
     * [notify] default preset. Color always comes from the rule. */
    if (!rgb_for(e->pkg, &r, &g, &b, &app)) {
        /* Neither a [rules] colour nor a usable [notify] render colour:
         * nothing to paint, so the pool tries the next candidate.
         * config.c already named the unresolved key once per reload. */
        if (pkg_traced(e->pkg))
            LOGI("notify: %s has no colour in led.conf", e->pkg);
        return 0;
    }
    const char *sec = app ? conf_notify_sec(e->pkg) : "notify";
    /* led_event() resolves the active [sec] mode and paints from the
     * section's render= line. Its [led] line names the section (a per-app
     * rule as notify.<pkg>, the shared default as notify), the mode and the
     * chip parameters - everything this paint decided, once per real
     * change, so a paint hook must not narrate it a second time. */
    PAINT_SEC(p, sec);
    p->r = r;
    p->g = g;
    p->b = b;
    PAINT_LABEL(p, e->pkg);
    return 1;
}

static const struct evt_kind notify_kind = {
    .name     = "notify",
    .def_rank = RANK_NOTIFY,
    /* the Android "Notification light" toggle, and the screen guard: both
     * apply, so no bypass and no singleton - a chat app posts several
     * notifications and each one is its own entry */
    .flags  = EV_PULSE_GATED,
    .cap_ms = notify_cap_ms,
    .accept = notify_accept,
    .paint  = notify_paint,
};
REGISTER_EVT(notify_kind);