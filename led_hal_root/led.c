/*
 * led.c - thin adapter that turns events + led.conf into AW2033 calls.
 *
 * v3 = per-event renderer. There is no global "mode" anymore: every
 * event owns its [sec] section and led_event() reads the active mode
 * from that event's own section:
 *
 *   [sec]         mode=off|solid|breath|wave
 *   [sec.solid]   cur=r,g,b         0..15 per channel current
 *   [sec.pattern] repeat=0..15, cur_r/cur_g/cur_b,
 *                 rise/hold/fall/offt (ms), sync=0|1,
 *                 t0=r,g,b phase offsets (wave only)
 *
 * Legacy [sec.breath]/[sec.wave] sections remain readable as fallbacks.
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
 * the config value, cleared on solid/off), so a stale master bit can
 * never leak into a config that turned sync off.
 *
 * Timing (rise/hold/fall/offt) belongs to [sec.pattern], shared by breath
 * and wave. The [led] section keeps only
 * chip/daemon globals: logging, imax.
 *
 * No timer threads, no sysfs poking, no software animation - the
 * breathing, traveling-wave and solid modes all run inside the chip
 * itself via the aw2033 controller (lib\libaw2033.a from the standalone
 * aw2033-driver repo, vendored header lib\aw2033.h). Every
 * other module (core, charge, notify, ring) lights the LEDs ONLY
 * through the exported led_event() / leds_all_off() calls in chgd.h.
 *
 * Events only supply the color; everything else (timing, current, phase
 * offsets, repeat) comes from the [sec.*] chip sections. config.c treats
 * any unknown [section] as mod-owned, so all of these read raw.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "chgd.h"
#include "../lib/aw2033.h"

/* clamp helper for the config knobs the sections let through */
static int clampi(long v, int lo, int hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)v;
}

/* ---------------- strict preset readers ----------------
 *
 * No builtin timing/current/phase substitutes here. A chip section that
 * omits a key is a broken preset: painting it with a default produces
 * exactly the "LED blinks wrong and nothing is logged" class of bug.
 * Every reader below reports the offending key and fails, and the
 * caller skips the event instead of guessing. */

/* required integer key: absent or non-numeric -> warn + fail */
static int req_int(const char *sec, const char *key, long *out)
{
    const char *v = conf_get_str(sec, key);
    if (!v || !v[0]) {
        LOGW("[led] %s: missing key %s", sec, key);
        return 0;
    }
    char *end = NULL;
    long x = strtol(v, &end, 10);
    if (end == v || *end) {
        LOGW("[led] %s: key %s=\"%s\" is not a number", sec, key, v);
        return 0;
    }
    *out = x;
    return 1;
}

/* required "a,b,c" key, clamped to lo..hi; absent or malformed -> warn
 * + fail. Reports every missing key in one line instead of one per key. */
static int req_triple(const char *sec, const char *key,
                      int *out, int lo, int hi)
{
    const char *v = conf_get_str(sec, key);
    if (!v || !v[0]) {
        LOGW("[led] %s: missing key %s", sec, key);
        return 0;
    }
    int a, b, c;
    if (sscanf(v, "%d,%d,%d", &a, &b, &c) != 3) {
        LOGW("[led] %s: key %s=\"%s\" is not \"a,b,c\"", sec, key, v);
        return 0;
    }
    out[0] = clampi(a, lo, hi);
    out[1] = clampi(b, lo, hi);
    out[2] = clampi(c, lo, hi);
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
 * NULL - the caller skips the event instead of painting a breath nobody
 * asked for. */
static const char *led_active_mode(const char *sec)
{
    const char *mode = conf_get_str(sec, "mode");
    if (!mode || !mode[0]) {
        LOGW("[led] %s: missing key mode", sec);
        return NULL;
    }
    return mode;
}

/* the chip preset section to render from: [sec.pattern], or the legacy
 * [sec.breath]/[sec.wave] while old configs are still around. */
static int led_pattern_sec(const char *sec, const char *legacy, char *out, size_t n)
{
    snprintf(out, n, "%s.pattern", sec);
    if (conf_sec_exists(out)) return 1;
    snprintf(out, n, "%s.%s", sec, legacy);
    if (conf_sec_exists(out)) return 1;
    LOGW("[led] %s: no [%s.pattern] and no legacy [%s.%s], event skipped",
         sec, sec, sec, legacy);
    return 0;
}

/* optional "a,b,c" reader: 0 when the key is absent or malformed, and
 * the caller keeps its own value. Only for genuinely optional extras
 * (the wave cur= override a hand-written config may add) - never as a
 * substitute for a key the preset requires. */
static int read_triple(const char *sec, const char *key,
                       int *out, int lo, int hi)
{
    const char *s = conf_get_str(sec, key);
    if (!s) return 0;
    int a, b, c;
    if (sscanf(s, "%d,%d,%d", &a, &b, &c) != 3) return 0;
    out[0] = clampi(a, lo, hi);
    out[1] = clampi(b, lo, hi);
    out[2] = clampi(c, lo, hi);
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
    if (!led_pattern_sec(sec, "breath", ss, sizeof(ss))) return;
    if (!req_int(ss, "rise",  &rise))  return;
    if (!req_int(ss, "hold",  &hold))  return;
    if (!req_int(ss, "fall",  &fall))  return;
    if (!req_int(ss, "offt",  &offt))  return;
    if (!req_int(ss, "repeat", &repeat)) return;
    if (!req_int(ss, "cur_r", &cur0)) return;
    if (!req_int(ss, "cur_g", &cur1)) return;
    if (!req_int(ss, "cur_b", &cur2)) return;
    if (!req_int(ss, "sync",  &sync))  return;
    cur0 = clampi(cur0, 0, 15);
    cur1 = clampi(cur1, 0, 15);
    cur2 = clampi(cur2, 0, 15);
    repeat = clampi(repeat, 0, 15);
    sync = clampi(sync, 0, 1);
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
    if (!led_pattern_sec(sec, "wave", ss, sizeof(ss))) return;
    if (!req_int(ss, "rise",  &rise))  return;
    if (!req_int(ss, "hold",  &hold))  return;
    if (!req_int(ss, "fall",  &fall))  return;
    if (!req_int(ss, "offt",  &offt))  return;
    if (!req_int(ss, "repeat", &repeat)) return;
    if (!req_int(ss, "cur_r", &cur0)) return;
    if (!req_int(ss, "cur_g", &cur1)) return;
    if (!req_int(ss, "cur_b", &cur2)) return;
    if (!req_int(ss, "sync",  &sync))  return;
    int zi[3];
    if (!req_triple(ss, "t0", zi, 0, 65535)) return;
    long z[3] = { zi[0], zi[1], zi[2] };
    long rt[3] = { rise, rise, rise };
    long ht[3] = { hold, hold, hold };
    long ft[3] = { fall, fall, fall };
    long ot[3] = { offt, offt, offt };
    cur0 = clampi(cur0, 0, 15);
    cur1 = clampi(cur1, 0, 15);
    cur2 = clampi(cur2, 0, 15);
    repeat = clampi(repeat, 0, 15);
    sync = clampi(sync, 0, 1);
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

void leds_all_off(void)
{
    aw_chip *c = led_hw();
    LOGI("[led] all off");
    if (c) {
        aw_sync_mode(c, 0);
        aw_all_off(c);
    }
}

/* per-event dispatch used by charge/notify/missed/alarm/ring/voip.
 * Resolves the active mode for [sec], paints, and returns the mode
 * string for the status file's engine field (NULL when the event was
 * skipped, which every caller already tolerates). Timing comes from the
 * chip sections themselves; the event only supplies sec + colors. */
const char *led_event(const char *sec, int r, int g, int b)
{
    const char *mode = led_active_mode(sec);
    if (!mode) return NULL;      /* no mode= -> warned, event skipped */

    if (!strcmp(mode, "off"))
        leds_all_off();
    else if (!strcmp(mode, "solid"))
        led_solid_rgb(sec, r, g, b);
    else if (!strcmp(mode, "wave"))
        led_wave_rgb(sec, r, g, b);
    else if (!strcmp(mode, "breath"))
        led_breathe_rgb(sec, r, g, b);
    else {
        /* an unknown mode word used to fall through to breath */
        LOGW("[led] %s: unknown mode \"%s\", event skipped", sec, mode);
        return NULL;
    }
    return mode;
}

/* mode resolution without painting: charge.c calls it to fold the active
 * mode into the applied-fingerprint, so editing only the renderer mode
 * in led.conf still repaints on the next refresh. */
const char *led_resolve_mode(const char *sec)
{
    return led_active_mode(sec);
}

/* init: soft reset + power rails on (Imax from global [led] imax) */
void led_init_hw(void)
{
    aw_chip *c = led_hw();
    if (!c) return;
    const char *s = conf_get_str("led", "imax");
    int imax = AW_IMAX_30MA;
    if (s && !strcmp(s, "5"))  imax = AW_IMAX_5MA;
    if (s && !strcmp(s, "10")) imax = AW_IMAX_10MA;
    if (s && !strcmp(s, "15")) imax = AW_IMAX_15MA;
    aw_rst(c);
    aw_pwr(c, imax);
    aw_lctr(c, -1, 0);
    LOGI("[led] aw2033 init imax=%s", s ? s : "30");
}