/*
 * led.c - thin adapter that turns events + led.conf into AW2033 calls.
 *
 * Every event owns its [sec] section, and one render= line in it carries
 * the whole renderer:
 *
 *   render=<r,g,b>,<mode>,<cur_r,g,b>,pattern,<sync>,<repeat>,
 *          <cur_r,g,b>,<rise>,<hold>,<fall>,<offt>,<t0_r,g,b>
 *
 * config.c expands that line into the flat keys below (led.c never sees
 * the line itself):
 *
 *   [sec]         mode=off|solid|breath|wave
 *   [sec.solid]   cur=r,g,b         0..15 per channel current
 *   [sec.pattern] repeat=0..15, cur_r/cur_g/cur_b,
 *                 rise/hold/fall/offt (ms), sync=0|1,
 *                 t0=r,g,b phase offsets (wave only)
 *
 * sync (breath/wave): the AW2033's per-channel pattern controllers
 * free-run on their own T0..T4, and because the rise/fall period grows
 * almost linearly with the PWM amplitude, channels at different PWM
 * levels drift out of phase over time. Setting LCFG0.SYNC (master =
 * channel 0, red) makes the chip slave channels 1/2 to the master:
 * the PWM written to channel 0 becomes the common amplitude for all
 * three, per-channel CUR still applies, and the phases stay locked.
 * Trade-off: in sync mode the color is expressed through the per-channel
 * cur ratio, not the rgb PWM (rgb green/blue are ignored). Default 0.
 * The bit is managed explicitly on every paint (set in breath/wave to
 * the config value, cleared on solid/off).
 *
 * Timing (rise/hold/fall/offt) belongs to the preset, shared by breath and
 * wave. The [led] section keeps only chip/daemon globals:
 * logging, imax.
 *
 * The breathing, traveling-wave and solid modes all run inside the chip
 * itself via the aw2033 controller (the vendored lib/libaw2033.a +
 * lib/aw2033.h from the standalone aw2033-driver repo). Nothing else in
 * the daemon touches the chip: the event pool (channel.c) is the only
 * caller of led_event() / leds_all_off(), and this file owns the
 * fingerprint of what is currently programmed, so a repost of the same
 * payload keeps the running pattern instead of restarting it.
 *
 * Events only supply the color; everything else (timing, current, phase
 * offsets, repeat) comes from the section's render= line. config.c treats
 * any unknown [section] as mod-owned, so all of these read raw.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chgd.h"
#include "../lib/aw2033.h"

/* ---------------- strict preset readers ----------------
 *
 * A preset key that is absent or illegal is a broken preset.
 * Every reader below reports the offending key and fails, and the
 * caller skips the event.
 *
 * Out of range is the same kind of broken: silently clamping cur=99 to 15
 * or rise=99999 to the chip maximum would paint something the file never
 * asked for, so a value outside its legal window is rejected and logged
 * like a missing key. */

/* required integer key inside lo..hi: absent, non-numeric or out of
 * range -> warn + fail */
static int req_int(const char *sec, const char *key, long lo, long hi,
                   long *out)
{
    const char *v = conf_get_str(sec, key);
    if (!v || !v[0]) {
        LOGI("warn: [led] %s: missing key %s", sec, key);
        return 0;
    }
    char *end = NULL;
    long x = strtol(v, &end, 10);
    if (end == v || *end) {
        LOGI("warn: [led] %s: key %s=\"%s\" is not a number", sec, key, v);
        return 0;
    }
    if (x < lo || x > hi) {
        LOGI("warn: [led] %s: key %s=%ld is outside %ld..%ld",
             sec, key, x, lo, hi);
        return 0;
    }
    *out = x;
    return 1;
}

/* required "a,b,c" key, every channel inside lo..hi; absent, malformed
 * or out of range -> warn + fail. One key per call, so a section with
 * three broken keys says so three times. */
static int req_triple(const char *sec, const char *key,
                      int *out, int lo, int hi)
{
    const char *v = conf_get_str(sec, key);
    if (!v || !v[0]) {
        LOGI("warn: [led] %s: missing key %s", sec, key);
        return 0;
    }
    int a, b, c;
    if (sscanf(v, "%d,%d,%d", &a, &b, &c) != 3) {
        LOGI("warn: [led] %s: key %s=\"%s\" is not \"a,b,c\"", sec, key, v);
        return 0;
    }
    if (a < lo || a > hi || b < lo || b > hi || c < lo || c > hi) {
        LOGI("warn: [led] %s: key %s=%d,%d,%d has a channel outside %d..%d",
             sec, key, a, b, c, lo, hi);
        return 0;
    }
    out[0] = a; out[1] = b; out[2] = c;
    return 1;
}

/* ---------------- AW2033 core handle ---------------- */

/* one shared chip handle; opened lazily on first use and kept for the
 * process lifetime. aw_reg_get/set re-open the reg sysfs file each call
 * anyway (the kernel driver owns the fd), so this is just cheap state. */
static aw_chip *g_aw;

static aw_chip *led_hw(void)
{
    if (!g_aw)
        g_aw = aw_open("red");
    if (!g_aw)
        LOGI("[led] aw2033 not reachable (%s)", g_reg_path);
    return g_aw;
}

/* active [sec] renderer mode: explicit led.conf key only. A section
 * without mode= is a broken config, so it is reported and reported as
 * NULL - the caller skips the event. */
static const char *led_active_mode(const char *sec)
{
    const char *mode = conf_get_str(sec, "mode");
    if (!mode || !mode[0]) {
        LOGI("warn: [led] %s: missing key mode", sec);
        return NULL;
    }
    return mode;
}

/* the chip preset section to render from: [sec.pattern], the section
 * config.c built out of the section's render= line. */
static int led_pattern_sec(const char *sec, char *out, size_t n)
{
    snprintf(out, n, "%s.pattern", sec);
    if (conf_sec_exists(out)) return 1;
    LOGI("warn: [led] %s: no [%s] - the render= line was not read, "
          "event skipped", sec, out);
    return 0;
}

/* optional "a,b,c" reader: 0 when the key is absent or malformed, and
 * the caller keeps its own value. Only for genuinely optional extras
 * (the wave cur= override a hand-written config may add). */
static int read_triple(const char *sec, const char *key,
                        int *out, int lo, int hi)
{
    const char *s = conf_get_str(sec, key);
    if (!s) return 0;
    int a, b, c;
    if (sscanf(s, "%d,%d,%d", &a, &b, &c) != 3) return 0;
    if (a < lo || a > hi || b < lo || b > hi || c < lo || c > hi) return 0;
    out[0] = a; out[1] = b; out[2] = c;
    return 3;
}

/* steady RGB: manual mode on the chip. Per-channel current from
 * [sec.solid] cur=r,g,b (0..15); color maps to PWM amplitude. */
static void led_solid_rgb(const char *sec, int r, int g, int b)
{
    aw_chip *c = led_hw();
    char ss[128];
    int cur[3];
    snprintf(ss, sizeof(ss), "%s.solid", sec);
    if (!req_triple(ss, "cur", cur, 0, 15)) return;
    LOGI("[led] %s solid rgb=%d,%d,%d cur=%d,%d,%d", sec, r, g, b,
         cur[0], cur[1], cur[2]);
    if (!c) return;
    /* manual mode ignores the LCFG0.SYNC master bit, but a stale bit
     * would leak into the next pattern arm (aw_breathe_ex preserves it),
     * so always drop it on solid. */
    aw_sync_mode(c, 0);
    aw_solid(c, r, g, b, cur[0], cur[1], cur[2]);
}

/* breathing RGB on the chip: synchronized pattern start. The timing
 * lives in the shared [sec.pattern] chip section (rise/hold/fall/offt);
 * repeat/cur are read from the same section. sync=1 turns on the chip's
 * built-in master sync (LCFG0.SYNC): all channels dim on PWM channel 0
 * (master red), per-channel cur still applies. */
static void led_breathe_rgb(const char *sec, int r, int g, int b)
{
    aw_chip *c = led_hw();
    char ss[128];
    long rise, hold, fall, offt, repeat, cur0, cur1, cur2, sync;
    if (!led_pattern_sec(sec, ss, sizeof(ss))) return;
    if (!req_int(ss, "rise",   0, AW_MAX_DELAY_MS, &rise))   return;
    if (!req_int(ss, "hold",   0, AW_MAX_DELAY_MS, &hold))   return;
    if (!req_int(ss, "fall",   0, AW_MAX_DELAY_MS, &fall))   return;
    if (!req_int(ss, "offt",   0, AW_MAX_DELAY_MS, &offt))   return;
    if (!req_int(ss, "repeat", 0, AW_TIME_CODES - 1, &repeat)) return;
    if (!req_int(ss, "cur_r",  0, 15, &cur0)) return;
    if (!req_int(ss, "cur_g",  0, 15, &cur1)) return;
    if (!req_int(ss, "cur_b",  0, 15, &cur2)) return;
    if (!req_int(ss, "sync",   0, 1,  &sync))  return;
    LOGI("[led] %s breathe rgb=%d,%d,%d t=%ld,%ld,%ld,%ldms cur=%ld,%ld,%ld rep=%ld sync=%ld",
         sec, r, g, b, rise, hold, fall, offt, cur0, cur1, cur2, repeat, sync);
    if (!c) return;
    /* LCFG0.SYNC must be in place before the pattern arms (the chip
     * latches the master-channel mode at pattern start). */
    aw_sync_mode(c, (int)sync);
    aw_breathe(c, r, g, b, rise, hold, fall, offt, 0, (int)repeat, 1,
               (int)cur0, (int)cur1, (int)cur2);
}

/* traveling-wave breathing: per-channel phase offset [sec.pattern]
 * t0=r,g,b (ms) delays each channel start -> color glides the channel
 * sequence. The shared pattern section supplies the timing. */
static void led_wave_rgb(const char *sec, int r, int g, int b)
{
    aw_chip *c = led_hw();
    char ss[128];
    long rise, hold, fall, offt, repeat, cur0, cur1, cur2, sync;
    if (!led_pattern_sec(sec, ss, sizeof(ss))) return;
    if (!req_int(ss, "rise",   0, AW_MAX_DELAY_MS, &rise))   return;
    if (!req_int(ss, "hold",   0, AW_MAX_DELAY_MS, &hold))   return;
    if (!req_int(ss, "fall",   0, AW_MAX_DELAY_MS, &fall))   return;
    if (!req_int(ss, "offt",   0, AW_MAX_DELAY_MS, &offt))   return;
    if (!req_int(ss, "repeat", 0, AW_TIME_CODES - 1, &repeat)) return;
    if (!req_int(ss, "cur_r",  0, 15, &cur0)) return;
    if (!req_int(ss, "cur_g",  0, 15, &cur1)) return;
    if (!req_int(ss, "cur_b",  0, 15, &cur2)) return;
    if (!req_int(ss, "sync",   0, 1,  &sync))  return;
    int zi[3];
    if (!req_triple(ss, "t0", zi, 0, AW_MAX_DELAY_MS)) return;
    long z[3] = { zi[0], zi[1], zi[2] };
    long rt[3] = { rise, rise, rise };
    long ht[3] = { hold, hold, hold };
    long ft[3] = { fall, fall, fall };
    long ot[3] = { offt, offt, offt };
    /* allow a plain cur=r,g,b triple too (GUI writes none for wave, but
     * hand-written configs may use the same key as solid) */
    int curc[3] = { (int)cur0, (int)cur1, (int)cur2 };
    if (read_triple(ss, "cur", curc, 0, 15)) {
        cur0 = curc[0]; cur1 = curc[1]; cur2 = curc[2];
    }
    LOGI("[led] %s wave rgb=%d,%d,%d t=%ld/%ld/%ld/%ld t0=%d,%d,%d rep=%ld cur=%ld,%ld,%ld sync=%ld",
         sec, r, g, b, rt[0], ht[0], ft[0], ot[0], z[0], z[1], z[2], repeat,
         cur0, cur1, cur2, sync);
    if (!c) return;
    int cur[3] = { (int)cur0, (int)cur1, (int)cur2 };
    aw_sync_mode(c, (int)sync);
    aw_breathe_ex(c, r, g, b, rt, ht, ft, ot, z, (int)repeat, 1, cur);
}

/* the chip's neutral state, without the public log line: every paint ends
 * the previous run with it, so no stale pattern survives a handover */
static void kill_all(aw_chip *c)
{
    if (!c) return;
    aw_sync_mode(c, 0);
    aw_all_off(c);
}

/* ---------------- applied fingerprint ----------------
 * The pool calls led_event() again for every repaint of the same entry, and
 * the bridge reposts notifications constantly. Programming the chip on
 * each of those restarts the running pattern from phase 0, which reads as
 * a strobe on a notification that is already showing. So the last applied
 * state is remembered here and the chip is touched only when the visible
 * state really changes. led_invalidate() drops the memory - the pool calls
 * it whenever the LEDs change owner or led.conf was reloaded, which are
 * exactly the cases where the chip must be reprogrammed no matter what the
 * fingerprint says. */

static struct {
    char sec[128];      /* "" for the off state                */
    char mode[16];
    int  r, g, b;
    int  valid;
} g_applied;

static void fp_set(const char *sec, const char *mode, int r, int g, int b)
{
    snprintf(g_applied.sec, sizeof(g_applied.sec), "%s", sec ? sec : "");
    snprintf(g_applied.mode, sizeof(g_applied.mode), "%s", mode);
    g_applied.r = r;
    g_applied.g = g;
    g_applied.b = b;
    g_applied.valid = 1;
}

void led_invalidate(void)
{
    g_applied.valid = 0;
}

/* The one place that puts the LEDs into the neutral state, and it leaves
 * the fingerprint saying exactly that: the pool's idle owner and the
 * charge "none" band both come through here, so the next real paint must
 * not be skipped as "already applied". */
void leds_all_off(void)
{
    LOGI("[led] all off");
    kill_all(led_hw());
    fp_set("", "off", 0, 0, 0);
}

/* per-event dispatch used by every kind in the pool. Resolves the active
 * mode for [sec], paints, and returns the mode string for the status
 * file's engine field (NULL when the event was skipped, which the pool
 * tolerates). Timing comes from the chip sections themselves; the event
 * only supplies sec + colors. sec == NULL paints "off". */
const char *led_event(const char *sec, int r, int g, int b)
{
    const char *mode = sec ? led_active_mode(sec) : "off";
    if (!mode) return NULL;      /* no mode= -> warned, event skipped */
    if (strcmp(mode, "off") && strcmp(mode, "solid") && strcmp(mode, "wave") &&
        strcmp(mode, "breath")) {
        /* An unknown mode word is a broken preset. */
        LOGI("warn: [led] %s: unknown mode \"%s\", event skipped", sec, mode);
        return NULL;
    }

    /* already on the chip: the run keeps its phase, the chip is untouched */
    if (g_applied.valid && !strcmp(g_applied.mode, mode) &&
        g_applied.r == r && g_applied.g == g && g_applied.b == b &&
        !strcmp(g_applied.sec, sec ? sec : "")) {
        return mode;
    }

    kill_all(led_hw());          /* nothing of the previous run survives */
    if (!strcmp(mode, "solid"))
        led_solid_rgb(sec, r, g, b);
    else if (!strcmp(mode, "wave"))
        led_wave_rgb(sec, r, g, b);
    else if (!strcmp(mode, "breath"))
        led_breathe_rgb(sec, r, g, b);

    fp_set(sec, mode, r, g, b);
    return mode;
}

/* Init: soft reset + power rails on, at the global [led] imax current
 * limit. imax is REQUIRED: the chip current ceiling
 * is a hardware limit, so picking one the file did not name can either
 * brown out the LED set or drive it past what the user wired. A missing
 * or illegal value therefore leaves the rails OFF and logs - the daemon
 * runs, reports the bad key, and no LED lights until led.conf names a
 * legal limit. */
void led_init_hw(void)
{
    aw_chip *c = led_hw();
    if (!c) return;
    const char *s = conf_get_str("led", "imax");
    int imax, ma;
    if      (s && !strcmp(s, "5"))  { imax = AW_IMAX_5MA;  ma = 5;  }
    else if (s && !strcmp(s, "10")) { imax = AW_IMAX_10MA; ma = 10; }
    else if (s && !strcmp(s, "15")) { imax = AW_IMAX_15MA; ma = 15; }
    else if (s && !strcmp(s, "30")) { imax = AW_IMAX_30MA; ma = 30; }
    else {
        LOGI("warn: [led] imax=%s is not 5|10|15|30 - chip left "
             "unpowered until led.conf names a legal limit",
             s ? s : "(unset)");
        aw_rst(c);
        return;
    }
    aw_rst(c);
    aw_pwr(c, imax);
    aw_lctr(c, -1, 0);
    /* Log milliamps, not the GCR2 register code (0x01 is 30mA, not 1mA). */
    LOGI("[led] aw2033 init imax=%dmA", ma);
}