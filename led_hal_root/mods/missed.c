/*
 * mods/missed.c - missed-call LED, pure bridge events.
 *
 * The NLS bridge classifies the dialer's missed-call tombstone
 * (channel "missed_calls") and forwards it as MISSED_ON <id> /
 * MISSED_OFF <id> - the SAME channel marker this mod used to verify by
 * forking a `content query` on content://call_log/calls. That read is
 * gone: the open/close edge of the tombstone IS the event, replay on
 * connect is handled by the bridge, and the daemon never touches the
 * call_log (no root-requiring queries, no verification windows, no
 * dedup tables - the bridge owns the live set and only emits on real
 * transitions).
 *
 * The live-call rainbow is still the ring mod's job (RING_ON/RING_OFF).
 * A call end with g_ring_incoming used to reopen a verification window
 * so the freshly-landed missed row could be caught; with the
 * event-driven bridge there is nothing to reopen - the tombstone emits
 * MISSED_ON on its own the moment it posts.
 *
 * Missed-call LED: owns its own [missed] section in led.conf,
 * independent of [notify] and [ring]:
 *   color            r,g,b                          (def 0,0,255 blue)
 *   max_sec          LED max duration (s), 0=unlim   (def 1800)
 *   mode + [missed.solid/breath/wave] chip sections  (mode; each chip
 *            owns its own rise/hold/fall/offt keys)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../chgd.h"

#define MISSED_PKG     "missed.call"

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
    g_st.owner_pkg[0] = '\0';
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
 * cap=0 means permanent and disarm is purely event-driven (MISSED_OFF) */
static long missed_next_wake(void)
{
    long cap = missed_max_sec();
    if (cap <= 0) return 0;
    double age = difftime(time(NULL), g_st.armed_at);
    long remain = (long)(cap - age);
    if (remain < 1) remain = 1;
    return remain * 1000L;
}

/* ---------------- NLS events ---------------- */

/* MISSED_ON <id>: the bridge saw a dialer missed-call tombstone post.
 * A repost of one already showing just re-arms idempotently. */
void missed_on(void)
{
    missed_paint();
}

/* MISSED_OFF <id>: the tombstone left (user cleared it / opened the
 * log / a newer one replaced it). Only disarm what we own. */
void missed_off(void)
{
    if (!missed_is_active()) {
        LOGI("missed-off: not armed, ignored");
        return;
    }
    disarm_notification(&g_st, "missed-off");
    retune_timer();
}

REGISTER_MODE_WAKE("missed", 1000, missed_owns, missed_tick, missed_next_wake);