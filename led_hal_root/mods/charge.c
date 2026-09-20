/*
 * mods/charge.c - charge band evaluation and LED application.
 *
 * Evaluates and applies the charge band PURELY on events - there is no
 * idle heartbeat: at true idle the core disarms the timer and the band
 * repaints only when a power_supply uevent arrives or led.conf is edited
 * (SIGALRM refresh). Registered two ways:
*     REGISTER_REFRESH   boot + SIGALRM (led.conf edited): re-evaluate
 *     REGISTER_UEVENT    "power_supply" uevents: same re-evaluation. NOTE:
 *                        on this kernel the POWER_SUPPLY uevent fires only
 *                        on plug/unplug - a mid-charge capacity trickle
 *                        past a threshold (lower -> middle -> upper)
 *                        broadcasts nothing.
 *     REGISTER_MODE      "charge" owns the idle channel ("") while the
 *                        charger is live: a 60s recheck re-evaluates the
 *                        band and repaints only on a real change
 *                        (fingerprint-gated, no LED blip). The kernel gap
 *                        is the recheck's only job and the cadence dies
 *                        the moment the status leaves charging.
 *                        queue_has_pending() / queue_active() defer "" to
 *                        the notification pool, so a parked or showing
 *                        notification never competes with the recheck.
 *     SIGQUIT charge test: holds the channel via cur_pkg, no mode/timer
 * The band is recomputed on battery uevents and written to the state
 * file; the LEDs are rewritten only on real state changes
 * (g_applied_band fingerprint: band + colors + mode).

 * Config ownership: [charge] carries ONLY the thresholds. Every band
 * owns its full renderer: [charge.lower|middle|upper] mode= + color=
 * and the [charge.<band>.solid/breath/wave] chip sections (each chip
 * owns its own timing keys). No base-section timing, no fallback.
 *
 * Nothing here needs the core to know about it; the core only walks
 * the registries. Owners are mutually exclusive: "" -> notify while
 * the pool has work (charges repaint on uevent/SIGALRM), armed package
 * -> notify, "incoming.call" -> ring.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include "../chgd.h"

#define STATE_PATH "/data/local/tmp/led_chg"
#define STATE_TMP  "/data/local/tmp/led_chg.tmp"

/* charge thresholds defaults live in config.c ([charge] section of
 * led.conf). Builtin fallbacks are defined there too. Band colors and
 * renderers live in each band's own section (see apply_band). */

/* builtin band colours (keep in sync with the GUI defaults): lower =
 * red, middle = lime, upper = green. The band section's color= key
 * overrides when present. */
#define DEF_LOWER_R 255
#define DEF_LOWER_G 0
#define DEF_LOWER_B 0
#define DEF_MIDDLE_R 96
#define DEF_MIDDLE_G 255
#define DEF_MIDDLE_B 0
#define DEF_UPPER_R 0
#define DEF_UPPER_G 255
#define DEF_UPPER_B 0

/* last applied charge state: band + color + mode.
 * Rewritten only on a real change, otherwise every tick/uevent would
 * blink the LED off->on. Invalidation: arm_notification / arm_ring set
 * the first byte to NUL, forcing a reapply on the next charge pass. */
char g_applied_band[64] = "\x01INIT";

/* Charge recheck cadence. This kernel's POWER_SUPPLY uevent fires only
 * on plug/unplug; a capacity crossing mid-charge is silent. While the
 * charger is live and nobody else owns the idle channel, one 60s tick
 * re-evaluates the band (two sysfs reads; the g_applied_band fingerprint
 * keeps the repaint silent until the band actually changes). The mode
 * owns "" only while charging/full AND the notification pool is empty. */
#define CHARGE_RECHECK_MS 60000L

static int g_charge_live;    /* 1 = status Charging/Full at last eval */

static const char *band_for(const char *status, const char *cap)
{
    int c = -1;
    if (cap && *cap) {
        c = atoi(cap);
        for (const char *p = cap; *p; p++)
            if (*p < '0' || *p > '9') { c = -1; break; }
    }

    /* range names are abstract (lower/middle/upper); the actual colors
     * and light types per range come from led.conf, not hardcoded */
    if (!strcmp(status, "Full"))
        return "upper";

    if (!strcmp(status, "Charging")) {
        int second = conf_second_threshold();
        int first = conf_first_threshold();
        if (c >= second) return "upper";
        if (c >= first) return "middle";
        return "lower";
    }

    if (!strcmp(status, "Not charging"))
        return (c >= conf_second_threshold()) ? "upper" : "none";

    return "none";
}

long eval_and_write(void)
{
    char status[64] = "", cap[16] = "";
    read_line("/sys/class/power_supply/battery/status", status, sizeof(status));
    read_line("/sys/class/power_supply/battery/capacity", cap, sizeof(cap));

    /* mark whether the charge recheck cadence should stay armed: plugged
     * and charging/full only. Discharging / Not charging -> cadence off. */
    g_charge_live = (!strcmp(status, "Charging") || !strcmp(status, "Full")) ? 1 : 0;

    const char *band = band_for(status, cap);
    long ts = (long)time(NULL);

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%s %ld\n", band, ts);

    int fd = open(STATE_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        ssize_t ig = write(fd, buf, (size_t)len); (void)ig;
        close(fd);
        rename(STATE_TMP, STATE_PATH);
    }
    return ts;
}

static void band_rgb(const char *band, int *r, int *g, int *b)
{
    int d0, d1, d2;
    if (!strcmp(band, "lower")) { d0 = DEF_LOWER_R; d1 = DEF_LOWER_G; d2 = DEF_LOWER_B; }
    else if (!strcmp(band, "middle")) { d0 = DEF_MIDDLE_R; d1 = DEF_MIDDLE_G; d2 = DEF_MIDDLE_B; }
    else { d0 = DEF_UPPER_R; d1 = DEF_UPPER_G; d2 = DEF_UPPER_B; }
    *r = d0; *g = d1; *b = d2;
    char sec[24];
    snprintf(sec, sizeof(sec), "charge.%s", band);
    const char *c = conf_get_str(sec, "color");
    if (c && c[0] && sscanf(c, "%d,%d,%d", r, g, b) == 3) {
        if (*r < 0) *r = 0; if (*r > 255) *r = 255;
        if (*g < 0) *g = 0; if (*g > 255) *g = 255;
        if (*b < 0) *b = 0; if (*b > 255) *b = 255;
    }
}

/* apply one named band to the LEDs (the shared core of both the
 * state-file path and the charge test).
 *
 * Every band owns its OWN section set: [charge.lower|middle|upper]
 * mode= + color= and the [charge.<band>.solid/breath/wave] chip
 * sections (each chip owns its timing keys). [charge] base keeps only
 * the thresholds. */
static void apply_band(const char *band)
{
    int r = 0, g = 0, b = 0;
    char sec[32];
    if (!strcmp(band, "lower")) {
        snprintf(sec, sizeof(sec), "charge.lower");
    } else if (!strcmp(band, "middle")) {
        snprintf(sec, sizeof(sec), "charge.middle");
    } else if (!strcmp(band, "upper")) {
        snprintf(sec, sizeof(sec), "charge.upper");
    } else {
        snprintf(sec, sizeof(sec), "charge");   /* "none" */
    }

    /* The "none"/idle band is ALWAYS off by design - painted directly,
     * never affected by whatever renderer the sections configure. */

    if (!strcmp(band, "none")) {
        char fp0[16];
        snprintf(fp0, sizeof(fp0), "none off");
        if (!strcmp(g_applied_band, fp0))
            return;
        leds_all_off();
        snprintf(g_applied_band, sizeof(g_applied_band), "%s", fp0);
        status_write("charge", band, "", 0, 0, 0, "off");
        LOGI("charge leds -> none (off)");
        return;
    }

    band_rgb(band, &r, &g, &b);

    /* fingerprint includes the resolved mode so a mode-only config edit
     * (SIGALRM refresh, same band/color) still repaints. */
    const char *mode = led_resolve_mode(sec);
    char fp[64];
    snprintf(fp, sizeof(fp), "%s %d,%d,%d %s",
             band, r, g, b, mode ? mode : "-");
    if (!strcmp(g_applied_band, fp))
        return;

    const char *engine = led_event(sec, r, g, b);
    snprintf(g_applied_band, sizeof(g_applied_band), "%s", fp);
    status_write("charge", band, "", r, g, b, engine);
    LOGI("charge leds -> %s (rgb=%d,%d,%d mode=%s)",
         band, r, g, b, engine);
}

void apply_charge_leds(void)
{
    char line[64] = "";
    read_line(STATE_PATH, line, sizeof(line));
    char *sp = strchr(line, ' ');
    if (sp) *sp = '\0';
    apply_band(line);
}

/* ---------------- registry hooks ---------------- */

/* the one refresh: write the band, repaint the LEDs unless another
 * owner (notify/ring) is currently showing something, and re-tune the
 * timer so the charging recheck cadence arms on plug / dies on unplug. */
static void charge_refresh(void)
{
    (void)eval_and_write();
    if (!g_st.cur_pkg[0])
        apply_charge_leds();
    retune_timer();
}

/* ---------------- charge zone test (SIGQUIT) ---------------- */

/* Each press advances the fake charge zone lower -> middle -> upper ->
 * and round. The [charge.<band>] config fully decides color + renderer
 * via led_event(), nothing is hardcoded. The fake zone is NOT timed: no
 * mode, no cadence - it holds until the next press or Disarm. */
#define CHARGE_TEST_PKG "charge.test"

static int g_charge_test_seq;

static const char *charge_test_band(int seq)
{
    switch (seq % 3) {
    case 0: return "lower";
    case 1: return "middle";
    default: return "upper";
    }
}

static int charge_test_owns(const char *pkg)
{
    return pkg && !strcmp(pkg, CHARGE_TEST_PKG);
}

void charge_test_next(void)
{
    if (!charge_test_owns(g_st.cur_pkg)) {
        /* fresh session: drop whatever owns the channel, start at lower */
        if (g_st.cur_pkg[0])
            disarm_notification(&g_st, "test switch");
        g_charge_test_seq = 0;
    }
    g_applied_band[0] = '\0';       /* force a real repaint */
    snprintf(g_st.cur_pkg, sizeof(g_st.cur_pkg), "%s", CHARGE_TEST_PKG);
    g_st.armed_at = time(NULL);
    g_st.test = 1;
    const char *band = charge_test_band(g_charge_test_seq);
    apply_band(band);
    LOGI("charge test -> %s", band);
    g_charge_test_seq++;
}

REGISTER_REFRESH(charge_refresh);
REGISTER_UEVENT("power_supply", charge_refresh);

/* ---------------- charge recheck cadence ----------------
 * mode_owns() consults this with cur_pkg == "" whenever the channel is
 * free. Ownership of "" is shared with the notification pool: notify
 * claims it while queue entries wait (its own adaptive cadence), charge
 * claims it only while the charger is live AND the pool is empty. The
 * two conditions are mutually exclusive, so the linker section order of
 * the two modes never matters.
 *
 * The tick re-runs the full band evaluation + fingerprint-gated repaint,
 * so a silenty crossed threshold lights up within one recheck period.
 * The timer goes away the moment the pool regains work or the status
 * leaves Charging/Full. */
static int charge_owns(const char *pkg)
{
    if (pkg && pkg[0]) return 0;
    if (queue_active() || queue_has_pending())   /* notify's idle claim */
        return 0;
    return g_charge_live;
}

static void charge_tick(void)
{
    charge_refresh();
}

REGISTER_MODE("charge", CHARGE_RECHECK_MS, charge_owns, charge_tick);