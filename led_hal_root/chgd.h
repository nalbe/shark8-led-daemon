/*
 * chgd.h - shared API for the modular led_hal_root daemon.
 *
 * Layout:
 *   core (always compiled, never edit for a new feature):
 *     core.c      main loop, select() (NLS socket + listener, timerfd,
 *                 config inotify), NLS socket transport (the only event
 *                 source), the socket line parser, adaptive timer
 *     channel.c   the event pool: every LED effect of the tree lives here
 *                 as an entry, ONE decision point for "what blinks right
 *                 now" and for how long
 *     led.c       LED adapter -> AW2033 chip calls (the only writer) and
 *                 the applied fingerprint
 *     config.c    INI runtime config + generic kv for mod sections
 *     util.c      logging + file read helper + status files + screen
 *                 state cache
 *   mods (drop a new .c into mods/, rebuild, done; pick and choose):
 *     charge.c    charge band eval; the payload of its own kind
 *     notify.c    notification colour rules + the [suppress] filter
 *     ring.c      call rainbow mode (SIM calls)
 *     voip.c      messenger call rainbow
 *     missed.c    missed-call LED
 *     alarm.c     alarm clock LED
 *
 * Which package is what is decided in the bridge, never here: core.c parses
 * the bridge's event lines and hands each one to the pool by KIND name, so
 * no mod carries a package list.
 *
 * The registry below is linker-packed into a dedicated section;
 * REGISTER_EVT entries must be static const data.
 *
 * ---------------- THE EVENT POOL ----------------
 * Every LED effect is one entry (struct evt) in channel.c's pool. An entry
 * is pushed when the bridge reports it, keeps its state when it does not
 * get the LEDs, and dies when the bridge takes the event back:
 *
 *   push       pool_push(kind, pkg, id, arg)   the bridge event
 *   end        pool_end/pool_end_pkg           the matching OFF edge / CAN
 *   elect      internal                        after every mutation: the one
 *                                             place that picks the owner
 *
 * The selection order is DATA, not code: [priority] in led.conf, one key
 * per kind name (ring, voip, alarm, missed, notify, charge), higher wins.
 * A missing key warns once and DROPS that kind. Equal configured ranks
 * fall back to def_rank (RANK_* below); equal on both, the freshest push
 * wins. Shipped default: ring > voip > alarm > missed > notify > charge.
 *
 * A KIND is what a mod registers (struct evt_kind). It supplies the
 * optional config policy only:
 *   cap_ms()    the budget of lit time for one entry (0 = untimed, -1 =
 *               the required key is missing -> the entry is dropped)
 *   accept()    optional filter (the [suppress] list)
 *   paint()     fill an evt_paint with the renderer section and colours.
 *               The pool then owns the chip write (led_event), the status
 *               file and the show clock - a mod never touches hardware.
 * The flags are the parts of the policy that are not per-package config:
 *   EV_SINGLETON       one entry of this kind at a time
 *   EV_SCREEN_BYPASS   the screen state never blocks this kind
 *   EV_PULSE_GATED     obeys the Android "Notification light" toggle
 *   EV_PERSISTENT      lives from boot, no OFF edge can end it
 *   EV_MOD_LOGGED      this mod narrates its own events, so the pool writes
 *                      no arrival line for this kind
 *
 * Lifetime of one entry:
 *   WAIT   in the general pool, waiting for its turn (behind a call
 *          rainbow, or behind the ranking)
 *   PARK   the basin: the screen is lit and this kind does not bypass that
 *          guard. The bridge's SCREEN 0 edge is the only thing that lets an
 *          entry out - inside [notify] notify_screen_delay_ms it joins the
 *          general pool, past it the entry is dropped. No timer runs there
 *   SHOW   on the LEDs: its accrued lit time is banked here and nowhere
 *          else, so a displacement never eats its budget and a resume
 *          continues from the same place
 *   spent  a flag on top of WAIT: the lit-time budget is used up, so the
 *          entry stays in the pool and dark until its own end edge and a
 *          repost of the same event gets no second budget. A test or a
 *          raised cap shows it again.
 * Charge is persistent and paints its "none" band as off, so the LEDs
 * always have exactly one owner.
 * The lit-time budget is the only clock in the daemon: the basin holds a
 * timestamp, not a deadline, so waiting is purely event-driven.
 */

#ifndef CHGD_H
#define CHGD_H

#include <stddef.h>
#include <stdio.h>
#include <time.h>

/* ---------------- logging (util.c) ---------------- */

/* The logging method of the daemon:
 * log_line() writes /data/local/tmp/ledd.log (or stderr with -v), and
 * [led] logging in led.conf switches the routine event trace - a "warn: "
 * line (a broken preset, an unparsed [rules] tail, a config table
 * overflow, an unranked effect) is written whatever the switch says, so
 * turning the trace off never hides a broken key. "grep warn:" separates
 * the problems from the trace.
 *
 * Every line is a decision, so a line has to be readable on its own: an
 * entry is named by the fields it carries (channel.c's ev_name).
 */
extern int g_verbose;
void log_line(const char *fmt, ...);
#define LOGI(...) log_line(__VA_ARGS__)
/* runtime logging switch: driven by [led] logging in led.conf */
void log_set_enabled(int on);

/* ---------------- sysfs / device helpers ---------------- */

/* Android's "Notification light" toggle (Settings.System
 * NOTIFICATION_LIGHT_PULSE) honoured by the NOTIFICATION kinds only
 * (notify + missed, see EV_PULSE_GATED). Call rainbows, alarms and charge
 * are deliberately unaffected. State comes only from the bridge's PULSE
 * events (pulse_note); unknown before the first event = on. */
int  pulse_on(void);
void pulse_note(int on);        /* PULSE event */
int  read_line(const char *path, char *out, size_t n);
void atomic_write(const char *path, const char *buf, size_t len);
int  screen_on(void);           /* cached last SCREEN event, 0 until known */
void screen_note(int on);       /* SCREEN event */
/* The ONE status writer, called by the pool alone: mode is the kind name,
 * band the charge band ("" for everything else), pkg the label the mod
 * painted under. */
void status_write(const char *mode, const char *band, const char *pkg,
                  int r, int g, int b, const char *engine);
void nls_status_write(int connected);

/* ---------------- LED hardware (led.c, the only writer) ---------------- */

/* Paint one event: resolves the active renderer mode of [sec] and
 * programs the chip with (r,g,b). Mode, timing and per-channel currents
 * come from that section's render= line (config.c expands it into the
 * flat keys led.c reads) - the event only supplies section + colors.
 * sec == NULL paints the "off" state. Returns the mode string for the
 * status file ("off"|"solid"|"breath"|"wave") or NULL when nothing was
 * painted (no render= line, broken preset - already logged by led.c).
 *
 * The chip is programmed only when this call really changes the visible
 * state, so a repost of the same payload keeps the running pattern phase
 * instead of restarting it. led_invalidate() drops that fingerprint: the
 * pool calls it whenever the LEDs change owner or led.conf was reloaded. */
const char *led_event(const char *sec, int r, int g, int b);
void led_init_hw(void);
void leds_all_off(void);
void led_invalidate(void);

/* ---------------- shared state ---------------- */

extern int g_tfd;                 /* adaptive timerfd                */
extern int g_nls;                 /* NLS client socket fd, -1 = none */

/* ---------------- runtime config hooks (config.c) ----------------
 * Early lookups the core consults for user-editable settings. The
 * generic kv lookups at the bottom let mods own their own [sections]
 * in led.conf without config.c knowing the keys exist.
 */
void conf_maybe_reload(void);
int  conf_file_changed(void);   /* mtime guard: 1 load per real write   */
int  conf_suppressed(const char *pkg);
int  conf_pkg_rgb(const char *pkg, int *r, int *g, int *b); /* 1 = set  */
/* ABSENT (-1) = the file defines no usable value.
 * Callers must check and drop. */
#define CONF_ABSENT (-1)
int  conf_first_threshold(void);
int  conf_second_threshold(void);
int  conf_notif_rgb(int *r, int *g, int *b);      /* 1 = set, 0 = drop  */

/* bumped on every real load: a cache that survives a reload would keep
 * serving values the file no longer defines. The pool stamps its cached
 * ranks and budgets with it. */
unsigned conf_generation(void);

void conf_note_change(void);         /* external writer touched led.conf   */
int  conf_watch_init(void);          /* inotify fd on the conf dir, -1 off */
void conf_watch_handle(void);        /* drain events, note changes         */
/* generic kv: any [section] key=value in led.conf is readable by name */
const char *conf_get_str(const char *sec, const char *key); /* NULL = absent */
/* conf_get_int takes a default and is reserved for [led] logging, the one
 * value the daemon may decide for itself. POLICY values are REQUIRED and
 * must use these: 1 + out, or 0 plus a once-only log and a dropped event. */
long conf_get_int(const char *sec, const char *key, long def);
int  conf_req_int(const char *sec, const char *key, long lo, long hi, long *out);
int  conf_req_color(const char *sec, int out[3]);
/* preset section a notification for [pkg] must use: the synthetic
 * [notify.<pkg>] when that rule carries an extended tail, else the
 * shared [notify] (color-only rules). Callers pick "notify"
 * themselves for packages without a rule. */
const char *conf_notify_sec(const char *pkg);
/* does a [section] exist at all? (e.g. a synthetic per-rule preset) */
int  conf_sec_exists(const char *sec);

/* ---------------- core services ---------------- */

/* The timer. core.c owns the fd, the pool owns what has to be waited for,
 * so the two sides are one function each: the pool re-arms the timer after
 * every decision it makes, the timer asks pool_deadline_ms() how long it
 * may sleep and calls pool_on_deadline() when it fires. The lit-time budget
 * of the shown entry is the only thing on that clock; nothing else sleeps. */
void retune_timer(void);

/* run every mod's config-reload hook (charge re-applies a moved threshold) */
void refresh_dispatch(void);

/* ---------------- charge (mods/charge.c) ----------------
 * The battery state is the one input that is NOT a notification edge per
 * event: the bridge's CHG line carries the whole snapshot, so this mod
 * resolves the band itself and pushes it into its own kind. The core only
 * forwards the line. */
void charge_note(const char *s);      /* CHG <status> <level> [<plugged>] */

/* the charge test is a mode of the band's own entry, not a kind: each press
 * steps the fake zone and Disarm restores the real band. */
void charge_test_next(void);
void charge_test_off(void);

/* ---------------- the event pool (channel.c) ----------------
 * The single authority on "what blinks right now". It owns the entries,
 * the ranking, the show clocks and the status file; the mods only
 * describe a kind and paint it. */

#define EV_MAX 64               /* one slot per live notification     */

enum evt_state { EV_WAIT, EV_PARK, EV_SHOW };

/* Compiled default ranking, also the tie-break for equal [priority]
 * values. Only the RELATIVE order matters - a user config replaces
 * these freely (bigger number = higher priority). */
#define RANK_CHARGE 0     /* charge band (its "none" band paints off)  */
#define RANK_NOTIFY 10    /* ordinary notifications                     */
#define RANK_MISSED 20    /* missed-call tombstone                     */
#define RANK_ALARM  30    /* alarm clock                               */
#define RANK_VOIP   40    /* messenger call rainbow                    */
#define RANK_RING   50    /* SIM call rainbow                          */

/* what one paint should put on the chip. Filled by the kind's paint()
 * hook, acted on by the pool: the mod names the section and the colours,
 * the pool programs the chip and writes the status file.
 *
 * The fields are BUFFERS, not pointers: the pool reads them after the hook
 * has returned, so a paint hook cannot hand out the address of its own
 * stack (the charge band builds its section name at runtime). Use the
 * PAINT_* helpers below, never a bare assignment. */
struct evt_paint {
    char   sec[48];     /* renderer section; "" = the off state      */
    int    r, g, b;     /* colours for that section                  */
    char   label[96];   /* status pkg field: the real or pseudo one  */
    char   band[16];    /* status band field (charge only, else "")  */
};

#define PAINT_SEC(p, _sec)   snprintf((p)->sec,   sizeof((p)->sec),   "%s", (_sec))
#define PAINT_LABEL(p, _l)   snprintf((p)->label, sizeof((p)->label), "%s", (_l))
#define PAINT_BAND(p, _b)    snprintf((p)->band,  sizeof((p)->band),  "%s", (_b))

struct evt {
    const struct evt_kind *k;
    char   pkg[96];       /* real package, "" for a pseudo one        */
    int    id;            /* bridge id, -1 = none                     */
    char   arg[32];       /* mod payload: charge band, ring direction */
    long   seq;           /* push order, the recency tie-break        */
    int    test;          /* a test button owns it (force, no cap)   */
    int    spent;         /* budget spent: dark, kept for its end edge*/
    int    st;            /* enum evt_state                           */
    time_t created;       /* push time: the basin's age reference     */
    time_t opened;        /* when the current show run started        */
    long   shown_ms;      /* accrued lit time, the resume credit     */
    long   cap_ms;        /* budget, 0 = untimed, -1 = unpaintable    */
    unsigned gen;         /* config generation cap_ms was read at     */
    unsigned tried;       /* selection round it lost in (anti-loop)   */
};

enum {
    EV_SINGLETON     = 1 << 0,  /* one entry of this kind at a time     */
    EV_SCREEN_BYPASS = 1 << 1,  /* the screen never blocks this kind    */
    EV_PULSE_GATED   = 1 << 2,  /* obeys notification_light_pulse       */
    EV_PERSISTENT    = 1 << 3,  /* lives from boot, no OFF edge ends it */
    EV_MOD_LOGGED    = 1 << 4   /* the mod narrates its own events      */
};

struct evt_kind {
    const char *name;      /* [priority] key + status mode + log label */
    int         def_rank;  /* compiled default / tie-break             */
    unsigned    flags;
    long      (*cap_ms)(struct evt *e);   /* budget of lit time        */
    int       (*accept)(struct evt *e);   /* NULL = take everything     */
    int       (*paint)(struct evt *e, struct evt_paint *p);
};
struct evt_registry { const struct evt_kind *k; };

/* create the persistent kinds and paint the initial state */
void pool_boot(void);

/* the bridge event: record it (or refresh the entry that already holds
 * this kind/pkg/id) and re-elect. pkg/id are the notification identity,
 * arg is the mod payload. */
void pool_push(const char *kind, const char *pkg, int id, const char *arg);
/* the matching end: RING_OFF / VOIP_OFF / MISSED_OFF / ALARM_OFF / CAN */
void pool_end(const char *kind, const char *pkg, int id);
/* CAN_ALL: every entry of this package, any kind */
void pool_end_pkg(const char *pkg);
/* PULSE 0: the gated kinds have nothing left to show, drop their entries */
void pool_pulse_off(void);
/* a SCREEN edge landed: drain the basin on the falling one, fill it on the
 * rising one */
void pool_screen(void);
/* Disarm: drop the test entries, the notifications and whatever is
 * showing; the other kinds keep waiting and take the LEDs right back. */
void pool_disarm(void);
/* led.conf was reloaded: the ranks and budgets are re-read and the
 * visible state is repainted. */
void pool_config_changed(void);
/* a GUI test button: the entry is shown by force - no rank check, no
 * screen guard, no budget - and the hold is the button's: a real event for
 * that kind neither cancels it nor overwrites its payload until Disarm
 * (the real state stays tracked, it is just not what the LEDs show). */
void pool_test(const char *kind, const char *pkg, int id, const char *arg);
/* the registered kind under this [priority] name, NULL when no mod
 * registered one. How the core names an effect for the test buttons. */
const struct evt_kind *pool_kind(const char *name);
/* ms left of the next deadline: the shown entry's lit-time budget.
 * 0 nothing time-bound, >0 ms left, <0 fire now. What the adaptive
 * timer sleeps on. The basin has no deadline - SCREEN 0 empties it. */
long pool_deadline_ms(void);
/* which deadline it is ("budget"), for the log */
const char *pool_deadline_why(void);
/* timer tick: the pool re-reads itself and drops whatever time is up */
void pool_on_deadline(void);
/* the label the adaptive timer logs, "" when the pool is idle */
const char *pool_owner_name(void);

/* ---------------- registries ---------------- */

#define _CHG_CAT2(a, b) a##b
#define _CHG_CAT(a, b)  _CHG_CAT2(a, b)

/* a kind enters the pool registry by name: the struct itself is declared
 * by the mod with designated initializers (order-safe), this only
 * publishes its address. */
#define REGISTER_EVT(_kind) \
    static const struct evt_registry \
    _CHG_CAT(chg_evt_, __COUNTER__) \
    __attribute__((used, section("chgd_events"))) = { &(_kind) }

/* section bounds, synthesized by the linker around the registry data */
extern const struct evt_registry __start_chgd_events[];
extern const struct evt_registry __stop_chgd_events[];

/* ---------------- config-reload hooks ----------------
 * Some state is NOT in the pool and is recomputed when led.conf changes:
 * the charge band is a function of [charge] thresholds and the battery
 * snapshot the bridge last sent, so a threshold edit can move it. The
 * visible repaint is the pool's business (pool_config_changed) - a hook
 * only refreshes what the mod derives for itself. */

struct refresh_hook { void (*fn)(void); };
#define REGISTER_REFRESH(_fn) \
    static const struct refresh_hook \
    _CHG_CAT(chg_ref_, __COUNTER__) \
    __attribute__((used, section("chgd_refresh"))) = { &(_fn) }

extern const struct refresh_hook __start_chgd_refresh[];
extern const struct refresh_hook __stop_chgd_refresh[];

#endif /* CHGD_H */