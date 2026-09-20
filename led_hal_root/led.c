/*
 * led.c - thin adapter that turns events + led.conf into AW2033 calls.
 *
 * v3 = per-event renderer. There is no global "mode" anymore: every
 * event owns its [sec] section and led_event() reads the active mode
 * from that event's own section:
 *
 *   [sec]         mode=off|solid|breath|wave
 *   [sec.solid]   cur=r,g,b         0..15 per channel current
 *   [sec.breath]  repeat=0..15, cur_r/cur_g/cur_b,
 *                 rise/hold/fall/offt (ms, owned here),
 *                 sync=0|1: LCFG0.SYNC master-channel lock (see below)
 *   [sec.wave]    t0=r,g,b phase offset (ms), repeat=0..15,
 *                 rise/hold/fall/offt (ms, owned here),
 *                 sync=0|1: LCFG0.SYNC master-channel lock (see below)
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
 * Timing (rise/hold/fall/offt) belongs to the chip section that
 * animates: [sec.breath] and [sec.wave] each carry their own keys.
 * No base-section timing, no fallback. The [led] section keeps only
 * chip/daemon globals: logging, trace_sysfs, imax.
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
#include <string.h>
#include <unistd.h>
#include "chgd.h"
#include "../lib/aw2033.h"

/* builtin timing for a chip section that omits its keys (the GUI always
 * writes them; this only covers hand-written minimal configs) */
#define DEF_T_RISE 500
#define DEF_T_HOLD 100
#define DEF_T_FALL 500
#define DEF_T_OFFT 1200

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

/* clamp helpers for the config knobs the sections let through */
static int clampi(long v, int lo, int hi)
{
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)v;
}

/* active [sec] renderer mode: explicit led.conf key, "breath" when the
 * section has none (the GUI always writes mode=, so this is only for
 * hand-written minimal configs). */
static const char *led_active_mode(const char *sec)
{
    const char *mode = conf_get_str(sec, "mode");
    if (!mode || !mode[0]) return "breath";
    return mode;
}

/* read an "a,b,c" triplet from [sec] key into out[], clamped, with
 * builtin fallback (lo..hi). Returns the number of items parsed. */
static int read_triple(const char *sec, const char *key,
                       int *out, int d0, int d1, int d2, int lo, int hi)
{
    out[0] = d0; out[1] = d1; out[2] = d2;
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
    char ss[40];
    int cur[3];
    snprintf(ss, sizeof(ss), "%s.solid", sec);
    read_triple(ss, "cur", cur, 15, 15, 15, 0, 15);
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
 * lives in the [sec.breath] chip section itself (rise/hold/fall/offt);
 * repeat/cur are knob passthrough from the same section. [sec.breath]
 * sync=1 turns on the chip's built-in master sync (LCFG0.SYNC): all
 * channels dim on PWM channel 0 (master red), per-channel cur still
 * applies. That pins the phases that otherwise drift apart because the
 * rise/fall period grows with the PWM amplitude. */
static void led_breathe_rgb(const char *sec, int r, int g, int b)
{
    aw_chip *c = led_hw();
    char ss[40];
    snprintf(ss, sizeof(ss), "%s.breath", sec);
    long rise = conf_get_int(ss, "rise", DEF_T_RISE);
    long hold = conf_get_int(ss, "hold", DEF_T_HOLD);
    long fall = conf_get_int(ss, "fall", DEF_T_FALL);
    long offt = conf_get_int(ss, "offt", DEF_T_OFFT);
    int  repeat  = clampi(conf_get_int(ss, "repeat", 0), 0, 15);
    int  cur0    = clampi(conf_get_int(ss, "cur_r", 15), 0, 15);
    int  cur1    = clampi(conf_get_int(ss, "cur_g", 15), 0, 15);
    int  cur2    = clampi(conf_get_int(ss, "cur_b", 15), 0, 15);
    int  sync    = clampi(conf_get_int(ss, "sync", 0), 0, 1);
    LOGI("[led] %s breathe rgb=%d,%d,%d t=%ld,%ld,%ld,%ldms cur=%d,%d,%d rep=%d sync=%d",
         sec, r, g, b, rise, hold, fall, offt, cur0, cur1, cur2, repeat, sync);
    if (!c) return;
    /* LCFG0.SYNC must be in place before the pattern arms (the chip
     * latches the master-channel mode at pattern start). */
    aw_sync_mode(c, sync);
    aw_breathe(c, r, g, b, rise, hold, fall, offt, 0, repeat, 1,
               cur0, cur1, cur2);
}

/* traveling-wave breathing: per-channel phase offset [sec.wave]
 * t0=r,g,b (ms) delays each channel start -> color glides the channel
 * sequence. The timing lives in the [sec.wave] chip section. */
static void led_wave_rgb(const char *sec, int r, int g, int b)
{
    aw_chip *c = led_hw();
    char ss[40];
    snprintf(ss, sizeof(ss), "%s.wave", sec);
    long rise = conf_get_int(ss, "rise", DEF_T_RISE);
    long hold = conf_get_int(ss, "hold", DEF_T_HOLD);
    long fall = conf_get_int(ss, "fall", DEF_T_FALL);
    long offt = conf_get_int(ss, "offt", DEF_T_OFFT);
    long rt[3] = { rise, rise, rise };
    long ht[3] = { hold, hold, hold };
    long ft[3] = { fall, fall, fall };
    long ot[3] = { offt, offt, offt };
    long z[3] = {0, 1300, 2600};
    const char *st0 = conf_get_str(ss, "t0");
    if (st0) {
        long a, b, c;
        if (sscanf(st0, "%ld,%ld,%ld", &a, &b, &c) == 3) {
            if (a < 0) a = 0; if (b < 0) b = 0; if (c < 0) c = 0;
            z[0] = a; z[1] = b; z[2] = c;
        }
    }
    int repeat = clampi(conf_get_int(ss, "repeat", 0), 0, 15);
    int cur0 = clampi(conf_get_int(ss, "cur_r", 15), 0, 15);
    int cur1 = clampi(conf_get_int(ss, "cur_g", 15), 0, 15);
    int cur2 = clampi(conf_get_int(ss, "cur_b", 15), 0, 15);
    int sync  = clampi(conf_get_int(ss, "sync", 0), 0, 1);
    /* allow a plain cur=r,g,b triple too (GUI writes none for wave, but
     * hand-written configs may use the same key as solid) */
    int curc[3] = { cur0, cur1, cur2 };
    if (read_triple(ss, "cur", curc, cur0, cur1, cur2, 0, 15))
        { cur0 = curc[0]; cur1 = curc[1]; cur2 = curc[2]; }
    LOGI("[led] %s wave rgb=%d,%d,%d t=%ld/%ld/%ld/%ld t0=%ld,%ld,%ld rep=%d cur=%d,%d,%d sync=%d",
         sec, r, g, b, rt[0], ht[0], ft[0], ot[0], z[0], z[1], z[2], repeat,
         cur0, cur1, cur2, sync);
    if (!c) return;
    int cur[3] = { cur0, cur1, cur2 };
    aw_sync_mode(c, sync);
    aw_breathe_ex(c, r, g, b, rt, ht, ft, ot, z, repeat, 1, cur);
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
 * string for the status file's engine field. Timing comes from the
 * chip sections themselves; the event only supplies sec + colors. */
const char *led_event(const char *sec, int r, int g, int b)
{
    const char *mode = led_active_mode(sec);

    LOGI("[led] %s mode=%s rgb=%d,%d,%d", sec, mode, r, g, b);

    if (!strcmp(mode, "off"))
        leds_all_off();
    else if (!strcmp(mode, "solid"))
        led_solid_rgb(sec, r, g, b);
    else if (!strcmp(mode, "wave"))
        led_wave_rgb(sec, r, g, b);
    else
        led_breathe_rgb(sec, r, g, b);
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