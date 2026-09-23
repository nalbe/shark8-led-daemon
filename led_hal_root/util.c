/*
 * util.c - logging, file read helper, live status files, screen state
 * cache. Pure device plumbing; no policy here. Shared by the core and
 * mods. All LED-channel writes live in led.c, not here. No sysfs access:
 * every hardware fact (screen, charge, notifications) comes from the
 * bridge's NLS events.
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

/* ---------------- file read / atomic write helpers ---------------- */

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

/* Atomic text write: full write to <path>.tmp then rename over <path>,
 * so a consumer read can never observe a half-written file. Every status
 * file in the daemon (led_status, notifybridge.status, led_chg) uses
 * this single helper - the classic write-tmp+rename pattern. */
void atomic_write(const char *path, const char *buf, size_t len)
{
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    ssize_t ig = write(fd, buf, len); (void)ig;
    close(fd);
    rename(tmp, path);
}

/* ---------------- system notification-light switch ----------------
 *
 * Android's own "Notification light" toggle lives in
 * Settings.System NOTIFICATION_LIGHT_PULSE (Settings -> Notifications).
 * Only the NOTIFICATION LEDs honour it - charge bands, call rainbows
 * and the alarm are separate "whatever signals", so they must NOT be
 * gated here. Gate points: arm_notification_ex() (mods/notify.c) for the
 * generic notification pool and missed_on() (mods/missed.c) for the
 * missed-call tombstone - the bridge emits it as a notification and the
 * stock toggle must kill it like any other notification LED.
 *
 * State comes ONLY from the bridge's NLS "PULSE <0|1>" event: the
 * notify-bridge watches the settings key with a ContentObserver and
 * forwards every real change; on socket connect it replays the current
 * value (same contract as SCREEN), so the daemon reads no settings and
 * forks no shell for this. Unknown before the first event = on
 * (Android's default), so a hiccup never silently eats notifications. */

static int s_pulse = 1;     /* 0 = toggle off, 1 = on (default until told) */

int pulse_on(void)
{
    return s_pulse;
}

/* NLS PULSE event: the only source. */
void pulse_note(int on)
{
    s_pulse = on ? 1 : 0;
}

/* ---------------- live status file ---------------- */

/* Write the NLS bridge connection state for the GUI. The daemon owns this
 * file: it runs as root, so the write always succeeds and the value tracks
 * the actual accept/EOF events, not the NLS app's root luck. Atomic write
 * via atomic_write() (tmp + rename). */
#define STATUS_PATH "/data/local/tmp/led_status"
#define NLS_STATUS_PATH "/data/local/tmp/notifybridge.status"

void nls_status_write(int connected)
{
    char buf[48];
    int len = snprintf(buf, sizeof(buf), "connected=%d\n",
                       connected ? 1 : 0);
    atomic_write(NLS_STATUS_PATH, buf, (size_t)len);
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
 * Atomic write via atomic_write() (tmp + rename), same as led_chg. */
void status_write(const char *mode, const char *band, const char *pkg,
                  int r, int g, int b, const char *engine)
{
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "ts=%ld\nmode=%s\nband=%s\npkg=%s\ncolor=%d,%d,%d\nengine=%s\n",
        (long)time(NULL), mode, band, pkg ? pkg : "", r, g, b,
        engine ? engine : "");
    atomic_write(STATUS_PATH, buf, (size_t)len);
}

/* ---------------- screen state ---------------- */

/* Screen state cache, fed ONLY by the bridge's NLS "SCREEN <0|1>" event
 * (the daemon reads no sysfs and owns no screen polling - the notify-
 * bridge registers ACTION_SCREEN_ON/OFF and replays the current polarity
 * on socket connect, so the first event arrives right after the client
 * connects). A park flashes the instant SCREEN 0 lands; there is no 1s
 * poll anywhere in this path. On bridge disconnect the last known value
 * simply stays - with the bridge down nothing can feed the notification
 * pool anyway, and the reconnect replay refreshes the state. */
static int s_screen = 0;    /* 0 off, 1 on; unknown before the first SCREEN */

/* returns 1 = screen on, 0 = off (unknown before the first SCREEN event
 * reports off -> a notification shows, matching the pre-hardcode
 * behavior). */
int screen_on(void)
{
    return s_screen;
}

/* NLS told us the real screen state (the only source). */
void screen_note(int on)
{
    s_screen = on ? 1 : 0;
}