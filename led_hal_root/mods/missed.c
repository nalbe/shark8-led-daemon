/*
 * mods/missed.c - missed-call LED, pure bridge events.
 *
 * The bridge classifies the dialer's missed-call tombstone (channel
 * "missed_calls") and forwards MISSED_ON / MISSED_OFF. The tombstone's
 * own open/close edge is the event.
 *
 * A missed call IS a notification (the dialer's own post), so it obeys
 * notification_light_pulse and the screen state like any notify entry.
 *
 * Config, all required keys:
 *   color            r,g,b
 *   max_sec          how long the LED may stay lit, 0 = unlimited
 *   the render= preset line (mode, per-channel currents, timing)
 * A missing key drops the tombstone entry and the log names it.
 */

#include <stdio.h>
#include <string.h>
#include "../chgd.h"

#define MISSED_LABEL "missed.call"

/* ---------------- [missed] config readers ---------------- */

/* [missed] max_sec: how long the LED may stay lit, seconds.
 * 0 = unlimited. Required key. */
static long missed_cap_ms(struct evt *e)
{
    long secs;
    (void)e;
    if (!conf_req_int("missed", "max_sec", 0, 86400, &secs)) return -1L;
    return secs * 1000L;
}

static int missed_paint(struct evt *e, struct evt_paint *p)
{
    int rgb[3];
    (void)e;
    if (!conf_req_color("missed", rgb)) return 0;
    /* led_event() resolves the active [missed] mode and paints from the
     * section's render= line. */
    PAINT_SEC(p, "missed");
    p->r = rgb[0];
    p->g = rgb[1];
    p->b = rgb[2];
    PAINT_LABEL(p, MISSED_LABEL);
    return 1;
}

static const struct evt_kind missed_kind = {
    .name     = "missed",
    .def_rank = RANK_MISSED,
    /* pulse-gated like a notification, screen-gated too */
    .flags  = EV_SINGLETON | EV_PULSE_GATED,
    .cap_ms = missed_cap_ms,
    .accept = NULL,
    .paint  = missed_paint,
};
REGISTER_EVT(missed_kind);



