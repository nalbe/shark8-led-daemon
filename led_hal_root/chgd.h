/*
 * chgd.h - shared API for the modular led_hal_root daemon.
 *
 * Layout:
 *   core (always compiled, never edit for a new feature):
 *     core.c      main loop, select() (NLS socket + listener, timerfd,
 *                 config inotify), NLS socket transport (the only event
 *                 source), shared armed state (g_st), pkg/refresh
 *                 dispatch, adaptive timer
 *     led.c       LED adapter -> AW2033 chip calls (the only writer)
 *     config.c    INI runtime config + generic kv for mod sections
 *     util.c      logging + file read helper + status files + screen
 *                 state cache (NLS SCREEN events only, no sysfs)
 *   mods (drop a new .c into mods/, rebuild, done; pick and choose):
 *     charge.c    charge band eval + LED application. Purely event-driven:
 *                 the band comes from the NLS bridge's CHG command
 *                 (REGISTER_REFRESH boot/SIGALRM re-renders it; no sysfs
 *                 reads, no polling, the timer is disarmed while nothing
 *                 owns the channel)
 *     notify.c    notification pipeline. Claims every unclaimed package
 *                 via the "*" default handler and owns the idle channel
 *                 while the pool has work (adaptive wake; disarm is
 *                 cancel-driven)
 *     ring.c      call rainbow mode (owns "incoming.call") - [ring] sec
 *     missed.c    missed-call LED (owns "missed.call"), driven purely by
 *                 the bridge's MISSED_ON/MISSED_OFF events - colors come
 *                 from [missed], no built-in rule, no call_log access
 *     voip.c      messenger (VoIP) call rainbow (owns "voip.call"),
 *                 driven by NLS RING/VOIP commands
 *
 * The registries below are linker-packed into dedicated sections;
 * the core iterates the section bounds, so a new feature never touches
 * core code. REGISTER_* entries must be static const data.
 *
 * Mode ownership is mutually exclusive by construction for every state
 * of g_st.cur_pkg: "" -> notify while the pool has work (disarmed at
 * true idle), "incoming.call" -> ring, "voip.call" -> voip, "missed.
 * call" -> missed, every other package -> notify.
 */

#ifndef CHGD_H
#define CHGD_H

#include <stddef.h>
#include <time.h>

/* ---------------- logging (util.c) ---------------- */

extern int g_verbose;
void log_line(const char *fmt, ...);
#define LOGI(...) log_line(__VA_ARGS__)
/* runtime logging switch: driven by [led] logging in led.conf */
void log_set_enabled(int on);

/* ---------------- sysfs / device helpers ---------------- */

/* Android's "Notification light" toggle (Settings.System
 * NOTIFICATION_LIGHT_PULSE) honoured by the NOTIFICATION LED only.
 * State comes only from the bridge's NLS PULSE events (pulse_note);
 * unknown before the first event = on. */
int  pulse_on(void);
void pulse_note(int on);        /* NLS PULSE event: the only source */
int  read_line(const char *path, char *out, size_t n);
void atomic_write(const char *path, const char *buf, size_t len);
int  screen_on(void);           /* cached last SCREEN event, 0 until known */
void screen_note(int on);       /* NLS SCREEN event: the only source */
void status_write(const char *mode, const char *band, const char *pkg,
                  int r, int g, int b, const char *engine);
void nls_status_write(int connected);

/* ---------------- LED hardware (led.c, the only writer) ---------------- */

/* per-event renderer: paints [sec] using its own mode + [sec.solid]/
 * [sec.breath]/[sec.wave] chip sections and returns the mode string
 * ("off"|"solid"|"breath"|"wave") for the status engine field.
 * Each chip section owns its own timing keys (rise/hold/fall/offt);
 * the event only supplies the colors. No base-section timing. */
const char *led_event(const char *sec, int r, int g, int b);
void led_init_hw(void);
void leds_all_off(void);
/* resolve [sec]'s active renderer mode without painting (charge.c folds
 * it into its applied-fingerprint so config-only mode edits repaint). */
const char *led_resolve_mode(const char *sec);

/* ---------------- child-process capture ----------------
 * None. The daemon never forks a shell command (no call_log queries,
 * no settings reads): every state arrives as an NLS bridge event. */

/* ---------------- shared state ---------------- */

struct notif_state {
    char   cur_pkg[96];   /* armed package, "" = idle */
    char   owner_pkg[96]; /* kept for compatibility; always "" now - the
                             pseudo-packages (missed.call, incoming.call,
                             voip.call) are armed/disarmed by their own NLS
                             commands, never by a cancel. */
    time_t armed_at;
    int    test;          /* armed from a test hook: ignore the screen */
};

extern struct notif_state g_st;
extern char g_applied_band[64];    /* last charge state applied (fp) */
extern int  g_tfd;                 /* adaptive timerfd            */
extern int  g_nls;                 /* NLS client socket fd, -1 = none */

/* safety pass: a one-shot mode with a deadline, or the defensive
 * ownerless-fallback, wakes this often. Purely event-driven modes with
 * next_wake_ms()==0 (no cap) keep the timer fully DISARMED - the LED
 * just holds and the phone sleeps until the next event. */
#define WATCHDOG_SEC 300

/* notif_max_sec is runtime-configurable via [notify] notif_max_sec in
 * led.conf (config.c); default 1800s, 0 = unlimited. */

/* ---------------- runtime config hooks (config.c) ----------------
 * Early lookups the core consults for user-editable settings. The
 * generic kv lookups at the bottom let mods own their own [sections]
 * in led.conf without config.c knowing the keys exist.
 */
void conf_maybe_reload(void);
int  conf_file_changed(void);   /* mtime guard: 1 load per real write      */
int  conf_suppressed(const char *pkg);
int  conf_pkg_rgb(const char *pkg, int *r, int *g, int *b); /* 1 = set  */
int  conf_first_threshold(void);
int  conf_second_threshold(void);
void conf_notif_rgb(int *r, int *g, int *b);         /* [notify] default_color */
long conf_notif_max_sec(void);
void conf_note_change(void);         /* external writer touched led.conf */
int  conf_watch_init(void);          /* inotify fd on the conf dir, -1 off */
void conf_watch_handle(void);        /* drain events, note changes */
/* generic kv: any [section] key=value in led.conf is readable by name */
const char *conf_get_str(const char *sec, const char *key); /* NULL = absent */
long conf_get_int(const char *sec, const char *key, long def);

/* ---------------- core services ---------------- */

void retune_timer(void);
const struct led_mode *mode_owns(const char *pkg);
/* pkg dispatch: exact-match claiming handlers first (alarm), then the
 * "*" default handler (notify). Returns 1 if something claimed it. */
int  pkg_dispatch(struct notif_state *st, const char *pkg, int id);
/* refresh hooks: every REGISTER_REFRESH entry fires (boot + SIGALRM). */
void refresh_dispatch(void);
void charge_note(const char *s); /* NLS CHG <status> <level> from the bridge */
void apply_charge_leds(void);
void arm_notification(struct notif_state *st, const char *pkg);
void arm_notification_ex(struct notif_state *st, const char *pkg, int test);
void disarm_notification(struct notif_state *st, const char *why);
void notif_enqueue(const char *pkg, int id);
void notif_cancel(const char *pkg, int id);
void notif_cancel_all(const char *pkg);

/* ---------------- notification priority pool (mods/queue.c) --------------
 * The single event ledger:every notification ENQ lands here (handler
 * claimed or not) and lives until it is picked for the LED, cancelled,
 * or expires. arms/disarms still go through the notification channel
 * (g_st.cur_pkg); the pool only decides WHO the channel shows and for
 * how long. LIFO by recency, screen-off staging, resume-with-credit. */
void queue_push(const char *pkg, int id);       /* ENQ: record + arbitrate  */
void queue_remove(const char *pkg, int id);     /* CAN: drop one id         */
void queue_remove_all(const char *pkg);         /* CAN_ALL: drop a package  */
void queue_clear(void);                         /* PULSE off / GUI toggle   */
void queue_arbitrate(void);                     /* single policy entry      */
int  queue_has(const char *pkg);                /* any live entry for pkg   */
int  queue_has_pending(void);                   /* non-active entries await */
int  queue_active(void);                        /* an entry owns the channel*/
long queue_active_remain_ms(void);              /* ms until cap, -1 no cap  */

/* ---------------- extension entry points ---------------- */

int  ring_is_active(void);          /* ring mode armed right now */
int  alarm_is_active(void);         /* alarm mode armed right now */
int  alarm_test(void);              /* SIGTSTP test hook: paint [alarm] now */
int  missed_is_active(void);        /* missed-call LED armed right now */
void arm_ring(int incoming);        /* 1 = incoming, 0 = outgoing */
void arm_ring_ex(int incoming, int test);   /* test=1: hold, ignore telephony */
void ring_off(void);                /* NLS RING_OFF: resolve call end */
int  voip_active(void);             /* messenger-call rainbow armed now */
int  voip_on(void);                 /* NLS call notification posted */
int  voip_off(void);                /* NLS call notification removed */
void charge_test_next(void);        /* SIGQUIT: advance the fake charge zone */
void missed_on(void);               /* NLS MISSED_ON: missed tombstone posted */
void missed_off(void);              /* NLS MISSED_OFF: tombstone removed */

/* ---------------- registries ---------------- */

struct led_rule {          /* internal pseudo-package color only      */
    const char *pkg;       /* (e.g. missed.call); real app colors    */
    unsigned char r, g, b; /* live in led.conf [rules] only          */
};
struct pkg_handler {       /* full owner of a package's notifications */
    const char *pkg;       /* exact package-name match only          */
    int (*fn)(const char *pkg, int id); /* return 1 to claim         */
};
struct led_mode {          /* timer-driven special state */
    const char *name;
    long        tick_ms;   /* heartbeat while armed     */
    int        (*owns)(const char *pkg);
    void       (*tick)(void);
    /* optional adaptive wakeup: when set and >0, the core uses its return
     * value (ms) as a one-shot "sleep until this much later" instead of the
     * fixed tick_ms ticking every cycle. Re-evaluated after every tick, so a
     * mode can sleep for minutes when there is nothing to do and only wake
     * for the next actual deadline (e.g. a timeout cap). NULL = periodic
     * tick_ms heartbeat, the old behaviour. */
    long       (*next_wake_ms)(void);
    /* optional honest label for the timer log; NULL = use .name. A mode
     * that owns a whole channel (e.g. notify owning the "" while the
     * pool has work) should return something truthful about the state. */
    const char *(*label)(void);
};
struct refresh_hook {      /* visible-state refresher: boot + SIGALRM */
    void (*fn)(void);
};

#define _CHG_CAT2(a, b) a##b
#define _CHG_CAT(a, b)  _CHG_CAT2(a, b)

#define REGISTER_RULE(_pkg, _r, _g, _b) \
    static const struct led_rule \
    _CHG_CAT(chg_rule_, __COUNTER__) \
    __attribute__((used, section("chgd_rules"))) = { (_pkg), (_r), (_g), (_b) }

#define REGISTER_HANDLER(_pkg, _fn) \
    static const struct pkg_handler \
    _CHG_CAT(chg_hand_, __COUNTER__) \
    __attribute__((used, section("chgd_handlers"))) = { (_pkg), (_fn) }

#define REGISTER_MODE(_name, _tick_ms, _owns, _tick) \
    static const struct led_mode \
    _CHG_CAT(chg_mode_, __COUNTER__) \
    __attribute__((used, section("chgd_modes"))) = \
        { (_name), (_tick_ms), (_owns), (_tick), NULL, NULL }

/* like REGISTER_MODE, but with an adaptive wakeup fn (see struct led_mode) */
#define REGISTER_MODE_WAKE(_name, _tick_ms, _owns, _tick, _wake) \
    static const struct led_mode \
    _CHG_CAT(chg_mode_, __COUNTER__) \
    __attribute__((used, section("chgd_modes"))) = \
        { (_name), (_tick_ms), (_owns), (_tick), (_wake), NULL }

/* like REGISTER_MODE, but the timer log shows label() instead of the name */
#define REGISTER_MODE_LABEL(_name, _tick_ms, _owns, _tick, _label) \
    static const struct led_mode \
    _CHG_CAT(chg_mode_, __COUNTER__) \
    __attribute__((used, section("chgd_modes"))) = \
        { (_name), (_tick_ms), (_owns), (_tick), NULL, (_label) }

#define REGISTER_REFRESH(_fn) \
    static const struct refresh_hook \
    _CHG_CAT(chg_refr_, __COUNTER__) \
    __attribute__((used, section("chgd_refresh"))) = { (_fn) }

/* section bounds, synthesized by the linker around the registry data */
extern const struct led_rule        __start_chgd_rules[];
extern const struct led_rule        __stop_chgd_rules[];
extern const struct pkg_handler     __start_chgd_handlers[];
extern const struct pkg_handler     __stop_chgd_handlers[];
extern const struct led_mode        __start_chgd_modes[];
extern const struct led_mode        __stop_chgd_modes[];
extern const struct refresh_hook    __start_chgd_refresh[];
extern const struct refresh_hook    __stop_chgd_refresh[];

#endif /* CHGD_H */