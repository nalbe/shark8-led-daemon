/*
 * mods/notify.c - notification pipeline mod.
 *
 * Claims every notification the plugins did not take, and owns the
 * ARMED channel of the daemon:
 *   REGISTER_HANDLER("*", ...)  default handler: suppression filter,
 *                               then an unconditional pool push
 *   REGISTER_MODE_WAKE("notify", ...) owns any armed real package while
 *                               ring/charge own their own channels;
 *                               adaptive wake: the cap deadline or
 *                               nothing (event-driven disarm)
 *
 * All scheduling lives in mods/queue.c: push puts the event in the
 * ledger, queue_arbitrate() picks the freshest, parks a top that lands
 * on a lit screen (no timer - the bridge's SCREEN 0 event flashes it on
 * screen-off or the grace drop kills it), preempts with resume credit,
 * and caps the accumulated show time. This file only gates (suppress /
 * light toggle) and paints (rgb + [sec] renderer via
 * arm_notification_ex).
 *
 * REGISTER_HANDLER(EXACT, ...) handlers (alarm) still claim first -
 * their events never reach the pool; the pseudo-package colors
 * (missed.call) come from their own mods.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "../chgd.h"

/* The notify mode has no tick of its own: the pool policy IS
 * queue_arbitrate(), invoked directly as the mode tick by the core.
 * The adaptive wake (notify_next_wake) keeps the core asleep until the
 * cap deadline, so this runs on the deadline, not as a heartbeat. */

/* ---------------- registry lookups ---------------- */

/* colour for a package: [rules] in led.conf, then any built-in registry
 * entry (extension point; currently none), then the [notify] default_color.
 * Returns 1 when a per-package rule supplied the color (the "app" preset
 * is used), 0 for the shared default color (the "default" preset). */
static int rgb_for(const char *pkg, int *r, int *g, int *b)
{
    if (conf_pkg_rgb(pkg, r, g, b)) return 1;
    const struct led_rule *lr;
    for (lr = __start_chgd_rules; lr->pkg; lr++)
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
 * line per package per window, regardless of event frequency. */
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
    if (!pulse_on()) {
        LOGI("notification light off: %s dropped", pkg);
        return;
    }
    int r, g, b;
    /* rule-matched packages run their OWN preset when the rule carries an
     * extended tail (synthetic [notify.<pkg>]), otherwise the legacy
     * shared [notify.app]; everything else runs the shared [notify]
     * default preset. Color always comes from the rule. */
    int app  = rgb_for(pkg, &r, &g, &b);
    const char *sec = app ? conf_notify_sec(pkg) : "notify";
    LOGI("arm detect: %s -> [%s] rgb=%d,%d,%d (rules|default)",
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
 * arbitrate. The voip/alarm planes claim their packages BEFORE this
 * handler via exact-match registration, so a messenger call chat message
 * still lands here normally and simply waits its turn. */
static int notify_default_handle(const char *pkg, int id)
{
    conf_maybe_reload();            /* pick up edited led.conf on the fly */
    if (suppressed(pkg)) {
        if (supp_log_ok(pkg))
            LOGI("suppressed: %s", pkg);
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
     * (notify_next_wake): with a parked top the SCREEN 0 command from
     * the bridge IS the edge, so we sleep until the next real cap
     * deadline or nothing at all (event-driven disarm). */
    if (!pkg || !pkg[0])
        return queue_has_pending() || queue_active();
    if (ring_is_active()) return 0;     /* ring owns its pseudo-pkg */
    if (voip_active()) return 0;        /* voip rainbow owns its pseudo-pkg */
    if (alarm_is_active()) return 0;    /* alarm owns its clock pkg */
    if (missed_is_active()) return 0;   /* missed owns "missed.call" */
    return 1;
}

/* Adaptive heartbeat: the core sleeps until the next REAL deadline.
 *   active entry with a cap    -> wake exactly when it expires
 *   active entry, no cap       -> 0: no deadline at all, disarm is
 *                                  event-driven (a CAN) and the timer
 *                                  stays disarmed - the LED just sits
 *                                  there and the phone sleeps.
 * A parked top has no wakeup of its own: the bridge's SCREEN 0 command
 * IS the edge that flashes the park, so the core's timer stays disarmed
 * until a cancel, a cap deadline, or a new post re-arbitrates.
 * Returns 0 when nothing time-bound is pending. */
static long notify_next_wake(void)
{
    long rem = queue_active_remain_ms();
    if (rem > 0)
        return rem;                 /* exact cap expiry */
    return 0;                       /* event-driven only */
}

/* The mode tick IS the pool policy itself - queue_arbitrate() is
 * registered directly as the tick, no wrapper function. */
REGISTER_HANDLER("*", notify_default_handle);
REGISTER_MODE_WAKE("notify", 1000, notify_owns, queue_arbitrate, notify_next_wake);