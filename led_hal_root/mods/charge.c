/*
 * mods/charge.c - charge band evaluation and LED application.
 *
 * Purely event-driven, exactly like the notification pipeline: the charge
 * state arrives as a CHG command on the NLS socket, which the bridge
 * refreshes on every battery broadcast.
 *
 *   CHG <status> <level> [<plugged>]
 *                          the bridge's parse of the battery broadcast:
 *                          status is a BatteryManager constant or a word,
 *                          level is 0..100, and the optional trailing
 *                          EXTRA_PLUGGED bit gates the band - plugged=0
 *                          means the broadcast itself reports no source
 *                          attached, so a status stuck at Charging keeps
 *                          the charge LED dark.
 *   REGISTER_REFRESH       boot + SIGALRM (led.conf edited): re-render the
 *                          band from the last bridge state so a threshold
 *                          or color edit repaints.
 *   charge test            a MODE of this same entry (g_charge_test), not a
 *                          kind of its own: each press advances the fake
 *                          zone lower -> middle -> upper -> lower and the
 *                          entry is pushed through the one test entry point
 *                          (core.c's pool_test), so the press always lands
 *                          even while a call rainbow holds the LEDs.
 *
 * The band is the payload of an ordinary pool entry: [priority] ranks it
 * by the same key and the same tie-breaks as every other kind, and the
 * pool owns the handover in both directions. Its "none" band is a PAINT
 * too - an unplugged device means the LEDs go dark through this very
 * entry - and EV_PERSISTENT keeps that entry alive from boot (a battery
 * state has no OFF edge to end it).
 *
 * The band is recomputed on every CHG and written to the state file; the
 * LEDs are rewritten only on a real change (the applied fingerprint:
 * sec + colors + mode, owned by led.c). The log is transition-only too:
 * status/plug changes and zone crossings are logged, the bridge's periodic
 * sticky re-emissions are not. EV_MOD_LOGGED makes this mod the only
 * narrator of a charge event: its line carries the whole CHG snapshot.
 *
 * Config ownership: [charge] carries the thresholds. Every band owns its
 * full renderer as one render= line: [charge.lower|middle|upper].
 * Every one of those keys is required.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../chgd.h"

/* Thresholds and band colours are read from led.conf and nowhere else:
 * [charge] first_threshold / second_threshold, [charge.<band>] colour.
 * See band_color() and band_for(). */

/* one battery snapshot: the bridge's parse (CHG) plus the band resolved
 * from it. status "" = no data yet, level -1 = none, plugged -1 = not
 * reported, band "none" = off. */
struct chg_state {
    char status[24];
    int  level;
    int  plugged;
    char band[16];
};

/* the live state, the one every CHG overwrites */
static struct chg_state g_chg = { .level = -1, .plugged = -1, .band = "none" };

/* the same shape, holding the state that actually reached the log. The
 * bridge re-emits its sticky battery snapshot periodically (every 30s or
 * so); a repeat of the same status/band must stay silent, and so must a
 * level that moved inside its zone (the crossing is the event). Only
 * charging started/stopped, zone crossings and a source attached/detached
 * are noteworthy. */
static struct chg_state g_log = { .level = -1, .plugged = -1 };

static const char *band_for(const char *status, int level)
{
    /* range names are abstract (lower/middle/upper); the colors
     * and light types per range come from led.conf */
    if (!strcmp(status, "Full"))
        return "upper";

    int first = conf_first_threshold();
    int second = conf_second_threshold();
    /* Both thresholds are required. With either one missing no zone can
     * be decided, so the band stays "none" and nothing is painted; the
     * "conf: loaded" summary already named the unresolved key. */
    if (first == CONF_ABSENT || second == CONF_ABSENT)
        return "none";

    if (!strcmp(status, "Charging")) {
        if (level >= second) return "upper";
        if (level >= first) return "middle";
        return "lower";
    }

    if (!strcmp(status, "Not charging"))
        return (level >= second) ? "upper" : "none";

    return "none";      /* Discharging / Unknown / no data */
}

/* Resolve the band from the current snapshot and keep it there. A broadcast
 * reporting no source attached (plugged=0) overrides the status word, so a
 * stuck/stale "Charging" cannot light the charge LED - this device's battery
 * service has done exactly that. Both band consumers go through here, so the
 * gated and the plain answer cannot drift apart. */
static void chg_reband(void)
{
    const char *band = band_for(g_chg.status, g_chg.level);
    if (g_chg.plugged == 0) band = "none";
    snprintf(g_chg.band, sizeof(g_chg.band), "%s", band);
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

/* The band's colour is REQUIRED: it is the colour its render= line
 * carries. conf_req_color() logs the problem once per config reload, so
 * this stays quiet on every battery re-emission afterwards. */
static int band_color(const char *band, int out[3])
{
    char sec[24];
    snprintf(sec, sizeof(sec), "charge.%s", band);
    return conf_req_color(sec, out);
}

/* ---------------- the kind ---------------- */

/* Apply one named band, described for the pool.
 *
 * Every band owns its OWN section: [charge.lower|middle|upper]
 * render= line, colour included. [charge] base keeps only the
 * thresholds. Empty/unknown bands collapse to "none" (off). */
static int charge_paint(struct evt *e, struct evt_paint *p)
{
    const char *band = e->arg[0] ? e->arg : "none";
    if (strcmp(band, "lower") && strcmp(band, "middle") &&
        strcmp(band, "upper"))
        band = "none";

    /* the band travels in the entry, the status file keeps it verbatim */
    PAINT_BAND(p, band);

    /* The "none"/idle band is ALWAYS off by design. */
    if (!strcmp(band, "none")) return 1;

    char sec[32];
    snprintf(sec, sizeof(sec), "charge.%s", band);
    int rgb[3];
    if (!band_color(band, rgb)) return 0;   /* logged once by conf_req_color */
    PAINT_SEC(p, sec);
    p->r = rgb[0];
    p->g = rgb[1];
    p->b = rgb[2];
    return 1;
}

/* untimed: the band ends only when the bridge says so (a CHG), and a test
 * ends on Disarm or the next press. */
static const struct evt_kind charge_kind = {
    .name     = "charge",
    .def_rank = RANK_CHARGE,
    .flags    = EV_SINGLETON | EV_SCREEN_BYPASS | EV_PERSISTENT | EV_MOD_LOGGED,
    .cap_ms   = NULL,
    .accept   = NULL,
    .paint    = charge_paint,
};
REGISTER_EVT(charge_kind);

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

    g_chg.level = level;
    g_chg.plugged = plugged;
    snprintf(g_chg.status, sizeof(g_chg.status), "%s", status);
    chg_reband();

    /* Log only real transitions, not the bridge's periodic sticky
     * re-emissions: charging started/stopped (status changed or a source
     * was attached/detached) and zone crossings (band changed). The plugged
     * bitmask changes within one session too (the same charger reported as
     * USB, then as AC a second later), and that is not a transition - the
     * daemon acts on "a source is attached" or not, never on which one. */
    if (strcmp(g_chg.status, g_log.status) ||
        (g_chg.plugged != 0) != (g_log.plugged != 0) ||
        strcmp(g_chg.band, g_log.band)) {
        LOGI("charge: %s %d%% plug=%d band=%s",
             g_chg.status[0] ? g_chg.status : "-", g_chg.level,
             g_chg.plugged, g_chg.band);
        g_log = g_chg;
    }

    /* the ordinary path: the entry exists from boot, so a repost only
     * refreshes its payload - the run is not restarted and the pattern
     * keeps its phase. Anything outranking the band keeps the LEDs and the
     * pool hands them back when it frees them. */
    pool_push("charge", NULL, -1, g_chg.band);
}

/* ---------------- the charge zone test ---------------- */

static int  g_charge_test;        /* a test zone is armed             */
static int  g_charge_test_seq;    /* how many presses so far          */

static const char *charge_test_band(int seq)
{
    switch (seq % 3) {
    case 0: return "lower";
    case 1: return "middle";
    default: return "upper";
    }
}

/* Each press advances the fake charge zone lower -> middle -> upper ->
 * and round. The [charge.<band>] config fully decides color + renderer,
 * nothing is hardcoded.
 *
 * The press goes through pool_test, the entry point every other GUI button
 * uses: the pool marks this entry as a test, so it lands even while a call
 * rainbow holds the LEDs. Only the fake zone lives here. */
void charge_test_next(void)
{
    if (!g_charge_test) g_charge_test_seq = 0;
    else                g_charge_test_seq++;
    g_charge_test = 1;
    /* one log line per press comes out of pool_test, zone included */
    pool_test("charge", NULL, -1, charge_test_band(g_charge_test_seq));
}

/* Disarm: the test is over. The pool dropped the test hold but kept the
 * entry - it cannot know that the fake zone was written over the entry's
 * payload, so the real band is pushed back here (which also repaints, the
 * zone that comes back is usually not the one that was showing). */
void charge_test_off(void)
{
    if (!g_charge_test) return;
    g_charge_test = 0;
    pool_push("charge", NULL, -1, g_chg.band);
}

/* ---------------- registry hooks ---------------- */

/* led.conf changed: the band is a function of [charge] thresholds and the
 * battery snapshot the bridge last sent, so the thresholds are re-applied
 * to that snapshot here - a moved threshold can move the zone under a
 * phone that has not changed its level. The repaint of whatever is on the
 * LEDs is the pool's call (pool_config_changed). */
static void charge_refresh(void)
{
    chg_reband();
    pool_push("charge", NULL, -1, g_charge_test
              ? charge_test_band(g_charge_test_seq) : g_chg.band);
}

REGISTER_REFRESH(charge_refresh);



