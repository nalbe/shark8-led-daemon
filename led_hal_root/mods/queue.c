/*
 * mods/queue.c - notification priority pool (the event ledger).
 *
 * Every notification ENQ lands here unconditionally (mods/notify.c
 * filters suppress first) and lives until it is picked for the LED,
 * cancelled, or expires. The arms/disarms still walk the notification
 * channel (g_st.cur_pkg / arm_notification); this module only decides
 * WHO the channel shows, WHEN, and for HOW LONG.
 *
 * Selection is LIFO by recency - the newest post wins, an older one is
 * preempted with resume-credit (shown_ms) so it later finishes what it
 * started. No user-configurable priorities, no per-entry timers:
 *
 *   lifetime  = created + [notify(.app)] notif_max_sec, immutable
 *               (0 = unlimited: lives until cancelled).
 *   screen    = a fresh top that lands while the screen is on is parked
 *               in Q_HOLD with NO timer - the heartbeat polls for
 *               screen-off (there is no such uevent on this device),
 *               but the park is bounded by the grace check
 *               (notify_screen_delay_ms), which drops it even while the
 *               screen stays on.
 *   preempt   = a newer top behind the LED returns the current one to
 *               the pool with its shown time accrued; the cap (notif_
 *               max_sec) is enforced on the accumulated show time, so
 *               alternating priorities can't blink forever.
 *
 * Cancel semantics mirror Android's cancel+post round-trips: a specific
 * CAN drops one (pkg,id); the LED is disarmed only when no entries for
 * the armed package remain (real dismissal), otherwise the rebuild keeps
 * it armed. queue_remove/remove_all end with an arbitrate so the next
 * eligible entry takes over immediately.
 *
 * Emergency planes (ring/voip/alarm/missed call) own the channel
 * separately; while one is active the pool just accumulates - the times
 * in `created` keep ticking, and the lazy expire check behaves. The pool
 * never touches the LED while an emergency plane is up.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../chgd.h"

#define Q_MAX 32

enum qev_state {
    Q_WAIT,     /* in the pool, waiting to be picked                */
    Q_HOLD,     /* picked top, screen is itself the blocker         */
    Q_ACTIVE    /* currently showing on the LED                     */
};

struct qev {
    char           pkg[96];
    int            id;
    enum qev_state st;
    time_t         created;    /* immutable: grace + expiry clock  */
    time_t         opened;     /* when the current show run started*/
    long           shown_ms;   /* accrued show time (preempt credit)*/
    long           cap_ms;     /* total allowed show time, 0 = n/a */
};

static struct qev g_q[Q_MAX];

/* ---------------- helpers ---------------- */

static struct qev *q_find(const char *pkg, int id)
{
    for (int i = 0; i < Q_MAX; i++)
        if (g_q[i].pkg[0] && !strcmp(g_q[i].pkg, pkg) &&
            g_q[i].id == id)
            return &g_q[i];
    return NULL;
}

static struct qev *q_find_pkg_q(const char *pkg)   /* any id */
{
    for (int i = 0; i < Q_MAX; i++)
        if (g_q[i].pkg[0] && !strcmp(g_q[i].pkg, pkg))
            return &g_q[i];
    return NULL;
}

static struct qev *q_active_ev(void)
{
    for (int i = 0; i < Q_MAX; i++)
        if (g_q[i].pkg[0] && g_q[i].st == Q_ACTIVE)
            return &g_q[i];
    return NULL;
}

/* a free slot, else the oldest non-active entry (LIFO eviction: on a
 * full pool the oldest waiter loses its place to the fresh post) */
static struct qev *q_slot(void)
{
    struct qev *old = NULL;
    for (int i = 0; i < Q_MAX; i++) {
        if (!g_q[i].pkg[0]) return &g_q[i];
        if (g_q[i].st != Q_ACTIVE &&
            (!old || g_q[i].created < old->created))
            old = &g_q[i];
    }
    return old;
}

static void q_free(struct qev *e)
{
    e->pkg[0] = '\0';
    e->id = -1;
    e->st = Q_WAIT;
    e->shown_ms = 0;
    e->cap_ms = 0;
}

/* ---------------- config ---------------- */

static long q_expiry_s(const char *pkg)
{
    int r, g, b;
    return conf_pkg_rgb(pkg, &r, &g, &b)
        ? conf_get_int("notify.app", "notif_max_sec", conf_notif_max_sec())
        : conf_notif_max_sec();
}

static long q_grace_s(const char *pkg)
{
    int r, g, b;
    long ms = conf_pkg_rgb(pkg, &r, &g, &b)
        ? conf_get_int("notify.app", "notify_screen_delay_ms", 60000)
        : conf_get_int("notify", "notify_screen_delay_ms", 60000);
    if (ms < 0) ms = 0;
    return ms / 1000L;
}

/* ---------------- public queries ---------------- */

int queue_has(const char *pkg)
{
    return q_find_pkg_q(pkg) != NULL;
}

int queue_has_pending(void)
{
    for (int i = 0; i < Q_MAX; i++)
        if (g_q[i].pkg[0] && g_q[i].st != Q_ACTIVE)
            return 1;
    return 0;
}

/* a parked top (screen-on staging). This is the ONLY case that still
 * needs the 1s heartbeat: screen-off has no uevent here, so the fall of
 * the screen is caught by polling - bounded by the grace window, after
 * which the park is dropped. */
int queue_has_hold(void)
{
    for (int i = 0; i < Q_MAX; i++)
        if (g_q[i].pkg[0] && g_q[i].st == Q_HOLD)
            return 1;
    return 0;
}

int queue_active(void)
{
    return q_active_ev() != NULL;
}

long queue_active_remain_ms(void)
{
    struct qev *a = q_active_ev();
    if (!a || a->cap_ms <= 0) return -1;
    long used = a->shown_ms + (long)((time(NULL) - a->opened) * 1000);
    long rem = a->cap_ms - used;
    return rem > 0 ? rem : 1;
}

/* ---------------- arbitration ---------------- */

/* A canceled entry whose package is gone from the pool entirely is a
 * REAL dismissal: take the notification LED down. Entries still alive
 * mean the cancel was half of an app rebuild (cancel+post) - keep the
 * arm. Pseudo-packages (missed.call) reach this through owner_pkg. */
static void q_maybe_cancel_armed(const char *pkg)
{
    if (!g_st.cur_pkg[0]) return;
    const char *own = g_st.owner_pkg[0] ? g_st.owner_pkg : g_st.cur_pkg;
    if (strcmp(own, pkg)) return;
    if (q_find_pkg_q(pkg)) return;       /* IDs still live: rebuild */
    LOGI("queue: %s dismissed (channel freed)", pkg);
    disarm_notification(&g_st, "notification_cancel");
}

/* preempt: send the current owner back with its shown time accrued */
static void q_preempt_current(void)
{
    struct qev *a = q_active_ev();
    if (!a) return;
    a->shown_ms += (long)((time(NULL) - a->opened) * 1000);
    a->opened = 0;
    a->st = Q_WAIT;
    disarm_notification(&g_st, "preempt");
    LOGI("queue: preempt %s to pool (shown_ms=%ld)", a->pkg, a->shown_ms);
}

static void q_paint(struct qev *e)
{
    e->opened = time(NULL);
    if (e->cap_ms <= 0)
        e->cap_ms = q_expiry_s(e->pkg) * 1000L;
    arm_notification(&g_st, e->pkg);
    if (!g_st.cur_pkg[0]) {
        /* the "Notification light" toggle is off: the entry is pointless,
         * drop it instead of retrying (and logging) every heartbeat */
        LOGI("queue: %s dropped (notification light off)", e->pkg);
        q_free(e);
        return;
    }
    e->st = Q_ACTIVE;
    LOGI("queue: flash %s (cap=%lds)", e->pkg, e->cap_ms / 1000L);
}

/* The only policy entry. Every trigger funnels here:
 *   - pool mutation (push/remove/remove_all)
 *   - the notify heartbeat (screen-off poll on a parked top; grace,
 *     expiry and cap deadlines)
 * It is cheap: with nothing changing it early-outs. */
void queue_arbitrate(void)
{
retry:
    /* emergency planes own the channel: the pool just waits */
    if (ring_is_active() || voip_active() ||
        alarm_is_active() || missed_is_active())
        return;

    /* 1) a cap-expired active entry relinquishes the channel */
    struct qev *a = q_active_ev();
    if (a && a->cap_ms > 0) {
        long used = a->shown_ms + (long)((time(NULL) - a->opened) * 1000);
        if (used >= a->cap_ms) {
            LOGI("queue: %s cap reached (%ldms)", a->pkg, used);
            q_free(a);
            disarm_notification(&g_st, "queue cap");
        } else {
            a = NULL;
        }
    }

    /* 2) pick the freshest non-active entry */
    struct qev *e = NULL;
    for (int i = 0; i < Q_MAX; i++) {
        if (!g_q[i].pkg[0] || g_q[i].st == Q_ACTIVE)
            continue;
        if (a && !strcmp(g_q[i].pkg, a->pkg))
            continue;                /* same app already showing */
        if (!e || g_q[i].created > e->created)
            e = &g_q[i];
    }
    if (!e) return;

    /* 3) the same package posted again: refresh, keep the led */
    if (!g_st.cur_pkg[0]) {
        /* free channel */
    } else if (!strcmp(g_st.cur_pkg, e->pkg)) {
        struct qev *aa = q_active_ev();
        if (aa) aa->created = time(NULL);   /* extend the live show */
        LOGI("queue: %s reposted, stays armed", e->pkg);
        return;
    } else if (q_active_ev()) {
        q_preempt_current();
    } else {
        return;                      /* test / charge channel: held */
    }

    /* 4) lifetime first: a dead post never arms. notif_max_sec=0 means
     *    unlimited - the entry lives until it is cancelled. */
    long age = (long)(time(NULL) - e->created);
    long life = q_expiry_s(e->pkg);
    if (life > 0 && age >= life) {
        LOGI("queue: %s expired after %lds", e->pkg, age);
        q_free(e);
        goto retry;
    }

    /* 5) screen is up: the top cannot show yet. Park it. No per-entry
     *    timer; the heartbeat polls for screen-off (there is no such
     *    uevent on this device), but the park is bounded by the SAME
     *    grace even while the screen stays on - otherwise a pinned
     *    screen kept the 1s poll alive until the lifetime cap. */
    if (screen_on()) {
        if (e->st == Q_HOLD && age >= q_grace_s(e->pkg)) {
            LOGI("queue: %s grace expired while screen on (%lds), dropped",
                 e->pkg, age);
            q_free(e);
            goto retry;
        }
        if (e->st == Q_HOLD)
            return;
        e->st = Q_HOLD;
        LOGI("queue: %s parked (screen on, grace %lds)", e->pkg,
             q_grace_s(e->pkg));
        return;
    }

    /* 6) a held top that outlived its grace is dead on arrival */
    if (e->st == Q_HOLD && age >= q_grace_s(e->pkg)) {
        LOGI("queue: %s grace expired (%lds), dropped", e->pkg, age);
        q_free(e);
        goto retry;
    }

    /* 7) screen off: show it, resuming with the accrued credit */
    q_paint(e);
}

/* ---------------- pool mutations (public) ---------------- */

void queue_push(const char *pkg, int id)
{
    if (!pkg || !pkg[0]) return;
    struct qev *e = q_find(pkg, id);
    if (e) {
        e->created = time(NULL);     /* fresh lease on repost */
        e->st = Q_WAIT;
        LOGI("queue: repost %s id=%d", pkg, id);
    } else {
        e = q_slot();
        if (!e) return;
        snprintf(e->pkg, sizeof(e->pkg), "%s", pkg);
        e->id = id;
        e->st = Q_WAIT;
        e->created = time(NULL);
        e->opened = 0;
        e->shown_ms = 0;
        e->cap_ms = 0;
        LOGI("queue: push %s id=%d", pkg, id);
    }
    queue_arbitrate();
    retune_timer();     /* a park needs the 1s screen-off poll armed */
}

void queue_remove(const char *pkg, int id)
{
    struct qev *e = q_find(pkg, id);
    if (!e) return;
    int was_active = (e->st == Q_ACTIVE);
    time_t oc = e->opened;
    long sh = e->shown_ms;
    long cap = e->cap_ms;
    q_free(e);
    LOGI("queue: cancel %s id=%d", pkg, id);
    if (was_active) {
        /* surviving IDs of the same app mean a cancel+post rebuild:
         * the channel continues the very same show on the next entry
         * (cap/credit carry over, no restart) */
        struct qev *sib = q_find_pkg_q(pkg);
        if (sib) {
            sib->st = Q_ACTIVE;
            sib->opened = time(NULL);
            sib->shown_ms = sh + (long)((time(NULL) - oc) * 1000);
            if (cap > 0) sib->cap_ms = cap;
            LOGI("queue: %s rebuild continues via id=%d", pkg, sib->id);
        }
    }
    q_maybe_cancel_armed(pkg);
    queue_arbitrate();
    retune_timer();
}

void queue_remove_all(const char *pkg)
{
    int found = 0;
    for (int i = 0; i < Q_MAX; i++)
        if (g_q[i].pkg[0] && !strcmp(g_q[i].pkg, pkg)) {
            q_free(&g_q[i]);
            found = 1;
        }
    if (!found) return;
    LOGI("queue: cancel_all %s", pkg);
    q_maybe_cancel_armed(pkg);
    queue_arbitrate();
    retune_timer();
}

void queue_clear(void)
{
    int had = 0;
    for (int i = 0; i < Q_MAX; i++)
        if (g_q[i].pkg[0]) {
            q_free(&g_q[i]);
            had = 1;
        }
    if (had) {
        LOGI("queue: cleared (led toggle)");
        retune_timer();     /* pool empty: drop any screen-off poll */
    }
}