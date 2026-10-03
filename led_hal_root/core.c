/*
 * core.c - front door and event loop of the modular chgd daemon.
 *
 * Owns the transport and the registries, no policy:
 *   - main loop: select() over the NLS client socket (the only event
 *     source), its listener, the adaptive timerfd and the config inotify
 *   - NLS socket "notify_bus": the NotificationListenerService bridge.
 *     Every line ends in exactly one call into
 *     the event pool (channel.c): ENQ pushes a notify entry,
 *     CAN/CAN_ALL/RING_OFF/VOIP_OFF/MISSED_OFF/ALARM_OFF drop one, the
 *     *_ON lines push their kind, SCREEN carries the backlight polarity,
 *     CHG the charge state, PULSE the notification-light toggle.
 *   - adaptive timerfd: retune_timer() asks the pool how long the shown
 *     entry may still show and sleeps until then. Nothing time-bound (an
 *     unlimited cap, the charge band, a test) keeps the timer DISARMED -
 *     the LED just holds and the phone sleeps until the next event.
 *   - the test dispatch: one mechanism (pool_test) for every GUI test
 *     button, plus lockfile housekeeping
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/timerfd.h>
#include "chgd.h"

#define LOCK_PATH  "/data/local/tmp/led_chgd.lock"
#define NLS_SOCK_NAME "notify_bus"   /* abstract namespace: no fs entry */

int g_tfd = -1;
/* NotificationListenerService client fd. It is the authoritative
 * notification transport; while a client is connected it feeds the
 * whole dispatch pipeline (ENQ/CAN/CAN_ALL + the kind events). */
int g_nls               = -1;
static int g_nls_l      = -1;   /* listening socket for the NLS service */
static char g_nls_buf[1024];    /* line buffer for the client stream    */
static size_t g_nls_len = 0;
static int g_cfg        = -1;   /* inotify fd on the led.conf directory */

/* ---------------- notification lifecycle dispatch ----------------
 * ENQ/CAN/CAN_ALL of the raw route. The kind lines (RING/VOIP/MISSED/
 * ALARM) go straight into their own kinds below: their cancels resolve
 * there, not here. These are the only entry points the socket parser and
 * any future IPC use. */

void notif_enqueue(const char *pkg, int id)
{
    if (!pkg || !pkg[0]) return;
    pool_push("notify", pkg, id, NULL);
}

void notif_cancel(const char *pkg, int id)
{
    if (!pkg || !pkg[0]) return;
    /* a cancel with an id drops that one entry, a cancel_all drops every
     * entry of the package whatever kind it is */
    if (id >= 0) pool_end("notify", pkg, id);
    else         pool_end_pkg(pkg);
}

void notif_cancel_all(const char *pkg)
{
    if (!pkg || !pkg[0]) return;
    pool_end_pkg(pkg);
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
 *   MISSED_ON <id>      dialer missed-call tombstone posted
 *   MISSED_OFF <id>     dialer missed-call tombstone removed; the bridge
 *                       is the classification source (channel
 *                       "missed_calls"), so no call_log query anywhere
 *   ALARM_ON <pkg> <id> clock app's ringing notification posted (the
 *                       bridge matches the notification's own "alarm"
 *                       category)
 *   ALARM_OFF <pkg> <id>
 *                       the same notification removed - the alarm is over
 *   PULSE <0|1>         Settings.System notification_light_pulse changed
 *                       by ANY writer (observer in NLS): 0 disarms the
 *                       notification LEDs right away like stock SystemUI,
 *                       1 just drops the settings cache so the next push
 *                       reads the fresh value
 *   SCREEN <0|1>        ACTION_SCREEN_OFF/ON seen by the NLS app; this
 *                       edge is what empties the basin - the parked
 *                       entries inside [notify] notify_screen_delay_ms
 *                       join the pool, the older ones are dropped - and
 *                       on connect the app replays the current state.
 *   CHG <status> <level> [<plugged>]
 *                       battery snapshot forwarded from
 *                       ACTION_BATTERY_CHANGED: status is a
 *                       BatteryManager constant or a word, level is the
 *                       0..100 capacity, and the optional trailing
 *                       EXTRA_PLUGGED bit gates the band, so a status
 *                       stuck at Charging while nothing is attached does
 *                       not light the charge LED.
 * Only one client at a time; a new connect replaces the old one. On
 * connect the client replays its live state (RING/VOIP/MISSED + every
 * ENQ + SCREEN/PULSE) so a daemon restart mid-call re-arms cleanly.
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

/* the "<pkg> <id>" payload of the lifecycle lines, split out once */
static void nls_ident(const char *p, char *pkg, size_t n, int *id)
{
    while (*p == ' ') p++;
    size_t i = 0;
    while (p[i] && p[i] != ' ' && i < n - 1) { pkg[i] = p[i]; i++; }
    pkg[i] = '\0';
    const char *rest = p + i;
    while (*rest == ' ') rest++;
    *id = *rest ? atoi(rest) : -1;
}

/* an optional single-word payload (the messenger package of VOIP_ON) */
static void nls_word(const char *p, char *out, size_t n)
{
    out[0] = '\0';
    while (*p == ' ') p++;
    size_t i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '\n' && i < n - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

/* the MISSED lines carry no package: their single field IS the bridge id
 * of the tombstone, which is what the entry is keyed by, so a removal is
 * matched against the posting and not blindly applied to whatever is up */
static int nls_id(const char *p)
{
    while (*p == ' ') p++;
    return *p ? atoi(p) : -1;
}

static void nls_cmd(const char *s)
{
    /* Call detection is event-driven from the NLS bridge: it classifies
     * the SIM/dialer notification and posts RING_ON on the live call
     * (incoming=1 / outgoing=0), RING_OFF when the last call notification
     * is gone, MISSED_ON/MISSED_OFF for the missed-call tombstone;
     * messenger calls use VOIP_ON/VOIP_OFF the same way. Each kind is one
     * pool entry with its own budget: the daemon owns no arming state. */

    /* messenger call: VOIP_OFF carries the package, so a second messenger
     * ending its call cannot take down the rainbow of the first one */
    if (!strncmp(s, "VOIP_ON", 7)) {
        char pkg[96];
        nls_word(s + 7, pkg, sizeof(pkg));
        pool_push("voip", pkg[0] ? pkg : NULL, -1, NULL);
        return;
    }
    if (!strncmp(s, "VOIP_OFF", 8)) {
        char pkg[96];
        nls_word(s + 8, pkg, sizeof(pkg));
        pool_end("voip", pkg[0] ? pkg : NULL, -1);
        return;
    }
    /* SIM call: the direction is the entry's payload, so answering does
     * not restart the wave, it only relabels the entry */
    if (!strncmp(s, "RING_ON", 7)) {
        int in = (s[7] == ' ' && (s[8] == '1' || s[8] == '0') && !s[9])
               ? s[8] - '0' : 1;
        pool_push("ring", NULL, -1, in ? "incoming" : "outgoing");
        return;
    }
    if (!strncmp(s, "RING_OFF", 8)) {
        pool_end("ring", NULL, -1);
        return;
    }
    /* Missed-call LED: the bridge classifies the dialer's tombstone
     * (channel "missed_calls") and emits the MISSED edge - the exact
     * opening/closing of the notification IS the event. The tombstone
     * carries no package, only its own id. */
    if (!strncmp(s, "MISSED_ON", 9)) {
        pool_push("missed", NULL, nls_id(s + 9), NULL);
        return;
    }
    if (!strncmp(s, "MISSED_OFF", 10)) {
        pool_end("missed", NULL, nls_id(s + 10));
        return;
    }
    /* The system "Notification light" toggle (Settings.System
     * notification_light_pulse) changed - the NLS ContentObserver caught
     * SOME writer (Settings app, adb, the GUI) and forwarded the fresh
     * value. 0 = stock SystemUI semantics: the notification LEDs go dark
     * right away and nothing is left to flash later. Only the kinds that
     * declare EV_PULSE_GATED (notify, missed) are theirs to drop - a call
     * rainbow, an alarm and the charge band are not notifications and
     * stay up. 1 = just note the new state; the next push gates on it.
     * PULSE state is stored by pulse_note() and replayed by the bridge on
     * connect like SCREEN. */
    if (!strncmp(s, "PULSE ", 6)) {
        int on = (s[6] == '1' && (s[7] == '\0' || s[7] == '\n'));
        if (!on && !(s[6] == '0' && (s[7] == '\0' || s[7] == '\n')))
            return;             /* malformed: ignore the line */
        pulse_note(on);
        if (!on) pool_pulse_off();
        LOGI("pulse: toggle %s via NLS", on ? "on" : "off");
        return;
    }
    if (!strncmp(s, "SCREEN ", 7)) {
        int on = (s[7] == '1');
        if ((s[7] == '1' || s[7] == '0') && (s[8] == '\0' || s[8] == '\n')) {
            screen_note(on);
            LOGI("screen: %s (NLS event)", on ? "on" : "off");
            /* drain the basin on the falling edge, fill it on the
             * rising one */
            pool_screen();
        }
        return;
    }
    /* Charge state: the bridge watches the battery broadcasts and forwards
     * the parsed extras as CHG <status> <level> [<plugged>] */
    if (!strncmp(s, "CHG ", 4)) {
        charge_note(s + 4);
        return;
    }

    /* Notification lifecycle: ENQ/CAN/CAN_ALL/ALARM_ON/ALARM_OFF share the
     * same "<pkg> <id>" payload, only the action differs. */
    {
        int act = 0;                 /* 1 enqueue, 2 cancel, 3 cancel_all,
                                      * 4 alarm on, 5 alarm off */
        const char *p = s;
        if (!strncmp(p, "CAN_ALL ", 8))        { act = 3; p += 8; }
        else if (!strncmp(p, "ALARM_ON ", 9))  { act = 4; p += 9; }
        else if (!strncmp(p, "ALARM_OFF ", 10)){ act = 5; p += 10; }
        else if (!strncmp(p, "ENQ ", 4))       { act = 1; p += 4; }
        else if (!strncmp(p, "CAN ", 4))       { act = 2; p += 4; }
        else return;

        char pkg[96];
        int id;
        nls_ident(p, pkg, sizeof(pkg), &id);
        if (!pkg[0]) return;

        if (act == 1)      notif_enqueue(pkg, id);
        else if (act == 2) notif_cancel(pkg, id);
        else if (act == 4) pool_push("alarm", pkg, id, NULL);
        else if (act == 5) pool_end("alarm", pkg, id);
        else               notif_cancel_all(pkg);
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
        /* No event source anymore - nothing can feed the pool or the
         * charge band, so the daemon sleeps. Entries are NOT dropped: the
         * bridge replays its live state on reconnect, and dropping here
         * would kill a live call's LEDs because the service restarted.
         * The last screen state stays cached; SCREEN is replayed too. */
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

/* state a mod derives from led.conf rather than from the pool: runs on a
 * config reload (SIGALRM / inotify), so the charge band re-applies a moved
 * threshold. The repaint of whatever is on the LEDs is the pool's call. */
void refresh_dispatch(void)
{
    const struct refresh_hook *r;
    for (r = __start_chgd_refresh; r < __stop_chgd_refresh; r++)
        r->fn();
}

/* ---------------- adaptive timer ---------------- */

/* One timerfd serves the pool's single deadline; it is re-armed after every
 * decision the pool makes (retune_timer is called from inside it). The
 * pool is the only thing the core asks:
 *   an entry owns the LEDs -> sleep exactly until ITS budget runs out
 *     (pool_deadline_ms() > 0), which is the accrued lit time of a
 *     notification, the cap of a call rainbow, a missed tombstone, ...
 *   the budget is already spent -> 1 ms. The shown clock freezes while an
 *     entry is displaced, so it can come back past its cap, and disarming
 *     here would let an exhausted effect blink until the next unrelated
 *     event.
 *   nothing time-bound -> DISARMED. The LED holds and the phone sleeps:
 *     the end is event-driven (a cancel, RING_OFF, VOIP_OFF, a CHG, a test
 *     button), so nothing may wake for it. Same when nobody owns the LEDs.
 * Every deadline is a one-shot, re-armed after it fires. */

void retune_timer(void)
{
    if (g_tfd < 0) return;
    long w = pool_deadline_ms();
    long ms = w < 0 ? 1 : (w > 0 ? w : 0);
    const char *why = pool_owner_name();
    if (!why[0]) why = "idle";
    else if (ms == 0) why = "event-driven";
    const char *what = pool_deadline_why();
    if (what[0]) {
        static char buf[64];
        snprintf(buf, sizeof(buf), "%s %s", why, what);
        why = buf;
    }

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec  = ms / 1000L;
    its.it_value.tv_nsec = (ms % 1000L) * 1000000L;
    /* Every wake is a one-shot: the deadline is re-read from the pool
     * after it fires (ms == 0 disarms the timer entirely). Skipped when
     * the same deadline is already pending, which avoids resetting the
     * countdown when several paths retune back-to-back. g_last_set_ms
     * tracks the last TARGET value for the log line only. */
    static long g_last_set_ms = -1;
    struct itimerspec cur;
    if (timerfd_gettime(g_tfd, &cur) == 0) {
        long curms = (long)cur.it_value.tv_sec * 1000L +
                     cur.it_value.tv_nsec / 1000000L;
        if (curms == ms && (cur.it_value.tv_sec || cur.it_value.tv_nsec))
            return;
    }
    timerfd_settime(g_tfd, 0, &its, NULL);
    if (ms != g_last_set_ms) {
        LOGI("timer -> %ldms (%s)", ms, why);
        g_last_set_ms = ms;
    }
}

/* ---------------- signals ---------------- */

static volatile sig_atomic_t g_fake_enq    = 0;
static volatile sig_atomic_t g_fake_dial   = 0;
static volatile sig_atomic_t g_fake_voip   = 0;
static volatile sig_atomic_t g_fake_alarm  = 0;
static volatile sig_atomic_t g_fake_charge = 0;
static volatile sig_atomic_t g_fake_missed = 0;
static volatile sig_atomic_t g_clear       = 0;
static volatile sig_atomic_t g_clear_log   = 0;
static volatile sig_atomic_t g_conf_reload = 0;

static void on_usr1 (int s){ (void)s; g_fake_enq = 1; }
static void on_usr2 (int s){ (void)s; g_clear = 1; }
static void on_hup  (int s){ (void)s; g_fake_dial = 1; }
static void on_winch(int s){ (void)s; g_fake_voip = 1; }
static void on_tstp (int s){ (void)s; g_fake_alarm = 1; }
static void on_quit (int s){ (void)s; g_fake_charge = 1; }
static void on_pwr  (int s){ (void)s; g_fake_missed = 1; }
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

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose = 1;
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
    install_handler(SIGPWR,  on_pwr);
    install_handler(SIGCONT, on_cont);
    install_handler(SIGALRM, on_alrm);

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { perror("timerfd"); return 1; }
    g_tfd = tfd;
    /* disarmed until the pool has decided something worth timing */

    led_init_hw();              /* AW2033: soft reset, Imax, power rails */

    /* the initial visible state: the persistent kinds (the charge band)
     * enter the pool and the first election paints it */
    pool_boot();

    g_nls_l = nls_listen();
    if (g_nls_l < 0)
        LOGI("nls: listen failed (%s)", strerror(errno));
    else
        nls_status_write(0);        /* no client yet: mirror the truth */

    /* config edits become events: watcher flags a reload, see config.c */
    g_cfg = conf_watch_init();
    if (g_cfg < 0)
        LOGI("conf: inotify unavailable (%s), stat fallback", strerror(errno));

    LOGI("chgd running (tfd=%d nls=%d cfg=%d)", tfd, g_nls_l, g_cfg);

    for (;;) {
        /* --- test hooks (also run on EINTR restarts) ---
         * Every one of them is pool_test(kind): one entry, shown by force -
         * no rank check, no screen guard, no budget - and held until
         * Disarm. A test owns no [priority] key, it borrows the real
         * effect's kind and gives it back on Disarm. The per-effect test
         * MODES (what the fake arm must look like) stay in the mod. */
        if (g_fake_enq) {
            g_fake_enq = 0;
            pool_test("notify", "org.telegram.messenger", -1, NULL);
        }
        if (g_fake_dial) {
            g_fake_dial = 0;
            pool_test("ring", NULL, -1, "incoming");
        }
        if (g_fake_voip) {
            g_fake_voip = 0;
            pool_test("voip", NULL, -1, NULL);
        }
        if (g_fake_alarm) {
            g_fake_alarm = 0;
            pool_test("alarm", NULL, -1, NULL);
        }
        if (g_fake_charge) {
            g_fake_charge = 0;
            /* the charge test is a MODE of the band itself: each press
             * steps the fake zone (lower -> middle -> upper -> ...) and the
             * cycle state lives in charge.c */
            charge_test_next();
        }
        if (g_fake_missed) {
            g_fake_missed = 0;
            pool_test("missed", NULL, -1, NULL);
        }
        if (g_clear_log) {
            g_clear_log = 0;
            FILE *lf = fopen("/data/local/tmp/ledd.log", "w");
            if (lf) fclose(lf);     /* truncate */
            LOGI("log cleared (sigcont)");
        }
        if (g_clear) {
            g_clear = 0;
            /* GUI "Disarm" / test hook. No pulse_note(0) here: the toggle
             * state belongs to Settings.System and arrives only as the
             * bridge's PULSE event (live change or connect replay), so the
             * gate always tracks the real setting.
             *
             * Order matters here. The charge test overwrote the band's
             * payload with a fake zone and only the mod knows what the real
             * one is, so the pool clears the holds first and the mod
             * repaints the real band second: one repaint, no fake frame. */
            pool_disarm();
            charge_test_off();
        }
        if (g_conf_reload) {
            /* led.conf flagged - the GUI poked us via SIGALRM, the inotify
             * watcher saw an external writer, or the same write surfaced as
             * several queued flags. The mtime guard collapses those into a
             * single load, then the mods re-derive what they own, the pool
             * re-reads ranks and budgets and repaints the visible state,
             * and the connection mirror is refreshed. */
            g_conf_reload = 0;
            if (conf_file_changed()) {
                conf_note_change();
                conf_maybe_reload();
                refresh_dispatch();
                pool_config_changed();
                nls_status_write(g_nls >= 0 ? 1 : 0);   /* mirror: cold-start value */
            }
        }

        int maxfd = tfd;
        if (g_nls_l > maxfd) maxfd = g_nls_l;
        if (g_nls > maxfd) maxfd = g_nls;
        if (g_cfg > maxfd) maxfd = g_cfg;

        fd_set rfds;
        FD_ZERO(&rfds);
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

            /* the only time-driven end in the daemon: the shown entry's
             * budget ran out */
            pool_on_deadline();

            /* ownership may have changed inside the branch above */
            retune_timer();
        }
    }

    leds_all_off();
    return 1;
}