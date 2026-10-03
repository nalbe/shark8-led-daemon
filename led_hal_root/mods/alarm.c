/*
 * mods/alarm.c - alarm clock LED handling with its own config.
 *
 * A ringing clock is a bridge event, like the other planes: the bridge
 * classifies the notification by its own category ("alarm") and emits
 * ALARM_ON / ALARM_OFF. The alarm ring is its own kind, with its own
 * colour and its own budget.
 *
 * An alarm always wakes the screen, so a screen-on guard would park it and
 * only flash it after the display fell asleep again: EV_SCREEN_BYPASS.
 *
 * Own [alarm] section in led.conf:
 *   color            r,g,b                              (REQUIRED)
 *   max_sec          how long the LED may stay lit, 0 = unlimited (REQUIRED)
 *   the render= preset line (mode, per-channel currents, timing)
 * A missing key drops the alarm entry and the log names it.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../chgd.h"

/* pseudo-package a live alarm never carries: only the GUI test has no clock
 * app behind it and needs a stable label in the status file. */
#define ALARM_LABEL     "alarm.call"

/* [alarm] max_sec: how long the LED may stay lit, seconds.
 * 0 = unlimited. Required key. */
static long alarm_cap_ms(struct evt *e)
{
    long secs;
    (void)e;
    if (!conf_req_int("alarm", "max_sec", 0, 86400, &secs)) return -1L;
    return secs * 1000L;
}

static int alarm_paint(struct evt *e, struct evt_paint *p)
{
    int rgb[3];
    if (!conf_req_color("alarm", rgb)) return 0;
    /* led_event() resolves the active [alarm] mode and paints from the
     * section's render= line. */
    PAINT_SEC(p, "alarm");
    p->r = rgb[0];
    p->g = rgb[1];
    p->b = rgb[2];
    /* the clock app that rang, from the bridge's ALARM_ON; the GUI test
     * entry carries no package and paints under a stable label */
    PAINT_LABEL(p, e->pkg[0] ? e->pkg : ALARM_LABEL);
    return 1;
}

static const struct evt_kind alarm_kind = {
    .name     = "alarm",
    .def_rank = RANK_ALARM,
    .flags    = EV_SINGLETON | EV_SCREEN_BYPASS,
    .cap_ms   = alarm_cap_ms,
    .accept   = NULL,
    .paint    = alarm_paint,
};
REGISTER_EVT(alarm_kind);



