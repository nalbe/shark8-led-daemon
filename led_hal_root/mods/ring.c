/*
 * mods/ring.c - incoming/outgoing call rainbow.
 *
 * RING_ON from the bridge pushes this kind's entry, RING_OFF drops it.
 * A missed call arrives as MISSED_ON on its own kind.
 *
 * A live call never waits behind the screen: the pool is not allowed to
 * park a kind that declares EV_SCREEN_BYPASS, so a ringing phone lights
 * up with the display on.
 *
 * Config: [ring] max_sec=<n> and the [ring] render= line, both required.
 * max_sec is the budget of lit time: how long the rainbow may hold the
 * LEDs, 0 = unlimited (until RING_OFF).
 */

#include <stdio.h>
#include <string.h>
#include "../chgd.h"

/* the dialer reposts its call notification on every state refresh, so the
 * same event arrives again and again: the label below is what the status
 * file shows, the direction travels as the entry's payload */
#define INCOMING_LABEL  "incoming.call"
#define OUTGOING_LABEL  "outgoing.call"

/* [ring] max_sec: the budget of lit time, seconds. 0 = unlimited.
 * Required key - without it the rainbow cannot be budgeted, so the pool
 * drops the entry instead of blinking with an invented lifetime. */
static long ring_cap_ms(struct evt *e)
{
    long secs;
    (void)e;
    if (!conf_req_int("ring", "max_sec", 0, 86400, &secs)) return -1L;
    return secs * 1000L;
}

/* the wave, described: colours from [ring], timing from the section's
 * render= line. The pool paints it and writes the status file. */
static int ring_paint(struct evt *e, struct evt_paint *p)
{
    int rgb[3];
    if (!conf_req_color("ring", rgb)) return 0;
    PAINT_SEC(p, "ring");
    p->r = rgb[0];
    p->g = rgb[1];
    p->b = rgb[2];
    /* RING_ON carries 1 incoming / 0 outgoing; a direction flip (answered)
     * only relabels the entry, the wave keeps running */
    PAINT_LABEL(p, strcmp(e->arg, "outgoing") ? INCOMING_LABEL : OUTGOING_LABEL);
    return 1;
}

static const struct evt_kind ring_kind = {
    .name     = "ring",
    .def_rank = RANK_RING,
    .flags    = EV_SINGLETON | EV_SCREEN_BYPASS,
    .cap_ms   = ring_cap_ms,
    .accept   = NULL,
    .paint    = ring_paint,
};
REGISTER_EVT(ring_kind);



