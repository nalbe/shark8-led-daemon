/*
 * channel.c - the event pool: what blinks right now, decided in one place.
 *
 * Every LED effect of the tree is an entry here (struct evt, chgd.h). The
 * bridge event pushes an entry, its OFF edge drops it, and pool_select()
 * picks the one that owns the LEDs. A mod registers a KIND (struct
 * evt_kind) and paints it.
 *
 * Selection order, all of it DATA:
 *   [priority] in led.conf, one required key per kind name, higher wins
 *   -> def_rank (RANK_*) as the tie-break for equal keys
 *   -> push order (seq) as the tie-break inside one kind: the freshest
 *      event is the one the user just caused
 * A test entry outranks all of it (a test must show what the user asked
 * to see) and holds the LEDs until Disarm.
 * A missing/unparsable [priority] key warns once and drops that kind -
 * a rank is what orders an entry against the others, so there is no
 * compiled-in substitute to fall back on.
 *
 * Who may show right now:
 *   - a test: always
 *   - the entry that is already showing: yes, unchanged. Turning the
 *     screen on must not kill a live blink, and its budget only ends on
 *     the lit-time timer
 *   - a budget that cannot be resolved (required key missing): dropped
 *   - a spent budget: the entry stays in the pool and dark until its own
 *     end edge, because a repost is the same event and must not be handed
 *     a second budget
 *   - otherwise the screen decides, for every kind that does not declare
 *     EV_SCREEN_BYPASS: the entry settles in the basin (EV_PARK) and the
 *     bridge's SCREEN 0 edge is the only thing that lets it out
 * Nothing below the winner gets in the way: the scan simply continues
 * with the next candidate, which is why "a notification is waiting while
 * the screen is lit" still shows the charge band underneath.
 *
 * The basin holds no timer. An entry sits there with the wall-clock stamp
 * of its push, and the SCREEN 0 edge walks the whole thing: inside [notify]
 * notify_screen_delay_ms an entry joins the general pool and the ranking
 * decides who shows, past it the entry is dropped. So the lit-time budget
 * is the only clock the daemon keeps, and it is the pool's single deadline.
 *
 * Show time is banked in ONE place, when a run ends (a handover, a drop,
 * the cap), and lit_ms() adds the run in progress on top - that is the one
 * answer both the cap check and the deadline read. The budget therefore
 * drains only while the entry is really on the LEDs and freezes while it
 * waits or is parked, so a displaced notification resumes on the same
 * second it left.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "chgd.h"

static struct evt g_ev[EV_MAX];
static long   g_seq;       /* monotone push counter: the recency order  */
static unsigned g_round;   /* selection round, for the "already tried"  */
static struct evt *g_show; /* the entry on the LEDs, NULL = dark        */

/* ---------------- ranks: [priority] in led.conf ---------------- */

/* An unparsable or missing rank is a silent behaviour change if it is
 * ignored, so it is reported - but only once per distinct (kind, value)
 * pair: a broken line would otherwise flood the log for as long as it
 * stays in the file. */
#define RANK_WARN_MAX 8
static struct {
    char name[24];
    char val[24];
} s_rank_warn[RANK_WARN_MAX];
static int s_rank_warn_n;

static void warn_rank(const char *name, const char *val)
{
    for (int i = 0; i < s_rank_warn_n; i++)
        if (!strcmp(s_rank_warn[i].name, name) &&
            !strcmp(s_rank_warn[i].val, val))
            return;
    if (s_rank_warn_n < RANK_WARN_MAX) {
        snprintf(s_rank_warn[s_rank_warn_n].name,
                 sizeof(s_rank_warn[0]), "%s", name);
        snprintf(s_rank_warn[s_rank_warn_n].val,
                 sizeof(s_rank_warn[0]), "%s", val);
        s_rank_warn_n++;
    }
    if (!strcmp(val, "<missing>"))
        LOGI("warn: [priority] has no key for \"%s\" - this effect stays "
             "dark until the key is in led.conf", name);
    else if (!strcmp(val, "<empty>"))
        LOGI("warn: [priority] %s= is empty - this effect stays dark until "
             "the key is a whole number", name);
    else
        LOGI("warn: [priority] %s=\"%s\" is not a whole number - this "
             "effect stays dark until the key is one", name, val);
}

/* The rank one kind gets from the file, cached per config generation:
 * a selection walks every entry, and a kv lookup is a linear scan - the
 * ranks are read again only when led.conf actually changed. */
#define RANK_CACHE 16
static struct {
    const struct evt_kind *k;
    int      rank;
    unsigned gen;
} s_rank[RANK_CACHE];

static int read_rank(const struct evt_kind *k)
{
    const char *v = conf_get_str("priority", k->name);
    if (!v) { warn_rank(k->name, "<missing>"); return -1; }
    while (*v == ' ' || *v == '\t') v++;
    if (!*v) { warn_rank(k->name, "<empty>"); return -1; }
    char *end = NULL;
    long x = strtol(v, &end, 10);
    if (end == v || *end) {           /* not a number (or trailing junk) */
        warn_rank(k->name, v);
        return -1;
    }
    return (int)x;
}

static int kind_rank(const struct evt_kind *k, unsigned gen)
{
    int slot = -1;
    for (int i = 0; i < RANK_CACHE; i++) {
        if (s_rank[i].k == k) {
            if (s_rank[i].gen == gen) return s_rank[i].rank;
            slot = i;
            break;
        }
        if (!s_rank[i].k && slot < 0) slot = i;
    }
    if (slot < 0) return read_rank(k);        /* registry outgrew the cache */
    s_rank[slot].k = k;
    s_rank[slot].gen = gen;
    s_rank[slot].rank = read_rank(k);
    return s_rank[slot].rank;
}

/* ---------------- entries ---------------- */

/* The entry as the log names it: the kind, then the package and the
 * bridge id when the event carried them. */
#define EV_LOG_NAME 160
static const char *ev_named(const struct evt_kind *k, const char *pkg, int id,
                            char *buf)
{
    char head[160];
    snprintf(head, sizeof(head), "%s", k->name);
    if (pkg && pkg[0]) {
        size_t n = strlen(head);
        snprintf(head + n, sizeof(head) - n, " %s", pkg);
    }
    if (id >= 0) snprintf(buf, EV_LOG_NAME, "%s #%d", head, id);
    else         snprintf(buf, EV_LOG_NAME, "%s", head);
    return buf;
}

static const char *ev_name(const struct evt *e, char *buf)
{
    return ev_named(e->k, e->pkg, e->id, buf);
}

static void ev_free(struct evt *e)
{
    memset(e, 0, sizeof(*e));
}

static void bank_show(struct evt *e)
{
    if (e->opened) {
        e->shown_ms += (long)(time(NULL) - e->opened) * 1000L;
        e->opened = 0;
    }
    if (e->st == EV_SHOW) e->st = EV_WAIT;
}

/* How long this entry has been on the LEDs: the banked runs plus the one
 * in progress. The ONE definition - the cap check and the deadline must
 * not disagree, or a budget that the timer calls spent never ends. */
static long lit_ms(const struct evt *e)
{
    long ms = e->shown_ms;
    if (e->opened) ms += (long)(time(NULL) - e->opened) * 1000L;
    return ms;
}

/* an end line addresses one entry: an empty field is a wildcard, which
 * is how RING_OFF (no package, no id) reaches the live call and how a
 * VOIP_OFF from a second messenger cannot end the first one's call. */
static int ev_matches(const struct evt *e, const char *pkg, int id)
{
    if (pkg && pkg[0] && e->pkg[0] && strcmp(e->pkg, pkg)) return 0;
    if (id >= 0 && e->id >= 0 && e->id != id) return 0;
    return 1;
}

/* the entry that owns this identity. A singleton kind has exactly one, so
 * the kind alone identifies it and a new push from another package just
 * replaces its payload. */
static struct evt *ev_find(const struct evt_kind *k, const char *pkg, int id)
{
    for (int i = 0; i < EV_MAX; i++) {
        struct evt *e = &g_ev[i];
        if (!e->k || e->k != k) continue;
        if (k->flags & EV_SINGLETON)
            return ev_matches(e, pkg, id) ? e : NULL;
        if (e->id == id && !strcmp(e->pkg, pkg ? pkg : "")) return e;
    }
    return NULL;
}

/* a free slot, else the oldest waiter (LIFO eviction: on a full pool the
 * oldest entry still waiting loses its place to the fresh event). The
 * eviction is REPORTED - a silently discarded notification is exactly the
 * kind of "just like that" loss the pool must not have. The entry on the
 * LEDs is never a candidate. */
static struct evt *ev_slot(void)
{
    struct evt *old = NULL;
    for (int i = 0; i < EV_MAX; i++) {
        if (!g_ev[i].k) return &g_ev[i];
        if (g_ev[i].st != EV_SHOW && (!old || g_ev[i].seq < old->seq))
            old = &g_ev[i];
    }
    if (!old) return NULL;                     /* every slot is on screen */
    char who[EV_LOG_NAME];
    LOGI("warn: pool full (%d entries), evicted the oldest waiter %s",
         EV_MAX, ev_name(old, who));
    return old;
}

/* the entry is gone for good: its run ends here, so bank it before the
 * state goes away or that lit time is lost. */
static void ev_drop(struct evt *e, const char *why)
{
    char who[EV_LOG_NAME];
    LOGI("pool: %s dropped (%s, lit=%ldms)", ev_name(e, who), why, e->shown_ms);
    if (e == g_show) {
        bank_show(e);
        g_show = NULL;
        led_invalidate();
    }
    ev_free(e);
}

static void ev_spawn(struct evt *e, const struct evt_kind *k,
                     const char *pkg, int id)
{
    ev_free(e);
    e->k = k;
    snprintf(e->pkg, sizeof(e->pkg), "%s", pkg ? pkg : "");
    e->id = id;
    e->st = EV_WAIT;
    e->created = time(NULL);
}

/* ---------------- budgets ---------------- */

/* How long one entry may spend on the LEDs, milliseconds. 0 = untimed
 * (the entry lives until its own end edge), -1 = the required config key
 * is missing and the entry must not be shown with an invented lifetime.
 * Resolved once per config generation, like the ranks. */
static long entry_cap(struct evt *e)
{
    if (!e->k->cap_ms) return 0;
    unsigned gen = conf_generation();
    if (e->gen == gen) return e->cap_ms;
    e->cap_ms = e->k->cap_ms(e);
    e->gen = conf_generation();
    return e->cap_ms;
}

/* The basin window, shared by every kind that does not bypass the screen
 * guard: how old a parked entry may be when the screen falls. One [notify]
 * value, REQUIRED: with no bound in the file the age check has nothing to
 * judge against, and inventing one would either flash an entry the user
 * has stared past or drop a fresh one. */
static int basin_window_s(long *out)
{
    long ms;
    if (!conf_req_int("notify", "notify_screen_delay_ms", 0, 3600000, &ms))
        return 0;
    *out = ms / 1000L;
    return 1;
}

/* may this entry put its event on the chip right now? Entries that cannot
 * are either parked or dropped here, and the scan moves on to the next
 * candidate - that is the whole arbitration, there is no separate
 * preemption path. */
static int entry_showable(struct evt *e)
{
    if (e->test) return 1;          /* a test bypasses every gate below */

    long cap = entry_cap(e);
    if (cap < 0) {
        ev_drop(e, "no budget in led.conf");
        return 0;
    }
    if (cap > 0 && lit_ms(e) >= cap) {
        /* The entry keeps its slot and stays dark until its own end edge:
         * a repost is the same event, so it may not hand out a second
         * budget - the dialer re-posts its call notification on every
         * state refresh. Raising the cap above the accrued time brings
         * the entry back to life. */
        if (!e->spent) {
            char who[EV_LOG_NAME];
            e->spent = 1;
            LOGI("pool: %s budget spent (%ldms lit), dark until its end edge",
                 ev_name(e, who), lit_ms(e));
        }
        e->st = EV_WAIT;
        return 0;
    }
    /* only reachable with the budget live again: a raised cap revives a
     * spent entry, and the next spending has to log itself again */
    e->spent = 0;
    if (e->st == EV_SHOW) return 1;  /* a live show is never re-gated */

    if (e->k->flags & EV_SCREEN_BYPASS) return 1;

    /* the lit screen settles it in the basin: only SCREEN 0 lets it out */
    if (screen_on()) {
        if (e->st != EV_PARK) {
            char who[EV_LOG_NAME];
            e->st = EV_PARK;
            LOGI("pool: %s parked (screen on)", ev_name(e, who));
        }
        return 0;
    }
    return 1;
}

/* the SCREEN 0 edge: the age of every parked entry against the shared
 * window decides pool or drop, in slot order and one verdict per entry. */
static void basin_sort(void)
{
    int parked = 0;
    for (int i = 0; i < EV_MAX; i++)
        if (g_ev[i].k && g_ev[i].st == EV_PARK) { parked++; break; }
    if (!parked) return;         /* an empty basin has no verdict */

    long window;
    if (!basin_window_s(&window)) {
        int held = 0;
        for (int i = 0; i < EV_MAX; i++) {
            struct evt *p = &g_ev[i];
            if (!p->k || p->st != EV_PARK) continue;
            ev_drop(p, "no notify_screen_delay_ms");
            held++;
        }
        LOGI("pool: basin drained, %d entries dropped "
             "(no notify_screen_delay_ms)", held);
        return;
    }

    long now  = (long)time(NULL);
    int kept = 0, gone = 0;
    for (int i = 0; i < EV_MAX; i++) {
        struct evt *p = &g_ev[i];
        if (!p->k || p->st != EV_PARK) continue;
        long age = now - (long)p->created;
        if (age > window) {
            char why[64];
            snprintf(why, sizeof(why), "basin age %lds > window %lds",
                     age, window);
            ev_drop(p, why);
            gone++;
            continue;
        }
        p->st = EV_WAIT;
        kept++;
    }
    LOGI("pool: basin drained, %d released, %d dropped (window %lds)",
         kept, gone, window);
}

/* ---------------- selection ---------------- */

/* the best entry that may show: rank, then def_rank, then push order.
 * Entries already tried in this round are skipped, so one round cannot
 * loop over a candidate that turns out to have nothing to paint.
 *
 * A test outranks everything with INT_MAX/INT_MAX, which also settles two
 * tests against each other: both ranks equal means the tie-break falls
 * through to the push order, so the button the user just pressed wins and
 * the previous test stays in the pool waiting for Disarm. */
static struct evt *pool_pick(unsigned gen)
{
    struct evt *best = NULL;
    int br = 0, bd = 0;

    for (int i = 0; i < EV_MAX; i++) {
        struct evt *e = &g_ev[i];
        if (!e->k || e->tried == g_round) continue;
        int r = e->test ? INT_MAX : kind_rank(e->k, gen);
        if (r < 0) continue;                    /* unranked kind */
        int d = e->test ? INT_MAX : e->k->def_rank;
        if (!entry_showable(e)) {
            e->tried = g_round;                  /* dropped or parked */
            continue;
        }
        if (!best || r > br || (r == br && (d > bd ||
            (d == bd && e->seq > best->seq)))) {
            best = e;
            br = r;
            bd = d;
        }
    }
    return best;
}

/* No candidate: the LEDs are dark and the status file says so. Every
 * shipped effect is in the pool and the charge band paints its "none" as
 * off, so in practice the pool always has an owner - this is the defined
 * meaning of "nothing to show", kept so a config that leaves every kind
 * unranked still ends in dark LEDs instead of stale ones. */
static void pool_idle(void)
{
    leds_all_off();
    status_write("idle", "none", "", 0, 0, 0, "off");
}

/* Put one entry on the chip and in the status file. The mod only names the
 * section and the colours; the pool owns the write, so no entry can paint
 * without being elected. 0 = nothing was painted (no colour in led.conf, a
 * broken preset - already logged by the mod or led.c): the entry cannot be
 * shown and the caller tries the next candidate. Repeating this for the
 * entry that is already showing is cheap and correct - led.c's fingerprint
 * makes an unchanged payload a no-op, and a repost with a changed one (a
 * new charge band, a flipped call direction) repaints. */
static int paint_entry(struct evt *e)
{
    struct evt_paint p;
    memset(&p, 0, sizeof(p));
    PAINT_LABEL(&p, e->pkg);
    const char *engine = NULL;
    if (e->k->paint && e->k->paint(e, &p))
        engine = led_event(p.sec[0] ? p.sec : NULL, p.r, p.g, p.b);
    if (!engine) return 0;
    status_write(e->k->name, p.band, p.label, p.r, p.g, p.b, engine);
    return 1;
}

/* Hand the LEDs to the best entry. ONE election per triggering event: an
 * inner step that only mutates the pool (a cancel, a cap, a park) comes
 * back through here instead of painting on its own, so a decision can
 * never nest. The loop is bounded by the pool size because every round
 * marks what it tried. */
static void pool_select(const char *why)
{
    unsigned gen = conf_generation();
    g_round++;

    for (int guard = 0; guard < EV_MAX; guard++) {
        struct evt *want = pool_pick(gen);
        char from[EV_LOG_NAME], to[EV_LOG_NAME];
        if (!want) {
            if (g_show) {
                LOGI("pool: %s -> idle (%s, lit=%ldms)",
                     ev_name(g_show, from), why, g_show->shown_ms);
                bank_show(g_show);
                g_show = NULL;
                led_invalidate();
            }
            pool_idle();
            break;
        }
        want->tried = g_round;
        if (want == g_show) {         /* same owner: nothing changes hands */
            paint_entry(want);
            break;
        }

        struct evt *inc = g_show;
        if (inc) {
            LOGI("pool: %s -> %s (%s)", ev_name(inc, from),
                 ev_name(want, to), why);
            bank_show(inc);              /* the run ends here */
        } else {
            LOGI("pool: idle -> %s (%s)", ev_name(want, to), why);
        }
        g_show = want;
        want->st = EV_SHOW;
        want->opened = time(NULL);
        led_invalidate();                /* new owner: paint from scratch */

        /* nothing to paint: the LEDs keep whatever they showed and the next
         * candidate gets its turn */
        if (!paint_entry(want)) {
            want->st = EV_WAIT;
            g_show = NULL;
            continue;
        }
        break;
    }
    retune_timer();
}

/* ---------------- registry ---------------- */

/* The registered kind with this [priority] name, NULL when no mod
 * registered one. A kind enters the registry by NAME (chgd.h's
 * REGISTER_EVT), so this lookup is how the core names an effect: the test
 * buttons need the kind they show and must not depend on each mod's
 * internal identifier. */
const struct evt_kind *pool_kind(const char *name)
{
    if (!name) return NULL;
    for (const struct evt_registry *r = __start_chgd_events;
         r < __stop_chgd_events; r++)
        if (!strcmp(r->k->name, name))
            return r->k;
    return NULL;
}

/* ---------------- pool mutations (public) ---------------- */

/* Create the entries that exist before the bridge says anything. A
 * persistent kind is in the pool from the first line: its payload is the
 * current state and "none" paints the LEDs off, so the pool always has an
 * owner. */
void pool_boot(void)
{
    conf_maybe_reload();
    for (const struct evt_registry *r = __start_chgd_events;
         r < __stop_chgd_events; r++) {
        const struct evt_kind *k = r->k;
        if (!(k->flags & EV_PERSISTENT)) continue;
        struct evt *e = ev_slot();
        if (!e) return;
        ev_spawn(e, k, NULL, -1);
        e->seq = ++g_seq;
        LOGI("pool: %s entered the pool (persistent)", k->name);
    }
    pool_select("boot");
}

/* A bridge event. The identity is (kind, pkg, id) for a multi-entry kind
 * and the kind alone for a singleton, so a repost updates the entry that
 * is already there: the payload is refreshed, the run is NOT restarted
 * and the LEDs keep the pattern phase they had.
 *
 * The line belongs to whoever handled the event last: a repost that changes
 * the payload is one line naming the entry and what it swapped, and a kind
 * that declares EV_MOD_LOGGED is narrated by its own mod, so the arrival
 * stays silent here. */
void pool_push(const char *kind, const char *pkg, int id, const char *arg)
{
    char who[EV_LOG_NAME];
    const struct evt_kind *k = pool_kind(kind);
    if (!k) {
        LOGI("warn: no mod registered kind \"%s\" - the event is ignored",
             kind ? kind : "(null)");
        return;
    }
    conf_maybe_reload();

    /* the mod's filter runs before the entry exists: a suppressed package
     * must not occupy a slot, and must not log once per heartbeat either */
    if (k->accept) {
        struct evt probe;
        ev_spawn(&probe, k, pkg, id);
        if (!k->accept(&probe)) return;
    }

    const char *a = arg ? arg : "";
    int narrate = !(k->flags & EV_MOD_LOGGED);
    struct evt *e = ev_find(k, pkg, id);
    if (e) {
        if (e->test) {
            /* A test hold is the button's, not the event's: the entry keeps
             * its payload, its bypasses and its banked time until Disarm,
             * and only its push order is refreshed (so a newer test of
             * another kind still wins). The real state is not lost - the
             * mod keeps tracking it and it lands in the status/state file
             * as always; it is simply not what the LEDs show while a button
             * owns them. Without this the charge preview would be cut short
             * by the next battery broadcast (one every ~30s). */
            if (narrate)
                LOGI("pool: %s refreshed, the test hold keeps showing \"%s\"",
                     ev_name(e, who), e->arg);
            e->seq = ++g_seq;
            pool_select("event");
            return;
        }
        /* a repost keeps the basin: only the SCREEN 0 edge may empty it. A
         * repost that brings the same payload decided nothing, so it is not
         * an event and stays out of the log - the emitters that repeat
         * themselves (a sticky battery snapshot, a dialer re-posting its
         * call) are quiet for as long as nothing changes. */
        if (narrate && strcmp(e->arg, a))
            LOGI("pool: %s refreshed: \"%s\" -> \"%s\"",
                 ev_name(e, who), e->arg, a);
    } else {
        e = ev_slot();
        if (!e) return;
        ev_spawn(e, k, pkg, id);
        if (narrate)
            LOGI("pool: %s pushed", ev_name(e, who));
    }
    snprintf(e->arg, sizeof(e->arg), "%s", a);
    e->seq = ++g_seq;
    pool_select("event");
}

/* The matching end edge: RING_OFF, VOIP_OFF, MISSED_OFF, ALARM_OFF, CAN. */
void pool_end(const char *kind, const char *pkg, int id)
{
    const struct evt_kind *k = pool_kind(kind);
    if (!k) {
        LOGI("warn: no mod registered kind \"%s\" - the end is ignored",
             kind ? kind : "(null)");
        return;
    }
    struct evt *e = ev_find(k, pkg, id);
    /* Nothing live: the end arrives for an entry that never existed - the
     * mod refused the push (a suppressed package) or the bridge's edges
     * raced. That is the absence of a decision, not one, and the line the
     * refusal already wrote is the whole story. */
    if (!e) return;
    ev_drop(e, "end edge");
    pool_select("end edge");
}

/* CAN_ALL: every entry of one package, whatever kind it is. */
void pool_end_pkg(const char *pkg)
{
    if (!pkg || !pkg[0]) return;
    int had = 0;
    for (int i = 0; i < EV_MAX; i++) {
        if (g_ev[i].k && !strcmp(g_ev[i].pkg, pkg)) {
            ev_drop(&g_ev[i], "cancel all");
            had = 1;
        }
    }
    if (!had) return;       /* nothing to cancel: no decision, no line */
    pool_select("cancel all");
}

/* PULSE 0: stock SystemUI semantics - the notification LEDs go dark right
 * away and nothing is left behind to flash later. Only the kinds that
 * declare the gate are touched; a call, an alarm or the charge band is not
 * a notification. */
void pool_pulse_off(void)
{
    int had = 0;
    for (int i = 0; i < EV_MAX; i++) {
        if (g_ev[i].k && (g_ev[i].k->flags & EV_PULSE_GATED)) {
            ev_drop(&g_ev[i], "notification light off");
            had = 1;
        }
    }
    if (!had) { retune_timer(); return; }
    pool_select("notification light off");
}

/* A SCREEN edge landed: the falling edge empties the basin (basin_sort),
 * the rising one fills it with whatever the lit screen blocks. No polling,
 * no per-entry timer. */
void pool_screen(void)
{
    if (!screen_on()) basin_sort();
    pool_select("screen edge");
}

/* Disarm: the user asked for darkness. The test entries go, the
 * notifications go, and whatever was on the LEDs goes with them. Every
 * other kind keeps waiting and takes the LEDs straight back. A persistent
 * kind is neither dropped nor hidden: the charge band is a state, not a
 * notification, so a Disarm ends a test hold of it and leaves the real
 * payload to paint (the mod restores it, the pool cannot know what the test
 * overwrote). */
void pool_disarm(void)
{
    for (int i = 0; i < EV_MAX; i++) {
        struct evt *e = &g_ev[i];
        if (!e->k) continue;
        if (e->k->flags & EV_PERSISTENT) {
            if (e->test) {
                e->test = 0;
                led_invalidate();
            }
            continue;
        }
        if (e->test || !strcmp(e->k->name, "notify") || e == g_show) {
            ev_drop(e, "disarm");
        }
    }
    pool_select("disarm");
}

/* led.conf was reloaded: the cached ranks and budgets are stale (the pool
 * compares the config generation itself) and the visible state has to be
 * repainted, so a colour, mode or threshold edit takes effect without
 * waiting for the next event. */
void pool_config_changed(void)
{
    led_invalidate();
    pool_select("config");
}

/* A GUI test button. The entry is shown by force: no rank check (a test
 * owns no [priority] key of its own and must survive a config edit), no
 * screen guard, no budget - it is held until Disarm. One line per press,
 * naming the effect and the payload the button just put on the LEDs. */
void pool_test(const char *kind, const char *pkg, int id, const char *arg)
{
    char who[EV_LOG_NAME];
    const struct evt_kind *k = pool_kind(kind);
    if (!k) {
        LOGI("warn: no mod registered kind \"%s\" - the test is ignored",
             kind ? kind : "(null)");
        return;
    }
    struct evt *e = ev_find(k, pkg, id);
    if (!e) {
        e = ev_slot();
        if (!e) return;
        ev_spawn(e, k, pkg, id);
    }
    snprintf(e->arg, sizeof(e->arg), "%s", arg ? arg : "");
    e->seq = ++g_seq;
    e->test = 1;
    /* the budget is not consulted for a test, but the entry that the real
     * event replaces must not inherit the test's: entry_cap() re-reads it
     * once test is cleared (pool_push), the accrued lit time stays */
    if (e->st != EV_SHOW) e->st = EV_WAIT;   /* a test is never parked */
    if (e->arg[0])
        LOGI("pool: test %s [%s] held until Disarm", ev_name(e, who), e->arg);
    else
        LOGI("pool: test %s held until Disarm", ev_name(e, who));
    pool_select("test");
}

/* ---------------- the shown entry's budget ---------------- */

/* which deadline pool_deadline_ms() is reporting, for the log */
static const char *g_dl_why = "";

/* ms left of the shown entry's ACCRUED lit time: a displacement does not
 * eat it. 0 = nothing time-bound, >0 ms left, <0 fire now. What the
 * adaptive timer sleeps on - the pool is the only thing it asks. The basin
 * contributes nothing here: it is emptied by the SCREEN 0 edge. */
long pool_deadline_ms(void)
{
    long best = 0;
    g_dl_why = "";

    struct evt *e = g_show;
    if (e && !e->test) {
        long cap = entry_cap(e);
        if (cap > 0) {
            long rem = cap - lit_ms(e);
            best = rem > 0 ? rem : -1L;
            g_dl_why = rem > 0 ? "budget" : "budget spent";
        }
    }
    return best;
}

const char *pool_deadline_why(void)
{
    return g_dl_why;
}

/* The timer fired: the only time-driven end in the whole daemon. The pool
 * re-reads itself here - the shown entry whose lit time is spent is dropped
 * by entry_showable() in this scan, and the scan then hands the LEDs to
 * whatever is left. */
void pool_on_deadline(void)
{
    pool_select("deadline");
}

const char *pool_owner_name(void)
{
    return g_show ? g_show->k->name : "";
}