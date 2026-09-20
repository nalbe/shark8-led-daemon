/*
 * core.c - front door and event loop of the modular chgd daemon.
 *
 * Owns the transport and the registries, no policy:
 *   - main loop: select() over netlink uevents, NLS client socket,
 *     timerfd
 *   - netlink KOBJECT_UEVENT -> uev_dispatch(): every REGISTER_UEVENT
 *     hook whose match substring hits the raw message fires (charge
 *     owns "power_supply")
 *   - NLS socket "notify_bus": the NotificationListenerService bridge
 *     is the ONLY notification transport (no logdr/logcat anymore).
 *     ENQ/CAN/CAN_ALL feed the same pkg_dispatch pipeline; VOIP_ON/OFF,
 *     RING_ON/OFF arm the call rainbows directly.
 *   - adaptive timerfd: retune_timer() picks the heartbeat of the mode
 *     that owns g_st.cur_pkg - ring owns "incoming.call", voip owns
 *     "voip.call", dialer/alarm/notify own theirs. notify owns "" only
 *     while the pool still has work; at true idle (empty pool + empty
 *     cur_pkg) no mode owns anything and the timer is disarmed - the
 *     charge band repaints via power_supply uevent or SIGALRM.
 *     A mode with no deadline (next_wake_ms()==0, i.e. max_sec=0) keeps
 *     the timer DISARMED too: once its LED is up, nothing runs and the
 *     phone sleeps until the next event (cancel, RING_OFF, VOIP_OFF).
 *     Dialer's missed-call verification window wins over idle while
 *     pending
 *   - signal test hooks and watchdog / lockfile housekeeping
 *
 * All policy lives in mods/: charge.c, notify.c, ring.c, dialer.c.
 * The core only walks the registry sections and calls shared services
 * declared in chgd.h (pkg_dispatch, uev_dispatch, refresh_dispatch,
 * retune_timer). No LED, no band, no arming here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <poll.h>
#include <linux/netlink.h>
#include "chgd.h"

#define STATE_PATH "/data/local/tmp/led_chg"
#define LOCK_PATH  "/data/local/tmp/led_chgd.lock"
#define NLS_SOCK_NAME "notify_bus"   /* abstract namespace: no fs entry */

int g_tfd = -1;
/* NotificationListenerService client fd. It is the authoritative
 * notification transport; while a client is connected it feeds the
 * whole dispatch pipeline (ENQ/CAN/CAN_ALL + VOIP/RING commands). */
int g_nls  = -1;
static int g_nls_l = -1;        /* listening socket for the NLS service */
static char g_nls_buf[1024];    /* line buffer for the client stream   */
static size_t g_nls_len = 0;
static int g_cfg = -1;          /* inotify fd on the led.conf directory */

/* ---------------- shared armed state ---------------- */

struct notif_state g_st;               /* single instance, shared universe */

/* ---------------- registry dispatch ---------------- */

/* any mode may own any pkg string, including "" (notify owns it only
 * while the pool has pending/active work); modes decide by pkg content */
const struct led_mode *mode_owns(const char *pkg)
{
    if (!pkg) return NULL;
    const struct led_mode *m;
    for (m = __start_chgd_modes; m < __stop_chgd_modes; m++)
        if (m->owns(pkg)) return m;
    return NULL;
}

/* exact-match claiming handlers first (dialer), then the "*" default
 * handler (notify's suppress/screen/dedup/arm pipeline). */
int pkg_dispatch(struct notif_state *st, const char *pkg, int id)
{
    (void)st;
    const struct pkg_handler *h;
    for (h = __start_chgd_handlers; h < __stop_chgd_handlers; h++)
        if (strcmp(h->pkg, "*") && !strcmp(h->pkg, pkg) && h->fn(pkg, id)) {
            LOGI("claimed by handler: %s", pkg);
            return 1;
        }
    for (h = __start_chgd_handlers; h < __stop_chgd_handlers; h++)
        if (!strcmp(h->pkg, "*") && h->fn(pkg, id))
            return 1;
    return 0;
}

/* netlink KOBJECT_UEVENT consumers: charge ("power_supply"). */
void uev_dispatch(const char *msg)
{
    const struct uevent_hook *h;
    for (h = __start_chgd_uevents; h < __stop_chgd_uevents; h++)
        if (strstr(msg, h->match))
            h->fn();
}

/* notification_cancel dispatch: drop the cancelled notification from the
 * priority pool. The LED is disarmed only when the last live entry for
 * the armed package leaves the pool (real dismissal); a cancel+post
 * round-trip keeps at least one entry, so a rebuild never kills the LED.
 * Purely event-driven, no timers, no windows, no polling. */
static void cancel_dispatch(struct notif_state *st, const char *pkg, int id)
{
    /* a generic cancel invalidates the whole affected entry for this
     * package - it must never flash later, armed or not */
    if (id >= 0)
        queue_remove(pkg, id);
    else
        queue_remove_all(pkg);
    (void)st;
}

/* ---------------- NLS notification dispatch (public API) -------------
 * The app's NotificationListenerService is the only notification source.
 * Every ENQ lands in the priority pool through the handlers (queue.c
 * owns arbitration); CAN/CAN_ALL remove entries. Pseudo-packages
 * (missed.call, incoming.call, voip.call) are owned by their own modes,
 * so their cancels are resolved there, not here. These are the only
 * entry points the socket parser and any future IPC uses. */

void notif_enqueue(const char *pkg, int id)
{
    if (!pkg || !pkg[0]) return;
    pkg_dispatch(&g_st, pkg, id);
}

void notif_cancel(const char *pkg, int id)
{
    if (!pkg || !pkg[0]) return;
    cancel_dispatch(&g_st, pkg, id);
}

void notif_cancel_all(const char *pkg)
{
    if (!pkg || !pkg[0]) return;
    cancel_dispatch(&g_st, pkg, -1);
}

/* ---------------- NLS socket transport ----------------
 * Abstract-namespace SOCK_STREAM server ("notify_bus"). The standalone
 * NLS app connects, sends one text command per line:
 *   ENQ <pkg> <id>      notification posted
 *   CAN <pkg> <id>      notification removed
 *   CAN_ALL <pkg>       every notification of the package gone
 *   VOIP_ON [pkg]       messenger call notification posted (rainbow)
 *   VOIP_OFF [pkg]      messenger call notification removed
 *   RING_ON <0|1>       SIM call notification posted (1 incoming, 0
 *                       outgoing/ongoing)
 *   RING_OFF            last SIM call notification removed
 *   PULSE <0|1>         Settings.System notification_light_pulse changed
 *                       by ANY writer (observer in NLS): 0 disarms the
 *                       notification LED right away like stock SystemUI,
 *                       1 just drops the 3s settings cache so the next
 *                       arm reads the fresh value.
 *   SCREEN <0|1>        ACTION_SCREEN_OFF/ON seen by the NLS app. This
 *                       kernel fires no uevent on backlight change, so
 *                       this edge is what lets a parked notification
 *                       flash with zero polling; on connect the app
 *                       replays the current state.
 * Only one client at a time; a new connect replaces the old one. This is
 * the ONLY notification transport: there is no logdr/logcat fallback. On
 * connect the client replays its live state (RING/VOIP + every ENQ) so a
 * daemon restart mid-call re-arms cleanly.
 */

static int nls_listen(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    /* abstract namespace: leading NUL then the name, no filesystem entry */
    size_t len = offsetof(struct sockaddr_un, sun_path) + 1 +
                 sizeof(NLS_SOCK_NAME) - 1;
    memcpy(sa.sun_path + 1, NLS_SOCK_NAME, sizeof(NLS_SOCK_NAME) - 1);
    if (bind(fd, (struct sockaddr *)&sa, (socklen_t)len) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 2) < 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    return fd;
}

static void nls_accept(void)
{
    if (g_nls_l < 0) return;
    int fd = accept(g_nls_l, NULL, NULL);
    if (fd < 0) return;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    if (g_nls >= 0) {
        LOGI("nls: replacing stale connection");
        close(g_nls);
    }
    g_nls = fd;
    g_nls_len = 0;
    LOGI("nls: client connected");
    nls_status_write(1);
    retune_timer();             /* drop any pending backoff */
}

static void nls_cmd(const char *s)
{
    /* Call detection is event-driven from the NLS bridge: it classifies
     * the SIM/dialer notification and posts RING_ON on the live call
     * (incoming=1 / outgoing=0), RING_OFF when the last call notification
     * is gone; messenger calls use VOIP_ON/VOIP_OFF the same way. No
     * telephony/dumpsys polling anywhere in this path. */
    if (!strcmp(s, "VOIP_ON")  || !strncmp(s, "VOIP_ON ",  8))  { voip_on();  return; }
    if (!strcmp(s, "VOIP_OFF") || !strncmp(s, "VOIP_OFF ", 9)) { voip_off(); return; }
    if (!strncmp(s, "RING_ON", 7)) {
        int in = (s[7] == ' ' && (s[8] == '1' || s[8] == '0')) && !s[9] ? s[8] - '0' : 1;
        arm_ring(in);
        return;
    }
    if (!strcmp(s, "RING_OFF")) {
        ring_off();
        return;
    }
    /* The system "Notification light" toggle (Settings.System
     * notification_light_pulse) changed - the NLS ContentObserver caught
     * SOME writer (Settings app, adb, the GUI) and forwarded the fresh
     * value. 0 = stock SystemUI semantics: drop the cache AND disarm the
     * notification LED that might be showing right now. 1 = just invalidate
     * the 3s cache so the next arm re-reads. No polling anywhere. */
    if (!strncmp(s, "PULSE ", 6)) {
        int on = (s[6] == '1' && (s[7] == '\0' || s[7] == '\n'));
        if (!on && !(s[6] == '0' && (s[7] == '\0' || s[7] == '\n')))
            return;             /* malformed: ignore the line */
        light_pulse_invalidate();
        if (!on) {
            queue_clear();                      /* toggle off: no backlog */
            disarm_notification(&g_st, "pulse-off");
        }
        LOGI("pulse: toggle %s via NLS", on ? "on" : "off");
        return;
    }
    if (!strncmp(s, "SCREEN ", 7)) {
        int on = (s[7] == '1');
        if ((s[7] == '1' || s[7] == '0') && (s[8] == '\0' || s[8] == '\n')) {
            screen_note(on);
            LOGI("screen: %s (NLS event)", on ? "on" : "off");
            /* flash a parked notification the instant the screen falls -
             * no 1s poll needed, this IS the edge */
            queue_arbitrate();
            retune_timer();
        }
        return;
    }
    /* Notification lifecycle: ENQ/CAN/CAN_ALL share the same
     * "name id" payload, only the action differs. */
    {
        int act = 0;                 /* 1 enqueue, 2 cancel, 3 cancel_all */
        const char *p = s;
        if (!strncmp(p, "CAN_ALL ", 8))      { act = 3; p += 8; }
        else if (!strncmp(p, "ENQ ", 4))     { act = 1; p += 4; }
        else if (!strncmp(p, "CAN ", 4))     { act = 2; p += 4; }
        else return;

        while (*p == ' ') p++;
        char pkg[96];
        size_t i = 0;
        while (p[i] && p[i] != ' ' && i < sizeof(pkg) - 1) { pkg[i] = p[i]; i++; }
        pkg[i] = '\0';
        if (!i) return;
        const char *rest = p + i;
        while (*rest == ' ') rest++;
        int id = *rest ? atoi(rest) : -1;

        if (act == 1)             notif_enqueue(pkg, id);
        else if (act == 2)        notif_cancel(pkg, id);
        else                      notif_cancel_all(pkg);
    }
}

static void nls_read(void)
{
    char tmp[512];
    ssize_t n = recv(g_nls, tmp, sizeof(tmp), 0);
    if (n <= 0) {
        /* EOF / error: the app's service went away */
        LOGI("nls: client disconnected");
        close(g_nls);
        g_nls = -1;
        g_nls_len = 0;
        nls_status_write(0);
        /* no event source anymore: fall back to polling the backlight so a
         * parked notification still flashes on the next screen-off. */
        screen_source_reset();
        retune_timer();
        return;
    }
    for (ssize_t i = 0; i < n; i++) {
        char c = tmp[i];
        if (c == '\n') {
            g_nls_buf[g_nls_len] = '\0';
            if (g_nls_len) nls_cmd(g_nls_buf);
            g_nls_len = 0;
        } else if (g_nls_len < sizeof(g_nls_buf) - 1) {
            g_nls_buf[g_nls_len++] = c;
        } else {
            g_nls_len = 0;      /* overflow: drop the line */
        }
    }
}

/* visible-state refreshers: run at boot and again on SIGALRM (led.conf
 * edited). charge.c re-evaluates its band and re-applies the LEDs. */
void refresh_dispatch(void)
{
    const struct refresh_hook *r;
    for (r = __start_chgd_refresh; r < __stop_chgd_refresh; r++)
        r->fn();
}

/* ---------------- adaptive timer ---------------- */

/* One timerfd serves all pending timeouts; it is re-armed on every state
 * transition instead of ticking blindly every 30s:
 *   dialer verification window open  -> 2s missed-call recheck
 *   mode owns cur_pkg               -> its heartbeat (notify 1s while a
 *                                      top parks, ring 1s, ...)
 *   mode with a next_wake deadline  -> one-shot until that deadline
 *   mode with no deadline           -> one-shot WATCHDOG_SEC safety pass
 *   nothing owns (true idle)        -> disarmed: events re-arm the timer
 * If the owning mode exposes an adaptive next_wake_ms(), that value wins:
 * it is programmed as a ONE-SHOT "sleep until the next real deadline"
 * (re-armed after every tick) instead of a fixed periodic heartbeat, so
 * notify may sleep for a minute when there is nothing to do and only
 * wake for the timeout cap. */
void retune_timer(void)
{
    if (g_tfd < 0) return;
    const char *why;
    long ms;
    int one_shot = 0;
    if (g_call_checks_left > 0)              { ms = CALL_RECHECK_MS;   why = "call check"; }
    else {
        const struct led_mode *m = mode_owns(g_st.cur_pkg);
        if (m) {
            why = m->label ? m->label() : m->name;
            if (m->next_wake_ms) {
                long w = m->next_wake_ms();
                if (w > 0) {
                    ms = w;              /* sleep exactly until the deadline */
                } else {
                    /* 0 = no deadline at all (permanent LED, disarm is
                     * purely event-driven): disarm the timer completely.
                     * After the LED is armed there is nothing time-bound
                     * left, so no wakeup at all and the phone sleeps. The
                     * old fallback to WATCHDOG_SEC re-armed a one-shot on
                     * every tick - an eternal wake every 5 minutes while a
                     * no-cap LED idled, and its tick was a no-op anyway. */
                    ms = 0;
                    why = "event-driven";
                }
                if (ms > 0) one_shot = 1;    /* re-armed after each tick */
            } else {
                ms = m->tick_ms;         /* plain periodic heartbeat */
            }
        } else if (g_st.cur_pkg[0])          { ms = WATCHDOG_SEC*1000L; why = "watchdog"; }
        else {
            /* true idle: nothing owns the LED. No heartbeat at all -
             * a charge band change repaints via power_supply uevent or
             * SIGALRM (led.conf edit), a new notification arms the timer
             * itself. A stale 300s tick here was pure noise. */
            ms = 0;
            why = "idle";
        }
    }
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec  = ms / 1000L;
    its.it_value.tv_nsec = (ms % 1000L) * 1000000L;
    /* periodic heartbeat only when the mode owns a fixed cadence; an
     * adaptive wake is a one-shot that the next tick re-arms */
    if (!one_shot) {
        its.it_interval.tv_sec  = its.it_value.tv_sec;
        its.it_interval.tv_nsec = its.it_value.tv_nsec;
    }
    /* skip if already running this policy (avoids resetting the countdown
     * and duplicate log lines when several paths retune back-to-back).
     * g_last_set_ms tracks the last TARGET value so one-shot re-arms
     * to the same interval (curms=0 after fire) are not logged -
     * they would flood the log with thousands of identical lines. */
    static long g_last_set_ms = -1;
    struct itimerspec cur;
    long curms;
    if (timerfd_gettime(g_tfd, &cur) == 0) {
        curms = (long)cur.it_value.tv_sec * 1000L +
                cur.it_value.tv_nsec / 1000000L;
        if (curms == ms && (cur.it_value.tv_sec || cur.it_value.tv_nsec))
            return;
        /* a periodic policy already running at the SAME period keeps its
         * phase: restarting the countdown from zero on every unrelated
         * retune (screen toggle, NLS connect/cancel, uevent) shifts the
         * tick grid, so a fixed cadence like the charge recheck looks
         * coupled to whatever event retuned last. One-shots (cap expiry,
         * adaptive deadlines) still re-arm from zero - a full window from
         * the event is their whole point. */
        if (!one_shot && ms > 0 &&
            cur.it_interval.tv_sec  == ms / 1000L &&
            cur.it_interval.tv_nsec == (ms % 1000L) * 1000000L &&
            curms > 0)
            return;
    }
    timerfd_settime(g_tfd, 0, &its, NULL);
    if (ms != g_last_set_ms) {
        LOGI("timer -> %ldms (%s)%s", ms, why, one_shot ? " one-shot" : "");
        g_last_set_ms = ms;
    }
}

/* ---------------- signals ---------------- */

static volatile sig_atomic_t g_fake_enq    = 0;
static volatile sig_atomic_t g_fake_dial   = 0;
static volatile sig_atomic_t g_fake_voip   = 0;
static volatile sig_atomic_t g_fake_alarm  = 0;
static volatile sig_atomic_t g_fake_charge = 0;
static volatile sig_atomic_t g_clear       = 0;
static volatile sig_atomic_t g_clear_log   = 0;
static volatile sig_atomic_t g_conf_reload = 0;

static void on_usr1 (int s){ (void)s; g_fake_enq = 1; }
static void on_usr2 (int s){ (void)s; g_clear = 1; }
static void on_hup  (int s){ (void)s; g_fake_dial = 1; }
static void on_winch(int s){ (void)s; g_fake_voip = 1; }
static void on_tstp (int s){ (void)s; g_fake_alarm = 1; }
static void on_quit (int s){ (void)s; g_fake_charge = 1; }
static void on_cont (int s){ (void)s; g_clear_log = 1; }
static void on_alrm(int s)
{
    (void)s;
    /* Every real poke is a GUI "save" poke sent AFTER the file was
     * already rewritten, so the inotify watcher carries that same change
     * as its own event - doing a reload here too would load the file a
     * second time per save (two identical "conf: loaded" lines, two
     * refreshes). With a live watcher the poke is pure redundancy: skip
     * it. It stays the only reload kick when inotify is unavailable
     * (stat fallback / g_cfg < 0). */
    if (g_cfg < 0) g_conf_reload = 1;
}

static void install_handler(int sig, void (*h)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h;              /* no SA_RESTART: select -> EINTR */
    sigaction(sig, &sa, NULL);
}

/* ---------------- test dispatch ---------------- */

/* any test button takes over the LED unconditionally: disarm whatever
 * is active first (notification or ring), then arm the new test. */
static void test_disarm(const char *why)
{
    if (g_st.cur_pkg[0])
        disarm_notification(&g_st, why);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
    }

    if (once) {
        eval_and_write();           /* mods/charge.c: band -> state file */
        char buf[64] = "";
        read_line(STATE_PATH, buf, sizeof(buf));
        fputs(buf, stdout);
        return 0;
    }

    int lfd = open(LOCK_PATH, O_RDWR | O_CREAT, 0666);
    if (lfd < 0) { perror("open lock"); return 1; }
    if (flock(lfd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "chgd: another instance owns the lock\n");
        return 0;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    install_handler(SIGUSR1, on_usr1);
    install_handler(SIGUSR2, on_usr2);
    install_handler(SIGHUP,  on_hup);
    install_handler(SIGWINCH, on_winch);
    install_handler(SIGTSTP, on_tstp);
    install_handler(SIGQUIT, on_quit);
    install_handler(SIGCONT, on_cont);
    install_handler(SIGALRM, on_alrm);

    int nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT);
    if (nl < 0) { perror("socket"); return 1; }
    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    sa.nl_groups = 1;
    if (bind(nl, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind netlink");
        return 1;
    }
    /* never let a spurious readiness block us */
    fcntl(nl, F_SETFL, fcntl(nl, F_GETFL, 0) | O_NONBLOCK);

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { perror("timerfd"); return 1; }
    g_tfd = tfd;
    /* disarmed until first retune_timer() below */

    memset(&g_st, 0, sizeof(g_st));

    led_init_hw();              /* AW2033: soft reset, Imax, power rails */

    /* initial visible state: mods refresh themselves (charge band) */
    refresh_dispatch();

    g_nls_l = nls_listen();
    if (g_nls_l < 0)
        LOGI("nls: listen failed (%s)", strerror(errno));
    else
        nls_status_write(0);        /* no client yet: mirror the truth */

    /* config edits become events: watcher flags a reload, see config.c */
    g_cfg = conf_watch_init();
    if (g_cfg < 0)
        LOGI("conf: inotify unavailable (%s), stat fallback", strerror(errno));

    static unsigned char buf[16384];
    LOGI("chgd running (nl=%d tfd=%d nls=%d cfg=%d)", nl, tfd, g_nls_l, g_cfg);
    retune_timer();

    for (;;) {
        /* --- test hooks (also run on EINTR restarts) --- */
        if (g_fake_enq) {
            g_fake_enq = 0;
            /* test telegram: arm directly, bypassing the screen-on guard
             * so the button works while the app is open in front of you */
            test_disarm("test switch");
            arm_notification_ex(&g_st, "org.telegram.messenger", 1);
        }
        if (g_fake_dial) {
            g_fake_dial = 0;
            /* test incoming call: rainbow, held (test mode ignores
             * telephony state, does not die from "call ended") */
            test_disarm("test switch");
            arm_ring_ex(1, 1);
        }
        if (g_fake_voip) {
            g_fake_voip = 0;
            /* test voip call: messenger-call rainbow, held until Disarm
             * or the configured [voip] max_sec cap */
            test_disarm("test switch");
            voip_on();
        }
        if (g_fake_alarm) {
            g_fake_alarm = 0;
            /* test alarm: [alarm] renderer now, even if a call rainbow
             * currently owns the channel */
            test_disarm("test switch");
            alarm_test();
        }
        if (g_fake_charge) {
            g_fake_charge = 0;
            /* test charge: step the fake zones per press (lower -> middle
             * -> upper -> ...), painted by [charge.<band>] config; the
             * cycle state lives in charge.c, no pre-disarm here */
            charge_test_next();
        }
        if (g_clear_log) {
            g_clear_log = 0;
            FILE *lf = fopen("/data/local/tmp/ledd.log", "w");
            if (lf) fclose(lf);     /* truncate */
            LOGI("log cleared (sigcont)");
        }
        if (g_clear) {
            g_clear = 0;
            /* GUI flipped the "Notification light" toggle OFF: drop the
             * cached settings value so the next arm reads the fresh one,
             * and kill whatever notification LED is showing right now. */
            light_pulse_invalidate();
            queue_clear();
            disarm_notification(&g_st, "sigusr2");
        }
        if (g_conf_reload) {
            /* led.conf flagged - the GUI poked us via SIGALRM, the inotify
             * watcher saw an external writer, or the same write surfaced as
             * several queued flags. The mtime guard collapses those into a
             * single load, then the visible-state mods re-apply whatever is
             * active and the connection mirror is refreshed. */
            g_conf_reload = 0;
            if (conf_file_changed()) {
                conf_note_change();
                conf_maybe_reload();
                refresh_dispatch();
                nls_status_write(g_nls >= 0 ? 1 : 0);   /* mirror: cold-start value */
            }
        }

        int maxfd = nl > tfd ? nl : tfd;
        if (g_nls_l > maxfd) maxfd = g_nls_l;
        if (g_nls > maxfd) maxfd = g_nls;
        if (g_cfg > maxfd) maxfd = g_cfg;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(nl, &rfds);
        FD_SET(tfd, &rfds);
        if (g_nls_l >= 0) FD_SET(g_nls_l, &rfds);
        if (g_nls >= 0) FD_SET(g_nls, &rfds);
        if (g_cfg >= 0) FD_SET(g_cfg, &rfds);

        int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) continue;   /* signal: flags handled above */
            perror("select");
            break;
        }
        if (rc == 0) continue;

        /* --- kernel uevents: drain everything queued --- */
        if (FD_ISSET(nl, &rfds)) {
            for (;;) {
                ssize_t n = recv(nl, buf, sizeof(buf) - 1, MSG_DONTWAIT);
                if (n <= 0) break;
                buf[n] = '\0';
                uev_dispatch((char *)buf);
            }
        }

        /* --- led.conf edited by ANY writer: reload this loop turn --- */
        if (g_cfg >= 0 && FD_ISSET(g_cfg, &rfds)) {
            conf_watch_handle();
            g_conf_reload = 1;      /* refresh + re-push, handled up top */
        }

        /* --- NLS client (the only notification transport) --- */
        if (g_nls_l >= 0 && FD_ISSET(g_nls_l, &rfds))
            nls_accept();
        if (g_nls >= 0 && FD_ISSET(g_nls, &rfds))
            nls_read();

        /* --- adaptive housekeeping --- */
        if (FD_ISSET(tfd, &rfds)) {
            uint64_t exp;
            ssize_t ig = read(tfd, &exp, sizeof(exp)); (void)ig;

            /* dialer verification window: no-op when closed */
            maybe_call_check();

            /* the mode that owns cur_pkg does the policy tick */
            const struct led_mode *m = mode_owns(g_st.cur_pkg);
            if (m) m->tick();
            else   LOGI("no mode owns %s", g_st.cur_pkg);

            /* policy may have changed inside the branches above */
            retune_timer();
        }
    }

    leds_all_off();
    return 1;
}