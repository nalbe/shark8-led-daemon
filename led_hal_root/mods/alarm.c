/*
 * mods/alarm.c - alarm clock LED handling with its own config.
 *
 * Claims every clock app notification via REGISTER_HANDLER so the alarm
 * ring lights up independently of ordinary notifications. An alarm always
 * wakes the screen (and keeps it on for the whole ring), so notify.c's
 * screen-on guard would park the event and only flash after the screen
 * falls asleep - i.e. almost never for an active alarm. Here the alarm
 * arms immediately, bypassing the park.
 *
 * Unlike notify, the alarm owns its own [alarm] section in led.conf, so
 * it is never confused with any other app:
 *   [alarm]
 *   color            r,g,b                                    (def 255,155,0)
 *   max_sec          LED max duration in seconds, 0=unlimited (def 0)
 *   mode + [alarm.solid/breath/wave] chip sections (each chip owns
 *            its own rise/hold/fall/offt keys; mode)
 * Defaults mirror [notify]'s builtins so the alarm behaves like every
 * app out of the box, but the user can tune it without touching [notify]
 * or [rules].
 *
 * REGISTER_HANDLER: exact match is walked before the "*" default in
 * notify.c, so the alarm claims its own packages and the core never
 * changes for this feature.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../chgd.h"

#define ALARM_PKG_GOOGLE "com.google.android.deskclock"
#define ALARM_PKG_AOSP   "com.android.deskclock"

#define ALARM_DEF_R 255
#define ALARM_DEF_G 155
#define ALARM_DEF_B 0

static void alarm_rgb(int *r, int *g, int *b)
{
    *r = ALARM_DEF_R; *g = ALARM_DEF_G; *b = ALARM_DEF_B;
    const char *c = conf_get_str("alarm", "color");
    if (c && c[0] &&
        sscanf(c, "%d,%d,%d", r, g, b) == 3) {
        if (*r < 0) *r = 0; if (*r > 255) *r = 255;
        if (*g < 0) *g = 0; if (*g > 255) *g = 255;
        if (*b < 0) *b = 0; if (*b > 255) *b = 255;
    }
}

/* max led duration in seconds, 0 = unlimited, decoupled from notify. */
static long alarm_max_sec(void)
{
    return conf_get_int("alarm", "max_sec", 0);
}

/* ---------------- armed mode ---------------- */

/* true while an alarm owns the led (so notify/others defer to it) */
static int alarm_owns(const char *pkg)
{
    return pkg &&
           (!strcmp(pkg, ALARM_PKG_GOOGLE) || !strcmp(pkg, ALARM_PKG_AOSP)) &&
           g_st.cur_pkg[0] && !strcmp(g_st.cur_pkg, pkg);
}

/* 1 if the alarm mode is armed right now (mirrors ring/voip_active) */
int alarm_is_active(void)
{
    return g_st.cur_pkg[0] &&
           (!strcmp(g_st.cur_pkg, ALARM_PKG_GOOGLE) ||
            !strcmp(g_st.cur_pkg, ALARM_PKG_AOSP));
}

static void alarm_paint(const char *pkg)
{
    int r, g, b;
    alarm_rgb(&r, &g, &b);

    LOGI("alarm arm: %s -> rgb=%d,%d,%d", pkg, r, g, b);
    g_applied_band[0] = '\0';       /* charge leds must reapply after */

    /* led_event() resolves the active [alarm] mode and paints via
     * [alarm.*] chip sections; timing comes from the chip sections. */
    const char *engine = led_event("alarm", r, g, b);

    snprintf(g_st.cur_pkg, sizeof(g_st.cur_pkg), "%s", pkg);
    g_st.armed_at = time(NULL);
    g_st.test = 0;
    status_write("alarm", "", pkg, r, g, b, engine);
    retune_timer();
}

static int alarm_handle(const char *pkg, int id)
{
    (void)id;
    if (ring_is_active() || voip_active())
        return 1;                      /* call rainbow owns the led now */
    if (alarm_is_active())
        return 1;                      /* already ringing: keep it */
    alarm_paint(pkg);
    return 1;
}

/* adaptive wakeup: sleep until the [alarm] max_sec cap expires; cap=0
 * means permanent and disarm is purely event-driven (cancel) */
static long alarm_next_wake(void)
{
    long cap = alarm_max_sec();
    if (cap <= 0) return 0;
    double age = difftime(time(NULL), g_st.armed_at);
    long remain = (long)(cap - age);
    if (remain < 1) remain = 1;
    return remain * 1000L;
}

static void alarm_tick(void)
{
    long cap = alarm_max_sec();
    if (cap > 0 && difftime(time(NULL), g_st.armed_at) >= (double)cap) {
        LOGI("alarm timeout");
        disarm_notification(&g_st, "alarm timeout");
    }
}

REGISTER_HANDLER(ALARM_PKG_GOOGLE, alarm_handle);
REGISTER_HANDLER(ALARM_PKG_AOSP, alarm_handle);
REGISTER_MODE_WAKE("alarm", 1000, alarm_owns, alarm_tick, alarm_next_wake);
