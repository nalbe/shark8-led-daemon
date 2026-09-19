led_hal_root - notification + charge LED daemon
================================================

v2.17: event-driven screen detection - the last 1s poll is gone.

The kernel fires no uevent when the backlight changes, so the LED NLS
app now forwards ACTION_SCREEN_OFF/ON as "SCREEN <0|1>" over the socket.
A notification parked while the screen is on therefore flashes the
instant the screen falls, with no per-second wakeup; the 1s sysfs poll
survives only as a fallback while the bridge is down.

The NLS supervisor's watchdog_ms is pushed over the same socket as
"WD <ms>" on connect and after every GUI save (GUI -> SIGALRM -> daemon
reload -> push). The app reads no config file: its starting cadence comes
once from the daemon's mirror /data/local/tmp/lednls.status.

v2.16: notification priority pool (mods/queue.c).

Every notification ENQ now lands in an in-memory pool (up to 32
entries); a single arbitrator decides who lights the LED, when, and
for how long. No per-entry timers.

  - Selection is LIFO by recency: the freshest post wins. A newer top
    preempts the current LED owner, which returns to the pool with its
    shown time accrued and resumes from the leftover on its next turn -
    alternating app traffic can never blink forever.
  - Lifetime is fixed at created + [notify(.app)] notif_max_sec. A post
    that aged out is dropped at pick time; the same cap bounds the
    accumulated show time.
  - Screen-on staging is timerless: a fresh top that lands on a lit
    screen parks as Q_HOLD; the lazy 1s heartbeat flashes it on screen-
    off or drops it (only the parked top, surviving entries keep their
    own clocks) once the grace window notify_screen_delay_ms runs out.
  - Emergency planes stay untouchable: while ring / voip / missed /
    alarm own the channel the pool quietly waits, ages its entries, and
    drops the stale ones lazily - the backlog can never light the LED
    mid-call or override an alarm.
  - The heartbeat is adaptive, not a steady 1s tick: a live LED with no
    cap (notif_max_sec=0) costs one WATCHDOG_SEC safety pass, an LED
    with a cap wakes exactly at the cap, and the ONLY 1s polling left is
    a top parked on a lit screen (bounded by the grace window, since
    screen-off has no uevent on this device).
  - The old core-side active-IDs bookkeeping is gone: the pool IS the
    ledger. Cancels remove (pkg,id), the LED disarms only when the last
    entry of the armed package leaves; a CAN+post rebuild continues the
    very same show on the surviving entry with the cap credit carried.
  - PULSE 0 / SIGUSR2 (Blink light off) also clears the pool.

v2.14: standalone NLS bridge, daemon fully independent of the GUI.

The notification bridge (NLS) is now a separate headless APK
(com.bastet.lednls) installed alongside the daemon by customize.sh.
The notification LED works without the GUI; the GUI is an optional
configurator on top.

  - NLS app (com.bastet.lednls): headless NotificationListenerService,
    no activity, no Compose - just the socket bridge + watchdog supervisor.
    Ships as nls.apk in the module zip, always installed by customize.sh.
  - led_gui (com.bastet.ledgui): optional configurator with the daemon
    status screen, config editor, live brightness read. Ships as
    led_gui.apk in the module zip, installed alongside the NLS.

Why the split: NLS is backend (n->n), not UI. The old design tied the
notification bridge to the GUI APK, so no GUI meant no notification LED.
Now a bare daemon + NLS headless gives the full notification stack.
The watchdog supervisor also lives in the NLS app; the daemon (the only
legitimate reader of its own config - /data/adb is 700 root, so a
non-root app cannot even inotify it) pushes "WD <ms>" over the socket
on every connect and after every config change, so GUI edits are applied
in real time with zero config access from the app. The NLS only reads
the daemon's world-readable mirror /data/local/tmp/lednls.status once
for its starting cadence - no IPC needed, no polling.

Critical limitation lifted: Android only logs notification_enqueue &
friends into the events buffer on debuggable builds. Running with
ro.debuggable=0 (hiding root from apps) means an event-log transport
sees no notifications at all, so the LED would go dead. The standalone
NLS app (com.bastet.lednls) runs a NotificationListenerService that
feeds chgd directly and is now the ONLY notification transport - the
old /dev/socket/logdr (logcat events) stream is gone entirely. The
bridge works on ANY build type:

  - Transport: abstract Unix domain socket "chgd_noty" (SOCK_STREAM).
  - Protocol, one command per line:
        ENQ <pkg> <id>     onNotificationPosted
        CAN <pkg> <id>     onNotificationRemoved
        CAN_ALL <pkg>      every notification of the package gone
        VOIP_ON <pkg>      messenger call started (rainbow on)
        VOIP_OFF <pkg>     messenger call over (rainbow off)
        RING_ON  <0|1>     SIM call started, 1 incoming / 0 outgoing
        RING_OFF           last SIM call notification gone
        PULSE <0|1>        Settings notification_light_pulse changed
        PING               liveness probe, daemon answers PONG
  - On connect the NLS replays its live state (RING/VOIP + every active
    ENQ), so a daemon restart mid-call re-arms cleanly.
  - Ledger and arbitration live in the daemon: every ENQ enters the
    notification priority pool, cancels remove from it, the LED is
    disarmed only when the last live entry of the armed package leaves.

Supervision moved into the NLS app too. keepalive.sh is gone. The NLS
service polls whether chgd is alive and restarts it via su
("setsid /data/adb/modules/led_hal_root/chgd &"). The cadence is
[led] watchdog_ms in led.conf (default 60000 ms, 0 = disabled - the
poll is pure crash insurance and every tick spawns an su shell, so it
is opt-in and tunable in the GUI's Daemon card too; or edit the file
directly - the daemon watches its config with inotify and pushes the
new cadence live, no signal of any kind needed).
The system rebinds a notification listener on its own, so a supervisor
living in the app outlives any shell keepalive that nobody ever
restarted. service.sh keeps its boot-time autostart so the daemon is
up before the NLS ever runs.

Requirements: the NLS app needs (a) notification access (Settings ->
Special app access -> Notification access -> LED NLS) and (b) root
granted to it in KernelSU Manager (needed for the watchdog's su). The
daemon still runs as its own root process.

v2.11: ID-based notification tracking + pseudo-package cancel bridge.

Android apps (Messages/RCS, Telegram) rebuild notifications via
cancel+post round-trips. The old cancel_grace timer (2s) was too
short — a cancel after the grace expired killed the LED even though
the notification was still live. chgd now tracks per-(package,
notification_ID) pairs: id_add on enqueue, id_remove on cancel. The
LED disarms only when the LAST active ID for the armed package is
removed. cancel_all clears all IDs for the package. No timers, no
grace windows, no polling — purely event-driven.

Pseudo-package bridge: mods like dialer.c arm the LED under a
pseudo-package ("missed.call") but the real cancel comes from the
actual app ("com.google.android.dialer"). notif_state now carries
owner_pkg — the real package that owns the LED. cancel_dispatch
matches against owner_pkg so a cancel from the real app correctly
disarms the pseudo-package. Real-package notifications set owner_pkg
to empty, preserving the direct cur_pkg == pkg match.

ARCHITECTURE (v2.10, MODULAR)
------------------------------
One native daemon "chgd" does everything. No polling scripts.
Sources are split into a CORE (written once) and MODS (extension
files). Adding a feature never touches the core:

  core (always compiled):
    core.c      main loop: select() over netlink uevents, NLS socket
                ("chgd_noty"), adaptive timerfd (retune_timer), pkg /
                uevent / refresh dispatch, signal test hooks
    led.c       LED adapter: solid/breathing/wave calls straight into
                the AW2033 chip controller (libaw2033.a, standalone
                aw2033-driver repo). The ONLY writer of the RGB
                channels.
    config.c    INI runtime config (led.conf) + generic key-value
                store for mod-owned sections
    util.c      logging, sysfs read/write helpers, live status
                file, screen detection

  mods/ (extension files, pick and choose):
    charge.c    charge band eval, state file, LED application.
                Purely event-driven: REGISTER_UEVENT "power_supply" +
                REGISTER_REFRESH (boot/SIGALRM); its only MODE owns the
                SIGQUIT charge test ("charge.test")
    queue.c     notification priority pool: LIFO pick, screen-on Q_HOLD
                staging, preemption with resume-credit, lazy grace/
                expiry, accumulated-cap accounting
    notify.c    notification gate: suppress list -> color rules ->
                push into the pool / arm the winner (the "*" default
                handler); owns the idle channel "" while the pool has
                work, adaptive wake (1s only for a screen-on park with
                no NLS SCREEN events, else the cap deadline)
    ring.c      incoming/outgoing call rainbow (a timer MODE); armed
                by NLS RING_ON, call end resolved event-driven by
                NLS RING_OFF (or [ring] max_sec cap) - no polling
    dialer.c    missed-call verification plugin (claims all dialer
                notifications via REGISTER_HANDLER)
    tele.c      one-shot child-process capture (call_log content
                query for missed-call verification)
    voip.c      messenger (VoIP) call rainbow: armed/disarmed straight
                by VOIP_ON/VOIP_OFF over the NLS bridge - no polling
    alarm.c     alarm-clock LED with its OWN [alarm] config (color,
                mode, cap; timing in its [alarm.breath/wave] chips) -
                independent of [notify];
                climbs over the screen-on guard (an alarm always
                wakes the screen)

Per-app colors and the suppress blacklist are runtime-only (led.conf),
no rebuild needed. The only remaining link-time rule is the internal
pseudo-package registered by dialer.c ("missed.call" = blue).

Registries are packed into linker sections: each REGISTER_* entry is
a static const struct in .chgd_rules/.chgd_handlers/.chgd_modes/
.chgd_uevents/.chgd_refresh, and the core iterates the section bounds.
New file in mods/ = new entries, no core edit. Any mod may register:
  REGISTER_HANDLER("pkg"|"*", fn)   claim notifications (exact match
                                    wins over the "*" default)
  REGISTER_MODE(name, ms, owns, tick)  own a cur_pkg state and get a
                                    timer heartbeat while it is armed
  REGISTER_UEVENT("substr", fn)     react to netlink kobject uevents
  REGISTER_REFRESH(fn)              refresh visible state at boot and
                                    on config change (SIGALRM poke or
                                    the daemon's own inotify watcher)
Mode ownership of g_st.cur_pkg is a strict partition: "" -> notify while
the pool has work (nobody owns it at true idle; the charge band repaints
on uevent/SIGALRM), "incoming.call" -> ring, "voip.call" -> voip,
"missed.call" -> missed, alarm clock pkgs -> alarm, any other package ->
notify.

MODULAR FLOW
------------
Add a claiming handler: create mods/myapp.c with one line
      REGISTER_HANDLER("com.example.app", my_handler_fn);
or a timer mode:
      REGISTER_MODE("mymode", 100, my_owns, my_tick);
then run build.cmd and apply. User-facing colors/suppress/charge/notify
settings all live in led.conf and apply on the fly without a rebuild.
The whole daemon still compiles into one static binary.

BOOT STACK
----------
  service.sh      module entry, launched by KernelSU at boot; sleeps 8s,
                  then starts chgd. That is all - no keepalive script.
  NLS app         (com.bastet.lednls, headless) owns daemon supervision:
                  restarts chgd via su if it dies (configurable cadence,
                  [led] watchdog_ms, 0 = off; the daemon pushes those ms
                  live, see the WD command). The system rebinds the
                  listener on its own, so the supervisor always comes
                  back (grant notification access once).
  led_gui app     (com.bastet.ledgui, optional) configurator on top:
                  status screen, config editor, live preview.
  self-test       adb shell am startservice -n \
                  com.bastet.lednls/.LedNotificationListenerService \
                  -a com.bastet.lednls.POST_TEST
                  posts a test notification from the app itself and arms
                  the LED through the full ENQ path.

NLS BRIDGE (daemon side)
-----------------------
core.c:  NLS_SOCK_NAME "chgd_noty" abstract listener, accept() in
         select(); nls_cmd() parses ENQ / CAN / CAN_ALL / VOIP_ON /
         VOIP_OFF / RING_ON / RING_OFF / PULSE / PING. On client connect
         g_nls gates nothing anymore - the NLS is the ONLY transport.
         Public API: notif_enqueue(pkg,id), notif_cancel(pkg,id),
         notif_cancel_all(pkg) feed pkg_dispatch / queue_remove(_all);
         the priority pool is the ledger (no id_add/id_remove anymore).

LED NODES
---------
/sys/class/leds/{red,green,blue}/{brightness,blink,led_time}
Hardware breathing via blink=1 + led_time "RISE HOLD FALL OFFT" (ms,
quantized by aw2033 driver). IMPORTANT: stale led_time keeps pulsing -
every solid/all-off write must clear blink and led_time first.

COLORS
------
All colors are RGB triplets (0-255 per channel), runtime-configured in
led.conf: [rules] pkg=r,g,b, [notify] default_color=r,g,b, and the
per-band [charge.lower/middle/upper] color=r,g,b. There are NO builtin
per-app colors anymore - the config is the single source (mods/rules.c
was removed). Builtin literals remain only in config.c when a key is
absent.

Notifications (screen OFF only), colors from led.conf [rules] - the
shipped template has only one entry, nothing is builtin:
  org.telegram.messenger=0,255,255          cyan
  everything else                           [notify] default_color
The light FORMAT (renderer mode + per-chip params and timing
rise/hold/fall/offt in [notify.breath]/[notify.wave], notif_max_sec)
lives in the [notify] base and chip sections; only the color is
per-app via [rules] (per-app mode is not supported - rule apps run the
[notify.app] preset).
Missed calls (verified via root content query on call_log:
type=3 new=1, fresh <=120s, dedup by _id):
  com.google.android.dialer -> missed.call  blue          0,0,255
Incoming call (NLS classifies the dialer's live-call notification, sends
RING_ON 1):
  traveling-wave rainbow on the AW2033 chip (led_wave_rgb: per-channel
  phase offset staggers R/G/B so the color glides R->G->B->R), until the
  ring ends; RING_OFF resolves the outcome event-driven: incoming ->
  reopens the missed-call verification window, then drops to
  blue breath / charge leds. No telephony polling, no focus tracker.
Outgoing call (NLS sends RING_ON 0):
  identical wave for the whole call; RING_OFF just drops back to the
  charge leds (no missed-call check for outgoing calls).
Messenger / VoIP call (Telegram/WhatsApp/Viber/Signal/Snapchat/Duo):
  the NLS app classifies call notifications (category CALL, a channel
  id containing "call" - Telegram's incoming_calls40 - or Answer /
  Decline / Reject / End-call actions on a messenger package) and sends
  VOIP_ON/VOIP_OFF over the socket; the LED runs the same wave while
  the call is up, the ordinary [rules] color otherwise. Chat messages
  keep their [rules] color. VOIP_OFF is only sent once no call
  notification is left, so a ring -> active id change cannot flicker.
  Config [voip]: max_sec (safety cap for a lost VOIP_OFF, default 300),
  packages (comma list overrides the messenger set).
The system "Blink light" toggle (Settings.System notification_light_pulse)
  gates ALL [notify] handling in one place; call waves, alarms and the
  charge band are not affected. customize.sh forces it ON on install.
Suppressed (led.conf [suppress] only, plus nothing else - the builtin
list in mods/suppress.c was removed): com.android.systemui, android,
com.android.shell, org.amnezia.vpn, and the quiet background emitters.
The LED adapter lives in led.c, call handling in mods/dialer.c, ring
policy in mods/ring.c. Test-wave hold and other mod settings go in
their own led.conf sections, e.g. [ring] test_sec=30
(any unknown key is readable by the mod via conf_get_int).

Charge bands. Ranges are named abstractly (lower/middle/upper) and the
behavior per range - renderer mode and color - is configured in the
per-band [charge.lower/middle/upper] sections (mode + color), with the
chip params/timing per band in [charge.<band>.solid/breath/wave]:
  Full                                      upper: static green (0,255,0)
  Charging, below first (90)                lower: breathing red (255,0,0)
  Charging, first..second (90-94)           middle: breathing lime (96,255,0)
  Charging, >= second (95)                  upper: static green
  Not charging, >= 95                       upper: static green
Thresholds (first_threshold/second_threshold, order-free - swapped
automatically) in led.conf [charge], builtin literals in config.c.
Edits re-apply on the next event: the applied cache is a fingerprint
of band+color+mode.

STATE FILES (/data/local/tmp/)
-----------------------------
ledd.log             daemon log
chgd.err             daemon stderr
led_chg              "<band> <epoch>" current charge band
led_status           live LED-owner state for GUI: ts / mode (charge |
                     notify | ring | voip | missed | alarm) / band /
                     pkg / color=r,g,b / engine
lednls.status        NLS bridge state, written by the daemon itself on
                     client accept/EOF: connected=1|0 plus the live
                     watchdog_ms= cadence the NLS reads once at startup
led_chgd.lock        flock lock
(led_keepalive.pid was dropped in v2.12 with keepalive.sh)

REBUILD
-------
build.cmd compiles core + every .c under mods\ into chgd.
Custom NDK_CC env var overrides the compiler path.

APPLY MANUALLY
--------------
adb push chgd $MOD/ && adb shell chmod 755 $MOD/chgd
then restart: kill -9 $(pidof chgd); setsid sh $MOD/service.sh
or just run install_core.cmd from the package root (rebuilds + pushes
everything + restarts). Both APKs ship inside the module zip:
  - nls.apk       notifications bridge + watchdog (HEADLESS, required
                  for notification LED on any build type)
  - led_gui.apk   optional configurator
customize.sh installs/refreshes both on every module install.
install_nls.cmd rebuilds nls.apk into the staging dir; install_gui.cmd
does the same for the GUI (or installs it directly to a device).

TESTING
-------
Screen off, then: kill -USR1 $(pidof chgd)   -> Telegram color breathing
                 kill -HUP  $(pidof chgd)   -> blue breathing if call_log
                                                has a fresh missed row
                 kill -WINCH $(pidof chgd)  -> rainbow (ring, held in test)
                 kill -QUIT  $(pidof chgd)  -> charge band cycle (lower/
                                                middle/upper/none)
Disarm (USR2)                        -> back to the charge leds
NLS bridge end-to-end: am startservice -n \
    com.bastet.lednls/.LedNotificationListenerService \
    -a com.bastet.lednls.POST_TEST    -> notification posted from the app
                                         itself; watch "notify armed:" in
                                         ledd.log -> LED breathes; snooze it
                                         (cmd notification snooze --for 60000
                                         <key>) -> CAN -> disarm
SIM call: straight from the NLS bridge - the dialer's live-call
notification is classified in the app and sent as RING_ON; let it ring
then reject -> RING_OFF + missed-call verification (ledd.log shows
"ring ended (RING_OFF) -> missed check" then a missed arm).

LEGACY
------
The pre-modular monoliths are archived in legacy/:
  chgd_monolithic_v2.6.c   the former single-file chgd.c
  chgd_dbg.c               the older debug-variant daemon
Rebuild them by hand if you ever need a fallback:
  aarch64-linux-android29-clang.cmd -O2 -s -o chgd legacy/chgd_monolithic_v2.6.c

KNOWN QUIRKS OF THIS DEVICE/PORT
-------------------------------
- the NLS socket is the single transport; no event-log/logcat dependency
- keyevent 223 dozes, 224 wakes (inverted vs docs on this build)
- reading /sys/class/leds/*/blink as root shell gives EACCES but writes
  succeed from the daemon context