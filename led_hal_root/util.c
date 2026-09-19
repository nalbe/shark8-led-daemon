/*
 * util.c - logging, sysfs read/write helpers, live status file,
 * screen detection. Pure device plumbing; no policy here. Shared by the
 * core and mods. All LED-channel writes live in led.c, not here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include "chgd.h"

int g_verbose = 0;

/* Runtime logging switch, driven by [led] logging in led.conf (config.c
 * calls log_set_enabled() every time the config is (re)loaded). When off,
 * log_line() becomes a pure no-op: no file write, no reload probing. The
 * switch is re-read by the core on SIGALRM / inotify (led.conf edited),
 * never per log line. The GUI's ledd.log (tail) card just shows whatever
 * was written before the switch left. */
static int g_log_allow = 1;

void log_set_enabled(int on)
{
    g_log_allow = on ? 1 : 0;
}

void log_line(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (g_verbose) {
        fprintf(stderr, "%s\n", msg);
        return;
    }
    if (!g_log_allow) return;
    FILE *n = fopen("/data/local/tmp/ledd.log", "a");
    if (!n) return;
    long sz = ftell(n);
    if (sz > 65536) {
        long half = sz / 2;
        char *tmp = malloc(half);
        if (tmp) {
            size_t r = fread(tmp, 1, (size_t)half, n);
            if (r > 0) {
                fclose(n);
                n = fopen("/data/local/tmp/ledd.log", "w");
                if (n) { fwrite(tmp, 1, r, n); }
            }
            free(tmp);
        } else {
            fclose(n);
            n = fopen("/data/local/tmp/ledd.log", "w");
        }
        if (!n) return;
    }
    time_t t = time(NULL);
    struct tm tm_;
    char tbuf[32];
    localtime_r(&t, &tm_);
    strftime(tbuf, sizeof(tbuf), "%m-%d %H:%M:%S", &tm_);
    fprintf(n, "%s %s\n", tbuf, msg);
    fclose(n);
}

/* ---------------- sysfs helpers ---------------- */

int read_line(const char *path, char *out, size_t n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r <= 0) return -1;
    out[r] = '\0';
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    return 0;
}

/* Verbose LED plumbing log. Every sysfs write to the RGB / backlight
 * channels is recorded so a run can be reconstructed afterwards: exactly
 * what chgd told each channel, in what order. Each line carries the
 * full path so channel + attribute are unambiguous. */
static int led_path(const char *path, const char **ch)
{
    static const char *LEDS[] = { "red", "green", "blue", "lcd-backlight" };
    for (int i = 0; i < (int)(sizeof(LEDS) / sizeof(LEDS[0])); i++)
        if (strstr(path, LEDS[i])) { *ch = LEDS[i]; return 1; }
    return 0;
}

void write_sys(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        if (g_verbose) fprintf(stderr, "write %s: %s\n", path, strerror(errno));
        return;
    }
    /* Log LED plumbing under [led] trace_sysfs only: these lines
     * duplicate the higher-level "[led] all off / breathe" summary and
     * flood the log (3 channels x 3 attributes per transition). Debug
     * knob, off by default. */
    const char *ch;
    if (led_path(path, &ch) && !strstr(path, "/brightness") &&
        conf_get_int("led", "trace_sysfs", 0))
        LOGI("LED %s <- %s", path, val);
    ssize_t ignored = write(fd, val, strlen(val));
    (void)ignored;
    close(fd);
}

/* ---------------- system notification-light switch ----------------
 *
 * Android's own "Notification light" toggle lives in
 * Settings.System NOTIFICATION_LIGHT_PULSE (Settings -> Notifications).
 * Only the NOTIFICATION LED honours it - charge bands, call rainbows
 * and the alarm are separate "whatever signals", so they must NOT be
 * gated here. One gate point: arm_notification_ex() (mods/notify.c).
 *
 * Evaluation is PURELY EVENT-DRIVEN: called on notification arming only,
 * never from a timer, so the daemon sleeps as hard with the toggle off
 * as with it on. The settings provider is only reachable via a forked
 * shell, so the value is cached LIGHT_PULSE_TTL seconds; "0" = off;
 * anything else including "null" (unset = Android's default ON) and
 * query failures means ON, so a hiccup never silently eats notifications.
 * light_pulse_invalidate() drops the cache: the GUI calls it (via
 * SIGUSR2) right after flipping the toggle, so the next arm reads the
 * fresh value instead of a stale one. */

#define LIGHT_PULSE_TTL 3

static time_t s_pl_last = 0;
static int    s_pl_val  = 1;

int light_pulse_enabled(void)
{
    time_t now = time(NULL);
    if (s_pl_last == 0 || now - s_pl_last >= LIGHT_PULSE_TTL) {
        s_pl_last = now;
        static char out[64];
        char *argv[] = {
            (char *)"/system/bin/settings", (char *)"get",
            (char *)"system", (char *)"notification_light_pulse", NULL
        };
        if (run_capture(argv, out, sizeof(out)) &&
            out[0] == '0' && (out[1] == '\0' || out[1] == '\n'))
            s_pl_val = 0;               /* exactly "0" = off */
        else
            s_pl_val = 1;               /* "1", "null", garbage, or failure */
    }
    return s_pl_val;
}

void light_pulse_invalidate(void)
{
    s_pl_last = 0;                      /* next call re-reads the provider */
}

/* ---------------- live status file ---------------- */

#define STATUS_PATH "/data/local/tmp/led_status"
#define STATUS_TMP  "/data/local/tmp/led_status.tmp"

/* Write the NLS bridge connection state for the GUI. The daemon owns this
 * file: it runs as root, so the write always succeeds and the value tracks
 * the actual accept/EOF events, not the NLS app's root luck. Atomic tmp +
 * rename, same pattern as led_status. */
#define NLS_STATUS_PATH "/data/local/tmp/lednls.status"
#define NLS_STATUS_TMP  "/data/local/tmp/lednls.status.tmp"

void nls_status_write(int connected)
{
    char buf[96];
    int len = snprintf(buf, sizeof(buf), "connected=%d\nwatchdog_ms=%ld\n",
                       connected ? 1 : 0,
                       conf_get_int("led", "watchdog_ms", 60000));
    int fd = open(NLS_STATUS_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    ssize_t ig = write(fd, buf, (size_t)len); (void)ig;
    close(fd);
    rename(NLS_STATUS_TMP, NLS_STATUS_PATH);
}

/* Write the current LED-owner state for external consumers (GUI etc.).
 * mode: charge | notify | ring | voip | missed | alarm
 * band: lower/middle/upper/none (charge only, "" otherwise)
 * pkg:  armed notification / ring / voip pseudo-package, "" when none
 * engine: how the LED is actually driven - tells a consumer whether the
 *   raw /sys/class/leds brightness value reflects what the eye sees:
 *     hw     hardware blink/breath/blink on the AW2033: the brightness
 *            node reads the constant peak while the chip pulses - live
 *            reads are NOT representative
 *     wave   traveling wave on the chip: same, live reads not live
 *     solid  static brightness: fixed value, live read == the color
 *     off    all channels dark
 * Atomic tmp + rename, same pattern as charge.c's led_chg. */
void status_write(const char *mode, const char *band, const char *pkg,
                  int r, int g, int b, const char *engine)
{
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "ts=%ld\nmode=%s\nband=%s\npkg=%s\ncolor=%d,%d,%d\nengine=%s\n",
        (long)time(NULL), mode, band, pkg ? pkg : "", r, g, b,
        engine ? engine : "");
    int fd = open(STATUS_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    ssize_t ig = write(fd, buf, (size_t)len); (void)ig;
    close(fd);
    rename(STATUS_TMP, STATUS_PATH);
}

/* ---------------- screen state ---------------- */

/* Screen state cache. This kernel emits NO uevent when the backlight
 * changes (probed with a netlink dump: keyevent 223/224 flips
 * lcd-backlight/brightness 27 -> 0 -> 27 but broadcasts nothing), so the
 * NLS app forwards ACTION_SCREEN_ON/OFF as "SCREEN <0|1>" over the
 * socket. While the bridge has reported the state we trust it and a
 * parked notification flashes the instant the screen falls - fully
 * event-driven, no 1s sysfs poll. When the bridge is absent/down we fall
 * back to polling the node. */
static int s_screen     = -1;   /* last known: -1 unknown, 0 off, 1 on */
static int s_screen_evt = 0;    /* 1 = state comes from the NLS event  */

/* returns 1 = screen on, 0 = off */
int screen_on(void)
{
    if (s_screen_evt && s_screen >= 0)
        return s_screen;
    /* primary: lcd backlight level */
    char buf[32];
    if (read_line("/sys/class/leds/lcd-backlight/brightness",
                  buf, sizeof(buf)) == 0) {
        return atoi(buf) > 0;
    }
    /* fallback: fb blank node, "0" = unblanked/on */
    if (read_line("/sys/class/graphics/fb0/blank", buf, sizeof(buf)) == 0) {
        return strcmp(buf, "0") == 0;
    }
    return 0;                    /* unknown -> assume off, show notification */
}

/* NLS told us the real screen state (event-driven source). */
void screen_note(int on)
{
    s_screen = on ? 1 : 0;
    s_screen_evt = 1;
}

/* 1 = a trusted event source is feeding screen state (no sysfs poll). */
int screen_event_driven(void)
{
    return s_screen_evt;
}

/* bridge gone: forget the event state, fall back to sysfs polling. */
void screen_source_reset(void)
{
    s_screen = -1;
    s_screen_evt = 0;
}