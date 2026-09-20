/*
 * mods/voip.c - messenger call (VoIP) rainbow mode.
 *
 * Lights the traveling-wave rainbow while a messenger (Telegram /
 * WhatsApp / Viber / Signal) is in a call. The call detection lives in
 * the NotifyBridge app (com.bastet.notifybridge), which inspects every
 * notification as it posts and removes it: an incoming/active messenger
 * call is a notification on the messenger's call channel (Telegram:
 * "incoming_calls40", category CALL, audio usage VOICE_COMMUNICATION),
 * so call START and END are plain notification events - no audio-policy
 * polling, no dumpsys.
 *
 * The rainbow animation runs on the AW2033 chip (traveling-wave mode,
 * led_wave_rgb) - voip.c only arms it and lets the mode machinery
 * monitor for a missed VOIP_OFF; there is no per-tick color stepping.
 *
 * Transport (notify_bus socket), sent by NLS:
 *   VOIP_ON <pkg>     call notification posted    -> arm the rainbow
 *   VOIP_OFF <pkg>    call notification removed   -> disarm, channel back
 * The mode machinery only owns the LED while the mode is armed.
 * voip_try() keeps messenger chat messages from recoloring the LED
 * mid-call; chats on their own still take the normal color path.
 *
 * Safety net: the max_sec cap bounds a rainbow that never got its
 * VOIP_OFF (NLS died mid-call, event dropped). Counts from arming,
 * default 300s.
 *
 * Config: [voip] max_sec=<n> seconds (default VOIP_MAX_SEC); 0 = never
 *         auto-disarm the rainbow (a lost VOIP_OFF then sticks).
 *         [voip] packages=c1,c2,... overrides the messenger list.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../chgd.h"

#define VOIP_PKG      "voip.call"
#define VOIP_MAX_SEC  300         /* safety cap (s) for a missed VOIP_OFF */

/* default messenger packages (comma list string, also the [voip] fallback) */
#define VOIP_DEF_PKGS "org.telegram.messenger," \
                       "com.whatsapp," \
                       "com.viber.voip," \
                       "org.thoughtcrime.securesms," \
                       "com.snapchat," \
                       "com.google.android.apps.tachyon"

int voip_active(void)
{
    return g_st.cur_pkg[0] && !strcmp(g_st.cur_pkg, VOIP_PKG);
}

/* the configured messenger package list (for the per-marker checks) */
static const char *voip_pkg_list(void)
{
    const char *list = conf_get_str("voip", "packages");
    return (list && list[0]) ? list : VOIP_DEF_PKGS;
}

static int is_messenger(const char *pkg)
{
    if (!pkg || !pkg[0]) return 0;
    const char *list = voip_pkg_list();
    size_t pl = strlen(pkg);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t L = e ? (size_t)(e - p) : strlen(p);
        if (L == pl && !strncmp(p, pkg, L)) return 1;
        if (!e) break;
        p = e + 1;
        while (*p == ' ' || *p == ',') p++;
    }
    return 0;
}

/* safety cap for a rainbow that missed its VOIP_OFF, seconds; [voip]
 * max_sec in led.conf, default VOIP_MAX_SEC. 0 = never auto-disarm
 * (0 must mean unlimited here too - the old fallback silently turned it
 * back into VOIP_MAX_SEC, so "inf" was not configurable). */

static long voip_max_sec(void)
{
    long v = conf_get_int("voip", "max_sec", VOIP_MAX_SEC);
    return v > 0 ? v : 0;
}

static void voip_rgb(int *r, int *g, int *b)
{
    *r = 255; *g = 255; *b = 255;
    const char *c = conf_get_str("voip", "color");
    if (c && c[0] &&
        sscanf(c, "%d,%d,%d", r, g, b) == 3) {
        if (*r < 0) *r = 0; if (*r > 255) *r = 255;
        if (*g < 0) *g = 0; if (*g > 255) *g = 255;
        if (*b < 0) *b = 0; if (*b > 255) *b = 255;
    }
}

/* ---------------- arming / disarming (event-driven) ---------------- */

static void arm_voip(void)
{
    g_applied_band[0] = '\0';       /* charge leds must reapply after */
    leds_all_off();                 /* kill breathing, all channels 0 */
    snprintf(g_st.cur_pkg, sizeof(g_st.cur_pkg), "%s", VOIP_PKG);
    g_st.armed_at = time(NULL);
    int r, g, b;
    voip_rgb(&r, &g, &b);
    const char *engine = led_event("voip", r, g, b);
    status_write("voip", "", "voip.call", r, g, b, engine);
    retune_timer();
    LOGI("voip ON -> wave rainbow");
}

/* Called over the NLS socket when a messenger's call notification posts. */
int voip_on(void)
{
    if (voip_active()) return 1;    /* already running: keep it */
    arm_voip();
    return 1;
}

/* Called over the NLS socket when that call notification is removed. */
int voip_off(void)
{
    if (voip_active()) disarm_notification(&g_st, "voip end");
    return 0;
}

/* ---------------- mode ---------------- */

static int voip_owns(const char *pkg)
{
    return pkg && !strcmp(pkg, VOIP_PKG);
}

static void voip_tick(void)
{
    /* rainbow runs on the chip; nothing to step. The mode machinery
     * still calls voip_tick on the timer so the safety net can fire. */
    /* missed VOIP_OFF safety net: bound the rainbow even if NLS vanished */
    long cap = voip_max_sec();
    if (cap > 0 && difftime(time(NULL), g_st.armed_at) >= (double)cap)
        disarm_notification(&g_st, "voip safety cap");
}

/* adaptive wakeup: sleep exactly until the [voip] max_sec cap; cap=0
 * means unlimited (never auto-disarm) and the end is purely event-driven
 * (VOIP_OFF), so no timer at all - the core sleeps with the rainbow held */
static long voip_next_wake(void)
{
    long cap = voip_max_sec();
    if (cap <= 0) return 0;
    double age = difftime(time(NULL), g_st.armed_at);
    long remain = (long)(cap - age);
    if (remain < 1) remain = 1;
    return remain * 1000L;
}

/* Called by notify's default handler before it colors a package.
 * Returns 1 if voip owns the led (mid-call: never let a chat recolor the
 * rainbow), 0 for a normal chat message. Call START/END themselves are
 * event-driven: NLS sends VOIP_ON/VOIP_OFF ahead of the ENQ/CAN. */
int voip_try(const char *pkg)
{
    if (!is_messenger(pkg)) return 0;     /* not ours -> normal color */
    if (voip_active()) return 1;          /* mid-call: keep the rainbow */
    return 0;                              /* chat message -> normal color */
}

REGISTER_MODE_WAKE("voip", 1000, voip_owns, voip_tick, voip_next_wake);