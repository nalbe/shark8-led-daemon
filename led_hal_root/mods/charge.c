/*
 * mods/charge.c - charge band evaluation and LED application.
 *
 * Purely event-driven, exactly like the notification pipeline: the charge
 * state arrives as a CHG command on the NLS socket (the bridge watches
 * ACTION_BATTERY_CHANGED / ACTION_POWER_CONNECTED / ACTION_POWER_DISCONNECTED
 * and pokes us on every real level/status change). There is NO sysfs read
 * anywhere in the daemon - /sys/class/power_supply is never touched - and
 * no polling cadence: the android battery broadcast fires on every capacity
 * step, so the old 60s threshold-recheck mode is gone along with the
 * power_supply uevent hook.
 *
 *   CHG <status> <level> [<plugged>]
 *                          the bridge's parse of the battery broadcast:
 *                          status is the raw BatteryManager constant
 *                          (2=Charging, 3=Discharging, 4=Not charging,
 *                          5=Full) or a ready-made word ("Not charging"
 *                          arrives as two tokens); level is 0..100; the
 *                          optional trailing plugged bit (BatteryManager
 *                          EXTRA_PLUGGED) gates the band: plugged=0 means
 *                          the broadcast itself says no source is attached,
 *                          so a stale "Charging" status can never light the
 *                          charge LED (this device's battery service has
 *                          been seen to stick at Charging while unplugged).
 *   REGISTER_REFRESH       boot + SIGALRM (led.conf edited): re-render the
 *                          band from the last bridge state so a threshold
 *                          or color edit repaints; no data yet -> off.
 *   SIGQUIT charge test    holds the channel via cur_pkg, no mode/timer
 * The band is recomputed on every CHG and written to the state file; the
 * LEDs are rewritten only on a real change (g_applied_band fingerprint:
 * band + colors + mode). The log is transition-only too: status/plug
 * changes and zone crossings are logged, the bridge's periodic sticky
 * re-emissions are not.
 *
 * Config ownership: [charge] carries ONLY the thresholds. Every band
 * owns its full renderer: [charge.lower|middle|upper] mode= + color=
 * and the [charge.<band>.solid/breath/wave] chip sections (each chip
 * owns its own timing keys). No base-section timing, no fallback.
 *
 * Nothing here needs the core to know about it; the core only walks
 * the registries. Owners are mutually exclusive: "" -> notify while
 * the pool has work (charges repaint on CHG/SIGALRM), armed package
 * -> notify, "incoming.call" -> ring.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../chgd.h"

#define STATE_PATH "/data/local/tmp/led_chg"

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
 * Rewritten only on a real change, otherwise every CHG would blink the
 * LED off->on. Invalidation: arm_notification / arm_ring set the first
 * byte to NUL, forcing a reapply on the next charge pass. */
char g_applied_band[64] = "\x01INIT";

/* last charge state reported by the bridge (CHG command). No sysfs, no
 * kernel reads: this is the ONLY charge input the daemon has. */
static char g_chg_status[24] = "";   /* status word, "" = no data yet */
static int  g_chg_level      = -1;   /* capacity percent, -1 = none   */
static int  g_chg_plugged    = -1;   /* plugged bit, -1 = not reported */

/* last state that actually reached the log. The bridge re-emits its
 * sticky battery snapshot periodically (every 30s or so); a repeat of
 * the same status/level/plugged/band must stay silent. Only charging
 * started/stopped, zone crossings and plug toggles are noteworthy. */
static char g_log_status[24]  = "";
static int  g_log_plugged     = -1;
static char g_log_band[16]    = "";

static const char *band_for(const char *status, int level)
{
    /* range names are abstract (lower/middle/upper); the actual colors
     * and light types per range come from led.conf, not hardcoded */
    if (!strcmp(status, "Full"))
        return "upper";

    if (!strcmp(status, "Charging")) {
        int second = conf_second_threshold();
        int first = conf_first_threshold();
        if (level >= second) return "upper";
        if (level >= first) return "middle";
        return "lower";
    }

    if (!strcmp(status, "Not charging"))
        return (level >= conf_second_threshold()) ? "upper" : "none";

    return "none";      /* Discharging / Unknown / no data */
}

/* persist the current band to the state file (GUI reads it), atomic
 * write via the shared helper (tmp + rename) */
static void charge_write(const char *band)
{
    long ts = (long)time(NULL);

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%s %ld\n", band, ts);

    atomic_write(STATE_PATH, buf, (size_t)len);
}

/* Normalize the bridge's BatteryManager status: a raw int constant or a
 * word (both spellings the config may emit). Unknown stays a no-band. */
static const char *status_word(const char *tok)
{
    if (tok[0] >= '0' && tok[0] <= '9') {
        switch (atoi(tok)) {
        case 2: return "Charging";
        case 3: return "Discharging";
        case 4: return "Not charging";
        case 5: return "Full";
        default: return "Unknown";
        }
    }
    return tok;
}

static void apply_band(const char *band_in);   /* defined below */

/* charge note from the NLS bridge: "CHG <status> <level> [<plugged>]".
 * Tokenized defensively: every non-numeric token builds the status word
 * ("Not charging" is two tokens), a leading 1..5 int is a raw
 * BatteryManager status, the next numeric token is the level and an
 * optional trailing numeric token is the plugged bit. */
void charge_note(const char *s)
{
    char status[32] = "";
    int level = -1;
    int plugged = -1;

    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        char tok[24];
        size_t n = 0;
        while (s[n] && s[n] != ' ' && s[n] != '\t' && s[n] != '\n' &&
               n < sizeof(tok) - 1) {
            tok[n] = s[n];
            n++;
        }
        tok[n] = '\0';
        s += n;

        if (tok[0] >= '0' && tok[0] <= '9') {
            int v = atoi(tok);
            if (!status[0] && v >= 1 && v <= 5)
                snprintf(status, sizeof(status), "%s", tok);
            else if (level < 0)
                level = (v < 0) ? -1 : (v > 100 ? 100 : v);
            else if (plugged < 0)
                plugged = v;
        } else if (status[0]) {
            size_t len = strlen(status);
            snprintf(status + len, sizeof(status) - len, " %s", tok);
        } else {
            snprintf(status, sizeof(status), "%s", tok);
        }
    }

    const char *sw = status_word(status);
    if (sw != status)
        snprintf(status, sizeof(status), "%s", sw);

    snprintf(g_chg_status, sizeof(g_chg_status), "%s", status);
    g_chg_level = level;
    g_chg_plugged = plugged;

    /* plugged=0 from the broadcast itself: no source is physically
     * attached, so a stuck/stale "Charging" status must not light the
     * charge LED (this device's battery service has done exactly that). */
    const char *band = band_for(g_chg_status, g_chg_level);
    if (plugged == 0)
        band = "none";

    /* Log only real transitions, not the bridge's periodic sticky
     * re-emissions: charging started/stopped (status or plug changed)
     * and zone crossings (band changed). */
    if (strcmp(g_chg_status, g_log_status) ||
        g_chg_plugged != g_log_plugged ||
        strcmp(band, g_log_band)) {
        LOGI("charge: %s %d%% plug=%d", status[0] ? status : "-", level, plugged);
        snprintf(g_log_status, sizeof(g_log_status), "%s", g_chg_status);
        g_log_plugged = plugged;
        snprintf(g_log_band, sizeof(g_log_band), "%s", band);
    }

    charge_write(band);
    if (!g_st.cur_pkg[0])       /* another owner holds the channel */
        apply_band(band);
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
 * the thresholds. Empty/unknown bands collapse to "none" (off). */
static void apply_band(const char *band_in)
{
    const char *band = band_in ? band_in : "none";
    if (strcmp(band, "lower") && strcmp(band, "middle") &&
        strcmp(band, "upper"))
        band = "none";

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

/* the one refresh: boot + SIGALRM (led.conf edited). Re-render the band
 * from the last bridge state so a threshold/color edit repaints; with no
 * CHG data yet the LEDs stay off. */
static void charge_refresh(void)
{
    if (g_st.cur_pkg[0])
        return;
    if (g_chg_status[0] && g_chg_plugged != 0)
        apply_band(band_for(g_chg_status, g_chg_level));
    else
        apply_band("none");
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