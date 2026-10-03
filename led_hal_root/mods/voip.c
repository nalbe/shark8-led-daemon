/*
 * mods/voip.c - messenger call (VoIP) rainbow.
 *
 * Transport (notify_bus socket), sent by NLS:
 *   VOIP_ON <pkg>     call notification posted    -> push the entry
 *   VOIP_OFF <pkg>    call notification removed   -> drop this entry
 * VOIP_OFF is pkg-matched: another messenger's call ending must not kill
 * the rainbow of the one that is still up.
 *
 * Config: [voip] max_sec and [voip] color, both required. max_sec is the
 * budget of lit time: how long the rainbow may hold the LEDs, 0 =
 * unlimited.
 */

#include <stdio.h>
#include <string.h>
#include "../chgd.h"

#define VOIP_LABEL "voip.call"

/* ---------------- config ---------------- */

/* [voip] max_sec: how long the rainbow may hold the LEDs, seconds.
 * 0 = unlimited. Required key. */
static long voip_cap_ms(struct evt *e)
{
    long secs;
    (void)e;
    if (!conf_req_int("voip", "max_sec", 0, 86400, &secs)) return -1L;
    return secs * 1000L;
}

/* [voip] color is required: without a base colour there is no rainbow. */
static int voip_paint(struct evt *e, struct evt_paint *p)
{
    int rgb[3];
    (void)e;
    if (!conf_req_color("voip", rgb)) return 0;
    PAINT_SEC(p, "voip");
    p->r = rgb[0];
    p->g = rgb[1];
    p->b = rgb[2];
    PAINT_LABEL(p, VOIP_LABEL);
    return 1;
}

static const struct evt_kind voip_kind = {
    .name     = "voip",
    .def_rank = RANK_VOIP,
    .flags    = EV_SINGLETON | EV_SCREEN_BYPASS,
    .cap_ms   = voip_cap_ms,
    .accept   = NULL,
    .paint    = voip_paint,
};
REGISTER_EVT(voip_kind);



