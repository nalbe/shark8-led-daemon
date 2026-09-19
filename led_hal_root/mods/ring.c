/*
 * mods/ring.c - incoming/outgoing call rainbow mode.
 *
 * Entered on a RING_ON command from the NLS bridge (it classifies the
 * dialer's live-call notification) and exited on RING_OFF, which resolves
 * the outcome: incoming -> missed-call verification window, outgoing ->
 * plain charge leds. Registers a timer MODE: while the pseudo-package is
 * armed, the core drops the timer to our heartbeat (RING_STEP_MS) and
 * calls ring_tick() on every tick. Ring mode deliberately ignores the
 * screen state (this ROM wakes the display for incoming calls and the
 * screen-on guard would kill the effect); it exits on RING_OFF, timeout,
 * or explicit disarm.
 *
 * The rainbow itself runs on the AW2033 chip (traveling-wave mode via
 * led_wave_rgb): ring.c only arms it and resolves the outcome on end.
 * No per-tick LED code here. Everything is event-driven from the NLS
 * bridge - there is no telephony/dumpsys polling and no logdr focus
 * tracking anymore.
 *
 * Config: [ring] test_sec=<n> overrides the test-rainbow hold (default
 * RING_TEST_SEC), max_sec=<n> caps a live ring rainbow (default 0 =
 * unlimited; a capped incoming ring resolves into the missed-call
 * check). Looked up through config.c's generic kv store.
 *
 * The mode pattern is the extension seam for any LED "behavior" that is
 * driven by the timer rather than by a single arm/disarm:
 *   REGISTER_MODE("name", heartbeat_ms, owns_pkg_fn, tick_fn);
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../chgd.h"

#define INCOMING_PKG   "incoming.call"   /* pseudo-pkg: ring rainbow */
#define RING_STEP_MS   1000      /* tick: cap checks only (chip animates) */
#define RING_TEST_SEC  30        /* default test-rainbow hold ([ring] test_sec) */

/* test-rainbow hold, seconds; overridable via [ring] test_sec in led.conf */
static long ring_test_hold(void)
{
    long v = conf_get_int("ring", "test_sec", RING_TEST_SEC);
    return v > 0 ? v : RING_TEST_SEC;
}

/* hard cap for a LIVE ring rainbow, seconds; [ring] max_sec in led.conf.
 * 0 (default) = unlimited: the rainbow runs for the whole telephony
 * call. A cap only bounds a pathological stuck state, and unlike the
 * old RING_MAX_SEC path it resolves the outcome instead of silently
 * killing the LED. */
static long ring_max_sec(void)
{
    long v = conf_get_int("ring", "max_sec", 0);
    return v > 0 ? v : 0;
}

static void ring_rgb(int *r, int *g, int *b)
{
    *r = 255; *g = 255; *b = 255;
    const char *c = conf_get_str("ring", "color");
    if (c && c[0] &&
        sscanf(c, "%d,%d,%d", r, g, b) == 3) {
        if (*r < 0) *r = 0; if (*r > 255) *r = 255;
        if (*g < 0) *g = 0; if (*g > 255) *g = 255;
        if (*b < 0) *b = 0; if (*b > 255) *b = 255;
    }
}

/* ---------------- mode state ---------------- */

/* 1 if the current rainbow was caused by an incoming (RINGING) call;
 * 0 for an outgoing (OFFHOOK) call. On end, incoming resolves to a missed
 * call check, outgoing just drops back to the charge leds. */
static int g_ring_incoming;
/* 1 when armed from a test hook: no live call semantics, the rainbow is
 * held for RING_TEST_SEC, or until Disarm / USR2. */
static int g_ring_test;

int ring_is_active(void)
{
    return g_st.cur_pkg[0] && !strcmp(g_st.cur_pkg, INCOMING_PKG);
}

void arm_ring_ex(int incoming, int test)
{
    /* Duplicate RING_ON dedup: Android re-posts the dialer call
     * notification on every state refresh, so NLS fires RING_ON for
     * each post. Re-arming the already-running rainbow restarts the
     * chip wave from phase 0 -> the LED visibly strobes mid-ring.
     * Same-direction re-arms are pure noise: keep the current rainbow.
     * A direction change (call answered: incoming -> outgoing) only
     * flips the resolution flag for RING_OFF, no re-paint. Test arms
     * always come from a fresh test_disarm(), so the guard never
     * swallows them. */
    if (!test && ring_is_active()) {
        if (g_ring_incoming == incoming)
            return;
        g_ring_incoming = incoming;         /* reclassify, keep the wave */
        LOGI("ring reclassified: %s",
             incoming ? "incoming" : "outgoing");
        return;
    }
    g_applied_band[0] = '\0';       /* charge leds must reapply after */
    leds_all_off();                 /* kill breathing, all channels 0 */
    g_ring_incoming = incoming;
    g_ring_test = test;
    snprintf(g_st.cur_pkg, sizeof(g_st.cur_pkg), "%s", INCOMING_PKG);
    g_st.armed_at = time(NULL);
    /* [ring] renderer: mode from [ring]/chip sections, color from the
     * section itself, timing from the [ring.breath]/[ring.wave] chips */
    int r, g, b;
    ring_rgb(&r, &g, &b);
    const char *engine = led_event("ring", r, g, b);
    status_write("ring", "",
                 incoming ? "incoming.call" : "outgoing.call",
                 r, g, b, engine);
    retune_timer();
    LOGI("ring armed -> wave rainbow (%s%s)",
         incoming ? "incoming" : "outgoing",
         test ? ", test" : "");
}

void arm_ring(int incoming)
{
    arm_ring_ex(incoming, 0);
}

/* ---------------- call end (RING_OFF / live cap) ---------------- */

/* Resolve a live call end: drop the rainbow, repaint the charge leds,
 * and for an incoming call reopen the missed-call verification window
 * (the missed row may land right now). Event-driven: ring_tick -> cap,
 * nls_cmd -> RING_OFF. */
static void ring_resolve(const char *why)
{
    g_st.cur_pkg[0] = '\0';
    apply_charge_leds();
    if (g_ring_incoming)
        dialer_reopen_window();
    LOGI("ring ended (%s)%s", why, g_ring_incoming ? " -> missed check" : "");
    retune_timer();
}

/* NLS RING_OFF: the dialer's last live-call notification is gone. Resolve
 * the outcome exactly like a call end. */
void ring_off(void)
{
    if (!ring_is_active()) return;
    if (g_ring_test) {
        disarm_notification(&g_st, "ring_off");
        return;
    }
    ring_resolve("RING_OFF");
}

/* ---------------- mode tick ---------------- */

static int ring_owns(const char *pkg)
{
    return pkg && !strcmp(pkg, INCOMING_PKG);
}

static void ring_tick(void)
{
    if (g_ring_test) {
        double age = difftime(time(NULL), g_st.armed_at);
        if (age >= (double)ring_test_hold())
            disarm_notification(&g_st, "test timeout");
        return;
    }
    /* [ring] max_sec: optional hard cap for the live rainbow.
     * 0 = unlimited; when a cap IS set, hitting it resolves the outcome
     * exactly like a RING_OFF, so a missed call landing right at the cap
     * is still caught. The normal end of a live call is event-driven
     * (RING_OFF from the NLS bridge), no polling here. */
    long cap = ring_max_sec();
    if (cap > 0 &&
        difftime(time(NULL), g_st.armed_at) >= (double)cap)
        ring_resolve("max_sec");
}

REGISTER_MODE("ring", RING_STEP_MS, ring_owns, ring_tick);