/*
 * mods/notify.c - notification pipeline mod.
 *
 * Claims every notification the plugins did not take, and owns the
 * ARMED channel of the daemon:
 *   REGISTER_HANDLER("*", ...)  default handler: suppression filter,
 *                               then an unconditional pool push
 *   REGISTER_MODE_WAKE("notify", ...) owns any armed real package while
 *                               ring/charge own their own channels;
 *                               adaptive wake: 1s only while a top parks
 *                               on the lit screen, else the cap deadline
 *                               or nothing (event-driven disarm)
 *
 * All scheduling lives in mods/queue.c: push puts the event in the
 * ledger, queue_arbitrate() picks the freshest, parks a top that lands
 * on a lit screen (no timer - the lazy tick flashes it on screen-off
 * or drops it on grace expiry), preempts with resume credit, and caps
 * the accumulated show time. This file only gates (suppress / light
 * toggle) and paints (rgb + [sec] renderer via arm_notification_ex).
 *
 * REGISTER_HANDLER(EXACT, ...) handlers (dialer, alarm) still claim
 * first - their events never reach the pool; the pseudo-package colors
 * (missed.call) come from their own mods.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "../chgd.h"

/* heartbeat while the pool has work: grace re-checks, screen-off flash,
 * cap disarm */
#define NOTIFY_TICK_MS 1000

/* ---------------- registry lookups ---------------- */

/* colour for a package: [rules] in led.conf, then internal pseudo-package
 * registry (missed.call), then the [notify] default_color. Returns 1 when
 * a per-package rule supplied the color (the "app" preset is used), 0 for
 * the shared default color (the "default" preset). */
static int rgb_for(const char *pkg, int *r, int *g, int *b)
{
    if (conf_pkg_rgb(pkg, r, g, b)) return 1;
    const struct led_rule *lr;
    for (lr = __start_chgd_rules; lr < __stop_chgd_rules; lr++)
        if (!strcmp(lr->pkg, pkg)) {
            *r = lr->r; *g = lr->g; *b = lr->b;
            return 1;
        }
    conf_notif_rgb(r, g, b);
    return 0;
}

/* suppressed = [suppress] in led.conf only (runtime, no link-time list) */
static int suppressed(const char *pkg)
{
    return conf_suppressed(pkg);
}

/* Rate-limit the log spam from persistent background emitters (e.g. VPN
 * foreground notifications fire every second): at most one suppressed
 * pair of lines per package per window, regardless of event frequency. */
#define SUPP_LOG_WINDOW_SEC 60
#define SUPP_LOG_SLOTS      32
static struct {
    char   pkg[128];
    time_t last;
} s_supp_log[SUPP_LOG_SLOTS];

static int supp_log_ok(const char *pkg)
{
    time_t now = time(NULL);
    for (int i = 0; i < SUPP_LOG_SLOTS; i++) {
        if (!s_supp_log[i].pkg[0]) {
            snprintf(s_supp_log[i].pkg, sizeof(s_supp_log[i].pkg), "%s", pkg);
            s_supp_log[i].last = now;
            return 1;
        }
        if (!strcmp(s_supp_log[i].pkg, pkg)) {
            if (now - s_supp_log[i].last < SUPP_LOG_WINDOW_SEC) return 0;
            s_supp_log[i].last = now;
            return 1;
        }
    }
    return 1;   /* table full: fall back to logging */
}

/* ---------------- arming / disarming ---------------- */

void arm_notification(struct notif_state *st, const char *pkg)
{
    arm_notification_ex(st, pkg, 0);
}

void arm_notification_ex(struct notif_state *st, const char *pkg, int test)
{
    /* System "Notification light" toggle: OFF = no LED for notifications.
     * Single gate point - everything notification-ish flows through here
     * (queue's q_paint). Call rainbows, alarms and the charge band are
     * separate and deliberately NOT gated. */
    if (!light_pulse_enabled()) {
        LOGI("notification light off: %s dropped", pkg);
        return;
    }
    int r, g, b;
    /* rule-matched packages run the [notify.app] preset (own renderer,
     * cap; color comes from the rule), everything else runs the shared
     * [notify] default preset. */
    int app  = rgb_for(pkg, &r, &g, &b);
    const char *sec = app ? "notify.app" : "notify";
    LOGI("arm detect: %s -> [%s] rgb=%d,%d,%d (rules|internal|default)",
         pkg, sec, r, g, b);
    g_applied_band[0] = '\0';       /* LEDs taken over: force reapply later */
    /* led_event() resolves the active [sec] mode and paints via the
     * [sec.*] chip sections; timing comes from those chip sections. */
    const char *engine = led_event(sec, r, g, b);
    snprintf(st->cur_pkg, sizeof(st->cur_pkg), "%s", pkg);
    st->owner_pkg[0] = '\0';    /* real package: owner = cur_pkg itself */
    st->armed_at = time(NULL);
    st->test = test;
    status_write("notify", "", pkg, r, g, b, engine);
    retune_timer();
    LOGI("notify armed: %s rgb=%d,%d,%d%s", pkg, r, g, b,
         test ? " (test)" : "");
}

void disarm_notification(struct notif_state *st, const char *why)
{
    if (!st->cur_pkg[0]) return;
    LOGI("notify disarmed (%s): %s", why, st->cur_pkg);
    st->cur_pkg[0] = '\0';
    st->test = 0;
    apply_charge_leds();            /* the idle owner takes over */
    retune_timer();
}

/* ---------------- default enqueue dispatch ---------------- */

/* The default handler is just a gate + push. All the "what now"
 * questions (screen on?, preemption, freshness) belong to the pool's
 * arbitrate. The voip/alarm/dialer planes claim their packages BEFORE
 * this handler via exact-match registration, so a messenger call chat
 * message still lands here normally and simply waits its turn. */
static int notify_default_handle(const char *pkg, int id)
{
    conf_maybe_reload();            /* pick up edited led.conf on the fly */
    if (suppressed(pkg)) {
        if (supp_log_ok(pkg)) {
            LOGI("enqueue event: %s", pkg);
            LOGI("suppressed: %s", pkg);
        }
        return 1;
    }
    LOGI("enqueue event: %s", pkg);
    queue_push(pkg, id);
    return 1;
}

/* ---------------- armed mode ---------------- */

/* owns any armed real package; defers to ring/voip/alarm/missed on
 * their pseudo-packages. The idle channel ("") goes to notify only while
 * the pool has work, so the modes partition g_st.cur_pkg exactly. */
static int notify_owns(const char *pkg)
{
    /* A non-empty pool claims the idle channel ("") so the core keeps a
     * timer and queue_arbitrate() runs - but the cadence is adaptive
     * (notify_next_wake): a 1s poll only while a top is parked and the
     * NLS bridge has not been feeding us ACTION_SCREEN events; with the
     * bridge alive the SCREEN command is the edge, so we sleep until the
     * next real cap deadline (or a single WATCHDOG_SEC pass). */
    if (!pkg || !pkg[0])
        return queue_has_pending() || queue_active();
    if (ring_is_active()) return 0;     /* ring owns its pseudo-pkg */
    if (voip_active()) return 0;        /* voip rainbow owns its pseudo-pkg */
    if (alarm_is_active()) return 0;    /* alarm owns its clock pkg */
    if (missed_is_active()) return 0;   /* missed owns "missed.call" */
    return 1;
}

static void notify_tick(void)
{
    static int cnt;
    /* all pool work lives here: screen-off flash, grace/expiry drops,
     * cap disarm, and picking the next entry after any of them */
    queue_arbitrate();
    if (!g_st.cur_pkg[0]) return;
    if (++cnt % 10 == 0) LOGI("armed tick");
}

/* Adaptive heartbeat: the core sleeps until the next REAL deadline.
 *   parked top + no NLS screen event -> 1s poll, because screen-off has
 *                              no uevent on this device and nobody told
 *                              us the state; bounded by the grace window
 *                              (queue_arbitrate drops the park once it ages)
 *   parked top + NLS screen event -> 0: the SCREEN 0 command from the app
 *                              IS the edge that flashes the park, so the
 *                              core just keeps its WATCHDOG_SEC safety pass.
 *   active entry with a cap -> wake exactly when it expires
 *   active entry, no cap    -> 0: no deadline at all, disarm is
 *                              event-driven (a CAN). The core turns that
 *                              into a single WATCHDOG_SEC safety pass -
 *                              never a 1s tick per second forever.
 * Returns 0 when nothing time-bound is pending. */
static long notify_next_wake(void)
{
    if (queue_has_hold() && !screen_event_driven())
        return NOTIFY_TICK_MS;      /* screen-off poll (grace-bounded) */
    long rem = queue_active_remain_ms();
    if (rem > 0)
        return rem;                 /* exact cap expiry */
    return 0;                       /* event-driven only */
}

REGISTER_HANDLER("*", notify_default_handle);
REGISTER_MODE_WAKE("notify", NOTIFY_TICK_MS, notify_owns, notify_tick, notify_next_wake);