# led_hal_root patch notes

Changelog for **this** repo: the chgd daemon core (`led_hal_root/`) and the
LED GUI (`led_gui/`). History that belongs to a standalone project is kept
out of here:

- the headless notification bridge (`com.bastet.notifybridge`) moved to the
  **notify-bridge** repo (its changelog lives there)
- the AW2033 chip controller and `awctl` live in the **aw2033-driver** repo
  (this repo ships only the prebuilt `libaw2033.a`)

## Revision: missed-call LED is 100% bridge events; the last child process is gone (2026-09-23, v3.5)

1. **The last `content query` fork is deleted.** `mods/dialer.c` +
   `mods/tele.c` (missed-call verification: root `content query
   content://call_log/calls`, freshness filter, dedup, 4x2s verification
   window, `run_capture()` child-process capture) are gone - the REPLACEMENT
   is `mods/missed.c`, a pure event mode.
3. **Bridge now classifies missed tombstones itself (notify-bridge side).**
   The bridge already separated the dialer's `missed_calls` channel from
   live calls (`isSimCallNotification`); instead of passing the tombstone
   through as a plain `notify.posted` it now emits `missed.on` when the
   tombstone posts and `missed.off` when it leaves, tracks the live set
   (`missedNotifs`) and replays it on connect. The dialer package is fully
   classified (ring | missed) and NEVER forwarded as a raw ENQ, so the
   daemon's pool never sees dialer events and no package claim is needed.
4. **Daemon: `MISSED_ON` / `MISSED_OFF` NLS commands.** `core.c` parses
   them; `missed_on()` arms the `[missed]` renderer, `missed_off()` disarms
   when the mode owns the channel. A call end no longer reopens a
   verification window (`ring_resolve` lost `dialer_reopen_window()`) -
   the tombstone event IS the missed call. Deleted: `g_last_missed_id`,
   `g_call_checks_left`/`CALL_RECHECK_MS` (and the `retune_timer` branch),
   `g_next_call_check`, `maybe_call_check()`, the SIGHUP-to-dialer test
   path became SIGPWR-to-missed, `dialer_pkg_id()`.
5. **VoIP package list deleted (also a dead hardcode).** `VOIP_DEF_PKGS`,
   `voip_pkg_list()`, `is_messenger()` and `voip_try()` were dead: the
   bridge classifies call notifications by category/channel, not by
   package name, and `voip_try` had no callers. The pool already yields to
   an active ring/voip/missed/alarm (`queue_arbitrate` early-out), so no
   package list is needed to keep chats from recoloring the rainbow. The
   `[voip] packages=` config, the GUI's VoIP package editor and its
   led.conf serialization are removed too.
6. **notify_tick() wrapper deleted.** The notify mode's tick was a
   one-line shim around `queue_arbitrate()`; the pool policy is now
   registered directly as the mode tick, and the "armed tick" periodic log
   is gone.
7. **Live-tested on the Shark8:** SIGPWR arms the missed LED (blue breath
   from `[missed]`), SIGUSR2 disarms it; bridge reconnect replays SCREEN
   and the PULSE gate. `Settings.System.notification_light_pulse` was off
   on the device, so the notification pool correctly idles (the toggle
   still gates only the notification LED; ring/voip/missed/alarm planes
   are unaffected). Docs updated: README (wire format, architecture,
   module list), module.prop bumped to 3.5 / versionCode 25,
   notifybridge.json routes gained `missed.on`/`missed.off`.

## Revision: blink-light gate is 100% bridge events too (2026-09-23, v3.4)

1. **The last settings read is gone: no more `settings get` fork.** The
   "Blink light" gate (`arm_notification_ex()`) used `light_pulse_enabled()`
   which forked `/system/bin/settings get system notification_light_pulse`
   on every arm (3s TTL cache, `light_pulse_invalidate()` to force a
   re-read). The notify-bridge already watches
   `Settings.System.notification_light_pulse` with a ContentObserver and
   forwards every real change as `PULSE 0|1` - so the daemon read was a
   duplicate of an event it already receives, with a shell fork attached.
   Deleted: the `settings get` argv, `LIGHT_PULSE_TTL`/`s_pl_last`/
   `s_pl_val` cache, `light_pulse_enabled()`, `light_pulse_invalidate()`
   and their `chgd.h` declarations.
2. **`pulse_on()` / `pulse_note()`: a screen-style state pair.** The daemon
   now stores the toggle from the NLS `PULSE` command only (`pulse_note`,
   the core.c handler and the GUI's SIGUSR2 both feed it) and the next arm
   gates on `pulse_on()`; unknown before the first event = on (Android's
   default), same fault-tolerance as before.
3. **Bridge: PULSE now replays on connect (notify-bridge side).** The
   connect replay covered SCREEN/ring/voip/ENQ but NOT watched settings -
   the old daemon worked around that gap by re-reading the toggle itself.
   `replayInto()` now re-emits the current polarity of every watched
   setting (same contract as the screen snapshot, before ring/ENQ so a
   fresh consumer learns the toggle first). Forwarding of live changes is
   unchanged.
4. **notify-bridge repo build plumbing: the `release/` folder is a junction
   onto `app/build/outputs/apk/release`.** AGP's `stageReleaseApk` copy
   task wrote the APK into it = copying the file onto itself through the
   junction, producing a 0-byte artifact. The copy task and the redundant
   `build-release.bat` copy are removed - AGP's single write lands in the
   junction target directly and stays visible under `release/`.
5. **Audit of every remaining "side read":** the only child-process capture
   left in the daemon is the dialer's `content query` on call_log
   (missed-call verification) - it has no bridge event equivalent (the
   bridge classifies SIM calls but cannot read call_log), so it stays, and
   it is event-triggered with dedup, never a poll. Status/config runtime
   paths under `/data/local/tmp` are script-owned state files, not
   hardware. No other sysfs/settings reads remain.
6. Docs updated: README (wire format, system gates, SIGUSR2 hook,
   architecture map), module.prop bumped to 3.4 / versionCode 24,
   notifybridge.json comment mentions the connect replay.

## Revision: screen state is 100% bridge events, last sysfs hardcode gone (2026-09-23)

1. **The daemon no longer touches ANY sysfs for device state.** The screen
   detection in `util.c` used to read
   `/sys/class/leds/lcd-backlight/brightness` then
   `/sys/class/graphics/fb0/blank` whenever the bridge was not feeding
   `SCREEN` events (socket EOF -> `screen_source_reset()` returned the
   daemon to polling). That fallback is deleted: the bridge is the ONLY
   event source, and it already ships `SCREEN 0|1` (+ snapshot replay on
   connect) in `module/notifybridge.json` - nothing had to be added
   there. With the bridge down nothing can feed the notification pool or
   the charge band anyway, so the poll was pure dead weight: the daemon
   now just keeps the last known screen state, and the reconnect replay
   refreshes it.
2. **`screen_on()` is now a pure cache read.** `screen_note()` (the NLS
   `SCREEN` command) is the only writer; unknown before the first event
   reports off (a notification shows), matching the old fallback
   semantics. The `s_screen_evt` flag, `screen_event_driven()` and
   `screen_source_reset()` are gone from `util.c`, `chgd.h` and `core.c`.
3. **`notify_next_wake()` loses its 1s screen-off poll.** A parked top no
   longer keeps a heartbeat alive on a lit screen: the `SCREEN 0` command
   IS the edge that flashes the park, and the park is still bounded by
   the grace window. The notify timer now arms only for a real cap
   deadline or stays disarmed entirely. `queue_has_hold()` had no
   consumers left and was removed.
4. **Dead LED sysfs plumbing removed.** `write_sys()` and its `led_path()`
   name matcher (`LEDS[] = {red,green,blue,lcd-backlight}`) had no
   callers; they are deleted along with the `[led] trace_sysfs` key
   (removed from `led.conf`, the GUI's LedConf.kt round-trip and the
   README/comment story). The only remaining hardcoded device paths in the
   daemon are the config/status/runtime paths under `/data/local/tmp` and
   `/data/adb/modules/led_hal_root` (script-owned, not hardware).
5. **The three tmp+rename status writers collapsed into one helper.**
   `status_write()` (led_status), `nls_status_write()`
   (notifybridge.status) and `charge_write()` (led_chg) used to open a
   `*.tmp` file, write, close and rename by hand - the same pattern
   triplicated with three `_TMP`/`_PATH` macro pairs. They now all call
   a single `atomic_write(path, buf, len)` from util.c: full write to
   `<path>.tmp` then rename, so a consumer read can never see a
   half-written file. The `_TMP` macros and hand-rolled open/write/close
   sequences are gone.
6. Docs updated: README (screen transport, supervision, architecture),
   module.prop version bumped to 3.3 / versionCode 23.

## Revision: charge state from the bridge, sysfs poll gone (2026-09-22)

1. **Charge is now event-driven end to end.** mods/charge.c no longer
   reads `/sys/class/power_supply/battery/{status,capacity}` and carried
   no `REGISTER_MODE` cadence; the `REGISTER_UEVENT("power_supply")` hook
   is gone. The ONLY charge input is the bridge's `CHG` command:
   NotifyBridge forwards `ACTION_BATTERY_CHANGED` as
   `CHG <status> <level> [<plugged>]` (status word or raw BatteryManager
   int 2..5, level 0..100, optional EXTRA_PLUGGED bit). The broadcast is
   sticky, so a fresh receiver gets the current state immediately - the
   same guarantee the old recheck provided, with zero polling.
2. **The netlink uevent socket was removed from core.c.** The charge
   hook was its only consumer; without it the `chgd_uevents` link section
   stood empty, so `uev_dispatch()`, the socket fd and the
   `#include <linux/netlink.h>` were deleted outright. select() now
   watches the NLS sockets and the timerfd only.
3. **`plugged` gate against a stuck "Charging" status.** This device's
   BatteryService has been observed frozen at Charging/52% while no
   charger is attached (sysfs said Discharging/76; no broadcast since the
   unplug; system_server alive, no crash, no watchdog - `dumpsys battery
   reset` revived it). The bridge sends EXTRA_PLUGGED; `plugged=0` forces
   the band to "none", so a stale Charging can never light the LED.
4. **`--once` no longer evaluates.** It reads and applies the persisted
   band exactly as before, but there is no eval to run - CHG drives the
   band at runtime, the state file only survives a restart. Boot with no
   CHG yet = LEDs off.
5. **Charge logging is transition-only.** The bridge re-emits its sticky
   battery snapshot periodically (~30s); charge_note() now logs only
   real transitions - status change, plug change, or zone crossing -
   instead of an identical `Discharging 75% plug=0` line per re-emission.
   Band/state-file writes and the LED fingerprint logic are unchanged.
6. Docs updated: README (intro, charge event, wire format, architecture,
   macro list) drops the uevent/60s-recheck story; module.prop describes
   the charge band as bridge-driven with no sysfs reads and no polling.

## Revision: charge band recheck while charging (2026-09-20)

1. **The LED could park on the wrong charge band for a whole charging
   session.** This kernel fires a POWER_SUPPLY uevent only on plug/unplug -
   a capacity crossing mid-charge broadcasts nothing. So a phone plugged
   at 64% painted `lower` red at plug time and never repainted when the
   battery rolled through the 70% threshold into `middle`: the GUI (which
   reads `led_status`) and the diode both stayed on the low band while the
   charge walked up to mid. `eval_and_write()` runs only on uevent /
   SIGALRM / boot, and none of those exists between two thresholds.
2. **Fix: a `charge` mode owns the idle channel (`""`) while the charger
   is live.** A single 60s time-recheck re-evaluates the band (two sysfs
   reads) and repaints only on a real band/color/mode change - the
   existing `g_applied_band` fingerprint keeps the LED from blipping on
   every tick. The cadence is a `REGISTER_MODE` in charge.c, no core
   changes:
   - `eval_and_write()` now caches `g_charge_live` (status Charging/Full);
   - `charge_owns("")` returns true only while charging/full **and** the
     notification pool is empty (`queue_active()` / `queue_has_pending()`
     defer to notify's own idle claim), so a parked or showing
     notification never competes with the recheck, in any link order;
   - `charge_refresh()` ends with `retune_timer()` so plug arms the
     cadence from the uevent itself and unplug (status -> Discharging)
     drops it back to a disarmed idle - the next tick self-corrects even
     if an unplug event was missed.
3. Documented the gap: README (intro, charge event, architecture) admits
   the third bounded fallback. No new led.conf keys, no GUI changes.
4. **Periodic timer cadences are now phase-stable (core.c retune_timer).**
   A periodic policy (the charge recheck, the notify screen-off poll, the
   watchdog) that was already running at the same period is left alone:
   previously every unrelated retune (SCREEN toggle, NLS connect/cancel,
   power_supply uevent) restarted the countdown from zero, shifting the
   tick grid. On the charge recheck that made the band switch look
   coupled to whatever event retuned last - in the field it appeared as
   "the diode changed right when the screen went off", though the repaint
   itself always came out of the 60s tick. One-shot deadlines (cap
   expiry, adaptive wakes) still re-arm from zero by design - a full
   window from the event is their point.

## Release: v3.2 - supervision and the WD push removed (2026-09-19)

1. **The daemon no longer pushes `WD <ms>`.** `nls_push_watchdog()`, its
   two call sites (client connect, config reload) and the `WD <ms>`
   protocol line are gone from core.c. The `notifybridge.status` mirror
   drops the `watchdog_ms=` field (util.c `nls_status_write`).
2. **`[led] watchdog_ms` is removed** from led.conf and every config-side
   comment (config.c/led.c). Nothing reads the key anymore.
3. **Docs no longer claim any supervision.** module/README.txt,
   module/module.prop and module/service.sh now state plainly that the
NotifyBridge app is a pure transport: it restarts nothing, and if chgd
    dies it stays down until reboot or a manual `service.sh` start. The
    bridge-side watchdog was already removed when NotifyBridge was
    repackaged (app v2.0.0, see the android-notify-bridge repo).
4. **Module APKs now ship as `*-release.apk`.** The debug APKs
    (led_gui 54 MB, notifybridge 2.4 MB) are gone from `module/`; the
    module carries the R8-minified release builds instead (led_gui
    ~0.45 MB, notifybridge ~0.06 MB), each signed up-front with the
   debug keystore (`signingConfig = debug` in the two app gradle
   files, `apksigner` no longer needed) so `pm install -r` works
   from customize.sh and adb alike. `install_gui.cmd` /
   `install_nls.cmd` build `assembleRelease` and stage the new names;
   `customize.sh` installs them (bridge + GUI). The bridge repo path
   is fixed to `projects\android-notify-bridge`. Module zip drops
   from ~57 MB to ~1 MB; only the binary `chgd` keeps it
   non-trivial.

## Release: v3.1 - test_sec removed, test ring held until Disarm (2026-09-19)

1. **`[ring] test_sec` removed.** The test-rainbow hold timer is gone from
   led.conf and the GUI (Call tab). A test ring behaves like every other
   test hook: it holds until Disarm, bounded only by the `[ring] max_sec`
   safety cap when one is set.

## Release: v3.0 - debug-named APK pair (2026-09-19)

1. **Module APK artifacts renamed to their Gradle debug output names.**
   The module no longer keeps hand-renamed copies: it consumes the
   standalone projects' build outputs as-is.
- NotifyBridge: `nls.apk` -> `notifybridge-debug.apk` (from the
      `android-notify-bridge` repo, `archivesName = notifybridge`)
   - LED GUI: `led_gui.apk` -> `led_gui-debug.apk` (from `led_gui/`,
     `archivesName = led_gui`)
   `customize.sh`, `.gitignore`, `build_module.cmd`,
   `install_nls.cmd`/`install_gui.cmd` (build + stage + install) and the
   docs all point at the new names. The stale `module/nls.apk` and
   `module/led_gui.apk` are gone.
2. **`install_nls.cmd` now finds the bridge repo.**
   The standalone project moved to `projects\android-notify-bridge`; the
   script's `NLS` path and consumed artifact name follow.
3. **GUI: the Info tab's bridge button reads `Config`** - a stray leading
   slash (`/Config`) was removed.
4. Module bumped to v3.0 / code 20.

## Release: v2.17 - event-driven screen detection, the last 1s poll is gone (2026-09-19)

1. **The kernel emits no uevent on backlight change.** Probed on device
   with a netlink `KOBJECT_UEVENT` dump: `input keyevent 223/224` flips
   `/sys/class/leds/lcd-backlight/brightness` `27 -> 0 -> 27` but nothing
   is broadcast (`/sys/class/graphics/fb0/blank` does not even exist
   here). That is why v2.16 still needed a 1s sysfs poll to notice the
   screen falling.
2. **The daemon trusts the `SCREEN <0|1>` event.** `screen_note()`/
   `screen_event_driven()` in util.c cache the state pushed by the bridge;
   `screen_on()` prefers it and falls back to the sysfs read while the
   bridge is down. On `SCREEN 0` the core calls `queue_arbitrate()` right
   away and re-tunes the timer, so a parked (`Q_HOLD`) notification
   flashes the instant the screen falls.
3. **`notify_next_wake()` drops the 1s poll when the event source is
   live**: a parked top with `screen_event_driven()` now returns 0, which
   the core turns into a single `WATCHDOG_SEC` safety pass instead of a
   per-second wakeup. The 1s sysfs poll remains only as the fallback for
   when the bridge is absent/down (`screen_source_reset()` on socket EOF
   restores polling).
4. **The watchdog cadence is event-driven end to end.** A non-root app
   cannot read led.conf at all (`/data/adb` is `700 root:root` - the
   daemon is the only legitimate reader), so the GUI touches
   `[led] watchdog_ms` in led.conf and SIGALRMs chgd (the same poke it
   already used for the log toggle); the daemon reloads and pushes
   **`WD <ms>`** over the socket on every reload *and* every connect, and
   mirrors the value into its world-readable status file for the app's
one-time start. The NotifyBridge app's own 10s config poll is gone (that side
    moved to the notify-bridge repo).
5. **The daemon now watches its own config with inotify - zero stat()
   left in the lookup path.** `conf_watch_init()` opens an `IN_NONBLOCK`
   inotify fd and watches `CONF_DIR` (the directory, not the file, so
   sed -i's temp+rename - a new inode - is caught by `IN_MOVED_TO` on
   the name, exactly like an in-place rewrite is caught by
   `IN_CLOSE_WRITE`). The fd joins the core's `select()`
   (`chgd running (... cfg=N)`); on readiness `conf_watch_handle()`
   drains and flags led.conf via `conf_note_change()`, then the loop
   reuses the reload branch (refresh + WD push + mirror update). So any
   writer - the GUI, a root shell, an OK-file browser - is picked up
   instantly, no SIGALRM required; the SIGALRM poke from the GUI
   survives as an explicit reload trigger that now feeds the same
   dirty-flag. `conf_maybe_reload()` consumes the flag instead of
   stat()ing on every lookup; the old stat-based probe remains only as
   a fallback if inotify ever fails to initialize. Verified live:
   `sed -i` (rename) and `cp` (in-place) of led.conf both reload + push
   (`watchdog pushed from daemon: 60000/88888ms`) with no signal sent.

Verified on device: ENQ -> `parked (screen on, grace 60s)` + `timer ->
300000ms (event-driven) one-shot`; `SCREEN 0` -> `notify armed` +
`queue: flash` in the same second; `SCREEN 1` on wake. Live cadence:
`sed watchdog_ms=12345` + `kill -ALRM $(pidof chgd)` -> the push recarries
`12345ms`, then back to 60000 steadily.

## Release: v2.16 - notification priority pool + LIFO preemption (2026-09-18)

The notification pipeline is now a real event pool (`mods/queue.c`)
instead of a single screen-on park slot:

1. **Every ENQ lands in the pool unconditionally** (`queue_push`, after
   the suppress gate in notify.c). Selection is LIFO by recency: the
   freshest post wins, an older one is preempted with resume-credit
   (`shown_ms`), so alternating app traffic cannot blink forever - the
   `[notify(.app)] notif_max_sec` cap is enforced on the ACCUMULATED
   show time, and a preempted event finishes its leftover on return.
2. **Screen-on staging has no per-entry timer.** A fresh top that lands
   on a lit screen parks as `Q_HOLD`; the drive to flash comes from the
   screen edge or, as a fallback, a lazy heartbeat that arbitrates once
   the screen falls or the grace (`notify_screen_delay_ms`) runs out
   (drop the top only - surviving entries keep their own clocks).
   Lifetime is `created + notif_max_sec`, checked lazily at pick time.
3. **Preemption is total**: a higher-recentcy post behind the LED returns
   the current entry to the pool with its time accrued and arms instead;
   on the next turn the displaced entry resumes from the leftover.
4. **Cancel semantics preserved**: per-(pkg,id) removal, LED disarmed
   only when no entries of the armed package remain (cancel+post
   rebuild continues the same show on the surviving entry with the
   credit carried over). The old `active_ids` bookkeeping in core.c was
   deleted - the pool IS the ledger.
5. **Emergency planes unchanged and untouchable**: while ring/voip/missed
   or alarm own the channel, `queue_arbitrate()` simply returns - the
   pool keeps accumulating, times still tick in `created`, and the lazy
   expire check handles what aged out. Backlog cannot light up mid-call.
6. `PULSE 0` / SIGUSR2 (the system "notification light" toggle) now also
   clears the pool, so a disabled LED cannot leave a stale backlog.
7. **The steady-state 1s heartbeat is gone.** With `notif_max_sec=0`
   (unlimited, the shipped default) the core used to stay armed at a
   fixed 1s period for as long as any notification sat on the LED - a
   permanent per-second wakeup. The core now treats a mode's
   `next_wake_ms()==0` as "no deadline at all" and arms a single
   `WATCHDOG_SEC` safety pass instead of the 1s fallback. The only 1s
   poll left is a top parked on a lit screen (screen-off has no uevent
   here), and that poll is bounded by the grace window - which now also
   drops the park while the screen stays on, instead of holding it until
   the lifetime cap.
8. **`notif_max_sec=0` no longer insta-expires.** The lifetime check was
   `age >= notif_max_sec`, so 0 (documented as unlimited) dropped every
   post the moment it was picked. 0 now genuinely means unlimited: the
   entry lives until cancelled, with a single WATCHDOG_SEC safety pass.
9. Fixed line endings: `.gitattributes` forces LF for `.sh`, `.prop`,
   `.conf` (and `.txt`). Windows `core.autocrlf` had left CRs in the
   module scripts, which broke the on-device shell
   (`sleep: Unknown suffix '\r'`) and the KernelSU install hook.

Handlers gained an `id` parameter (`struct pkg_handler`); exact-match
plugins (dialer, alarm) still claim before the `"*"` default. No config
schema changes, no new keys.

Releases ship the flashable module zip ONLY (`led_hal_root-vX.Y.zip`).
The full-source bundle is gone - sources live in the repo, nobody wants
a second archive on every release.

## Release: v2.15 - screen-on notification park fixed (2026-09-17)

1. **A notification parked while the screen is on could die silently.**
   notify.c parked screen-on notifications in `g_pending_pkg` and
   relied on a screen-off uevent that never existed, with the core
   timer disarmed in idle - so a message that arrived with the screen
   up then went dark without ever flashing. The park now claims the
   idle channel (`notify_owns("")` while a park exists) and re-arms
   the timer (`retune_timer()` right at park time), so the drive checks
   the park and flashes it the moment the screen falls asleep.
   `notify_screen_delay_ms` (default 60000) became a real grace
   window: a park is dropped as soon as it ages past the limit, even
   if the screen stays on forever - no stale flash an hour later, no
   pointless idle heartbeat. Age counts from the last parked event (a
   new screen-on notification replaces the slot and restarts it).
   A dismissed parked notification can no longer flash later:
   `notify_pending_cancel()` is called at the top of cancel dispatch,
   so CAN/CAN_ALL clears the park even when nothing is armed or a
   different package owns the LED.
2. Module bumped to v2.15 / code 17; the shipped APK pair was rebuilt
   so the module always carries fresh builds.

## Release: v2.14 - bridge split out of the GUI, notification-driven calls (2026-09-16)

1. **Notification bridge split out of the GUI: standalone headless NotifyBridge app
    (`com.bastet.notifybridge`).** The `NotificationListenerService` and the su
    watchdog are no longer built into the GUI APK - they live in the
    standalone notify-bridge project now. The GUI's
    `LedNotificationListenerService.kt` is deleted and no listener service
    remains in the GUI manifest; the GUI (`com.bastet.ledgui`) is a pure
    configurator. A bare daemon + NotifyBridge gives the full notification stack.
   `customize.sh` installs/refreshes both APKs on every module install;
   `install_nls.cmd` only consumes the standalone project's `nls.apk`.
2. **VoIP detection is notification-driven (no polling).** voip.c no
   longer shells out to `dumpsys media.audio_policy` / `telecom`. The
   bridge classifies call notifications and sends `VOIP_ON <pkg>` /
   `VOIP_OFF <pkg>` over the socket; the daemon arms/disarms the rainbow
   on those events. `[voip] max_sec` is now purely a safety cap for a
   lost `VOIP_OFF`, not a poll-driven half-life of the channel.
3. **Call end is event-driven too (core.c + ring.c).** The events
   buffer's `input_focus` records fed a `g_incall` call-window tracker
   ("Focus leaving ... InCallActivity" with `reason=NO_WINDOW` ends the
   rainbow with zero polling). SUPERSEDED: the event-log/`input_focus`
   stream and the `g_incall` tracker were removed outright once NotifyBridge
   bridge became the only transport - the daemon reads no logdr/event-log
   at all now and call end is resolved from `RING_OFF` alone (see
   mods/ring.c, which carries no focus/logcat code).
4. **Daemon honours the system "Blink light" toggle.** notify.c gates
   every notification through `light_pulse_enabled()`
   (`Settings.System notification_light_pulse`, read via `settings get`,
   3s TTL cache + `light_pulse_invalidate`). Turning the system
   notification light off drops every notification at this single gate;
   call rainbows, alarms and the charge band are deliberately NOT gated.
   The toggle is watched event-driven end to end: the bridge forwards any
   writer's change over the socket as `PULSE <0|1>` (`0` disarms the
   notification LED immediately, `1` just drops the 3s cache); SIGUSR2
   stays as the GUI's direct fallback. customize.sh forces the toggle ON
   on a fresh install. The GUI's old in-app toggle checkbox was removed -
   the system setting page is the one control.
5. **`/data/local/tmp/notifybridge.status` is written by the daemon, not the
   app.** chgd writes `connected=1` on NLS accept, `connected=0` on
   client EOF and at startup with no client (util.c `nls_status_write`,
   atomic tmp+rename). The app-side write needed su at the exact
   transition and raced the allowlist; the daemon writes it in-process,
   no su, no race. The GUI reads that file for its bridge state instead
   of an in-process static.
6. **GUI: "Open KernelSU Manager" actually opens the manager, and the
   root card explains a hidden su.** The manifest gained a `<queries>`
   block (KernelSU / KernelSU Next / Magisk / APatch / SuperSU + a
   LAUNCHER intent) so package visibility on Android 11+ no longer hides
   the manager. `requestRoot()` no longer paints an optimistic PENDING
   flash: when su is hidden from the unallowlisted app the card parks on
   a persistent orange "no su binary reachable" line with the allowlist
   hint, and the status poll will not clear it - it turns green the
   moment root lands.
7. Module bumped to v2.14 / code 16. led.conf stays canonical (always
overwritten). The status card's bridge line now reads `notifybridge.status`
    and checks `com.bastet.notifybridge` for the granted listener.

## Release: v2.13 - canonical config + GUI in the module zip (2026-09-15)

1. **led.conf is canonical and always overwritten on install**
   (customize.sh): the repo template is the source of truth now. The
   old awk merge script (template base + live values + [rules]/
   [suppress] union) is deleted. Ship the tuned on-device config (from
   the master's phone) as the template, keep `[led] watchdog_ms` and
   `[led] trace_sysfs` documented. What survives installs is decided in
   the repo, not on the device. The GUI's Save still rewrites led.conf
   as before - it had nothing to do with the install-time merge.
2. **LED GUI ships in the module zip** (led_gui.apk): customize.sh runs
   `pm install -r` on every module install/update, so a single zip
   carries the whole stack. Version 1.2 (versionCode 3). A failed apk
   install is non-fatal and reported via ui_print.
3. **Optional watchdog interval** (`[led] watchdog_ms`, LED GUI 1.2
   Daemon card): supervision is pure crash insurance, but every poll
   spawns an su shell (wakes the CPU in idle). The 10s fixed poll
   becomes `[led] watchdog_ms` (default 60000, 0 = disabled), editable
   live from the Status tab (Apply button) - the daemon reloads the key
   on SIGALRM and pushes the cadence where the supervisor reads it.
4. **Debug-only sysfs tracing** (`[led] trace_sysfs`): the per-write LED
   sysfs log lines and the "LED peak" line are now gated behind
   `[led] trace_sysfs=1` (default 0). Default installs log arming/
   disarming only, no per-write noise.
5. **Log noise removed**: `[led] skip red (amp 0)` line deleted from
   led_breathe_rgb; `charge leds -> none` only logs when a real band is
   applied (the status file is still written either way for the GUI).
6. Template config fix: charge soft-breath keys are
   `lower/middle/upper_soft_breath` (the `_range_`-prefixed variants
   were dead since v2.11; charge.c always read the short names).
7. Installer no longer ships keepalive.sh (the daemon is supervised by
   the NotyBridge app now); the runtime cleanup re-removes stale per-file `.c`
   copies from old releases.

## Revision: ID-based notification tracking + pseudo-package cancel bridge (2026-09-10)

Two issues fixed in the cancel/disarm pipeline:

1. **ID-based notification tracking replaces cancel_grace (core.c)**:
   Android apps (Messages, Telegram, RCS) frequently cancel+repost
   notifications during rebuilds (conversation updates, delivery status
   changes). The old `cancel_grace` timer (2s) was too short - a cancel
   arriving after the grace expired would disarm the LED even though the
   notification was still live. chgd now tracks per-(package, notification
   ID) pairs: `id_add()` on `notification_enqueue`, `id_remove()` on
   `notification_cancel`. The LED is disarmed only when the last active
   ID for the armed package is removed. `notification_cancel_all` clears
   all IDs for the package. Purely event-driven - no timers, no grace
   windows, no polling.

2. **Pseudo-package cancel bridge via `owner_pkg` (core.c, chgd.h,
   dialer.c)**: mods/dialer.c arms the LED under the pseudo-package
   `"missed.call"`, but the real cancel arrives from the actual app
   package (`"com.google.android.dialer"`). The old `cancel_dispatch`
   compared `g_st.cur_pkg` against the cancel package and never matched -
   swiping away a missed-call notification left the red LED breathing
   forever. Fix: `notif_state` now carries `owner_pkg`, the real
   package that owns the LED. `cancel_dispatch` checks `owner_pkg` (or
   `cur_pkg` when `owner_pkg` is empty) so a cancel from
   `"com.google.android.dialer"` correctly disarms `"missed.call"`.
   Real-package notifications (Telegram, etc.) set `owner_pkg` to
   empty, preserving the direct `cur_pkg == pkg` match.

Files changed: core.c (cancel_dispatch, id_* tracking), chgd.h (notif_state
owner_pkg, ev_parse id_out param), notify.c (arm_notification_ex clears
owner_pkg), dialer.c (missed_paint sets owner_pkg to DIALER_PKG).

## Revision: config schema v2.8 - VoIP rainbow / missed section / ring cap + LED GUI (2026-09-05)

Docs catch-up with the v2.10 tree (the sections below were in the code
but missing from this changelog).

1. **VoIP call state (mods/voip.c)**: when a messenger is actually in a
   call (audio policy `AUDIO_MODE_IN_COMMUNICATION` / `IN_RINGTONE`, or an
   unanswered self-managed ConnectionService call in `dumpsys telecom`),
   its LED becomes the rainbow; ordinary chat messages keep their `[rules]`
   color. (Superseded in v2.14 by notification-driven VoIP.) New config:
   - `[voip] max_sec` - grace timeout while the voice channel reports "no
     call" before the rainbow disarms (default 2; guards lost call-end
     events and audio-policy blips, NOT a hard cap - a live call keeps the
     rainbow for its whole duration)
   - `[voip] packages` - comma list overriding the default messengers
     (Telegram / WhatsApp / Viber / Signal / Snapchat / Duo)
2. **Missed-call LED owns a config section ([missed])**: mods/dialer.c now
   reads its whole behavior from led.conf instead of fixed blue breathing:
   `light_type` (0-3), `soft_breath` (software-breathe override),
   `soft_cycle_ms`, `rise/hold/fall/offt` (breath ms), `color` (r,g,b),
   `max_sec` (default 1800). Verification window unchanged (fresh call_log
   row <=120s, dedup by _id). Default color in the GUI/conf seed: red.
3. **Ring blink cap is configurable ([ring] max_sec)**: mods/ring.c replaced
   the hardcoded `RING_MAX_SEC` (120s) with `[ring] max_sec` (default 0 =
   unlimited). A capped incoming ring resolves into the missed-call check
   instead of silently killing the LED, so a missed row landing at the cap
   is still caught.
4. **LED GUI grew to six tabs**: Info / Charge / Notification / Call / VoIP
   / Alarm with horizontally scrollable tab strip (tabs no longer crush on
   narrow widths). Call gained the missed-call editor and the ring blink
   cap; VoIP moved out of Call into its own tab (silence grace + package
   list, one package per line in the editor, serialized back to the comma
   list the daemon expects). GUI defaults now mirror the tuned on-device
   config (charge colors `255,32,32 / 255,127,32 / 64,255,32`, soft breath
   on for lower+middle bands).

## Revision: screen-on detection fix + timer dedup (2026-09-03)

Two fixes driven by debug session with verbose log (55K lines, `-v` flag):

1. **NOTIFY_PERM_SEC 60 -> 2 (mods/notify.c)**: On this MediaTek Android 13
   GSI kernel, KOBJECT_UEVENT for `fb0`/`lcd-backlight` is **never sent**
   to unprivileged listeners - confirmed by zero `uev:` lines with matching
   header in debug log. The only uevent arriving is
   `change@/devices/virtual/xt_idletimer/timers`. Screen-on detection was
   therefore 100% timer-dependent, and with `NOTIFY_PERM_SEC=60` the poll
   interval was 60 seconds - user saw LED breathing white for a full minute
   after unlocking. New value: 2 seconds (matches the polling cadence used
   during active notification). The `notify_screen_uevent()` function still
   exists for kernels that DO send fb0/lcd uevents, but the timer is now the
   reliable fallback.
2. **retune_timer() dedup (core.c)**: One-shot timer re-arms (notify `-> ms`
   or ring `-> RING_STEP_MS`) set `curms=0` before calling `retune_timer()`,
   so the existing `curms == ms` dedup never triggered for them. Every
   re-arm logged `timer -> %dms (%s) one-shot` even when nothing changed -
   41,726 identical lines in one session. Added `g_last_set_ms` persistent
   tracker that survives one-shot resets: second and subsequent calls with
   the same target value are silently skipped. Net idle log output: zero.

Debug logging (`uev:` in `uev_dispatch()` and `screen uevent:` in
`notify_screen_uevent()`) was added during the session and removed before
final build.

## Revision: modular source layout v2 (2026-08-29)

Planned layout applied. The daemon is now split as:
- core: core.c (event loop, signals, timer policy), led.c (LED hardware
  primitives + rainbow cycler - the ONLY writer of the RGB channels),
  config.c (INI runtime config), util.c (log / sysfs helpers / status
  file / screen detect), tele.c (dumpsys capture), notify.c (event
  parser + dispatch), charge.c (charge bands)
- mods (optional): ring.c (call rainbow mode), dialer.c (missed-call
  verification)
- config.c gained a generic key-value store: every unknown
  [section] key=value in led.conf is readable via conf_get_str /
  conf_get_int, so a mod owns its own config section without config.c
  knowing the key exists. Example section:
[ring]
       max_sec=300        # live-ring safety cap in seconds, 0 = unlimited
   Test rainbows carry no hold timer of their own: they run until Disarm
   or the configured [ring] max_sec cap.
- Rainbow cycler moved from ring.c into led.c (led_rainbow_reset/step/
  rgb); ring.c only starts/stops it. No LED code outside led.c anymore.
- build.cmd tracks the new file list (chgd.c and mods/conf.c removed;
  behavior byte-for-byte identical).

## Revision: live status file for the GUI (2026-08-29)

chgd now maintains /data/local/tmp/led_status (atomic tmp+rename, same
pattern as led_chg) so an external GUI can show the current LED state
without parsing logs:
- ts / mode (charge|notify|ring) / band (lower/middle/upper/none) /
  pkg (armed package or incoming.call/outgoing.call) / color=r,g,b /
  type (0=off 1=breathing 2=flashing 3=static)
- Written from the three LED-owner points only: apply_charge_leds
  (charge.c), arm_notification (notify.c), arm_ring (mods/ring.c).
  Disarm / ring end / screen-on all route back through
  apply_charge_leds, so the file always mirrors reality.
- ring.c gained g_cur_r/g_cur_g/g_cur_b so the status shows the actual
  rainbow color instead of a placeholder.
- No new files, no config schema change.

## Revision: full RGB colors, masks removed (2026-08-29)

1. Colors are now RGB triplets (0-255 per channel) end to end instead of
   the bit masks (4=red, 2=green, 1=blue). Per-channel brightness is
   written into each LED node, so any color is expressible, not just the
   7 mask combinations (and the AW2033 blue-dominated diode is no longer
   hardcoded to the lens).
2. led.conf schema (v2.7) - replace the old mask values:
   - `[rules] pkg=r,g,b`          (was `pkg=color-mask`)
   - `[charge] first_threshold / second_threshold` (percent, order-free -
     swapped automatically)
   - per-range light types `lower/middle/upper_range_light_type`
     (0=off 1=breathing 2=flashing 3=static) and colors
     `lower/middle/upper_range_color=r,g,b` (band colors were hardcoded
     red/amber/green in charge.c)
   - `[notify] default_color=r,g,b` (white fallback for unlisted apps)
   Old-style single-number values are rejected and logged, not misparsed.
3. Range abstractions: thresholds are `first_threshold`/`second_threshold`,
   ranges are lower/middle/upper end to end (state file, logs, conf), and
   the behavior per range is explicitly configurable - previously upper
   was always solid and the names amber_at/green_at lied once the colors
   changed. "middle" defaults to flashing (square blink, fades forced 0).
4. Applied-charge cache is now a fingerprint (band + color + light type +
   timings): editing led.conf colors/types while charging re-applies on
   the next event instead of waiting for a band change. The "none" band
   also explicitly turns all channels off now.
5. Sources: struct led_rule / REGISTER_RULE take r,g,b; util.c gained
   led_solid_rgb / led_breathe_rgb (led_set now writes any level 0-255);
   notify.c resolves rgb_for() per package; charge.c reads its per-range
   colors and light types from conf.c. Defaults: lower breathing red,
   middle flashing lime, upper static green.
6. Bumped module to v2.7 / code 9.

## Revision: stale legacy LED daemons (2026-08-28)

Second "spurious blue/purple" report (no `notify armed` in the log, yet
green+blue channels lit at 255 with led_time "15 15 15 15" - the white
pattern reads blue/purple on the blue-dominated AW2033). Root cause:
orphaned legacy daemons `worker.sh` and `listener.sh` from the OLD pre-chgd
module stack. They survived the module update as PPid=1 and kept writing
their own colour into the RGB sysfs nodes, fighting the single chgd daemon.
Fix: killed both by hand once. keepalive stays at v7 - it does NOT scan for
these legacy names by design: the files no longer exist in the module and
the current service.sh never launches them, so they cannot come back.
No reaping logic, no wasted cycles.

## Revision: background-notification blacklist (2026-08-28)

Spurious colour switch observed while charging: the red charge breath changed
to blue/purple on its own and only went back to red after waking the display.
Root cause was NOT an internal LED bug - the default white (mask 7) for
unlisted packages lights red+green+blue together, and on the AW2033 the blue
channel dominates so it reads as blue/purple. `com.google.android.google
quicksearchbox` (Google Discover) posts such background feed notifications
spontaneously while the screen is off; they claim the LED away from the
charge indication (notifications take priority) and only drop it again on a
screen-on disarm tick.
Fix: expanded the suppress blacklist with these quiet background emitters
so they can never steal the LED:
- com.google.android.googlequicksearchbox   (Discover / Google app - #1)
- com.android.providers.media.module        (media/storage scanning)
- com.google.android.apps.nbu.files         (Google Files space hints)
Any further noise: add one REGISTER_SUPPRESSED line in mods/suppress.c and
rebuild. Verified against installed packages (superseded viber.voip, which is
not present, and dropped non-installed Assistant/TV/Chromecast entries).

## Revision: ColorNote night sync blacklist (2026-08-29)

"LED breathing blue/purple/white at night for no reason" report (no charger
connected, screen off, user asleep). ledd.log showed exactly ONE armed event
overnight: `09:43:00 notify armed: com.socialnmobile.dictapps.notepad.color
.note mask=7`. logcat pinned it - ColorNote started a background
`DailySyncJobService` + `SyncService` (nightly auto backup/sync of notes) and
posted a mask=7 notification (red+green+blue = white, reads blue/purple on
the AW2033). No visible user notification, no charger, so it looks
spontaneous to the user.
Fix: added com.socialnmobile.dictapps.notepad.color.note to the suppress
blacklist so its night sync can never steal the LED.

## Revision: runtime config, no-recompile editing (2026-08-29)

The blacklist, per-app colours, breathing timings and charge thresholds are
now user-editable in a plain text file:
`/data/adb/modules/led_hal_root/led.conf` (shipped with the module).
Sections: [suppress] (one package per line - never lights the LED),
[rules] (pkg=color-mask), [charge] (amber_at/green_at + breath ms),
[notify] (shared behavior for ALL apps: breath ms + notif_max_sec;
default_color applies only to apps without a [rules] entry).
- New mods/conf.c parses it; runtime entries MERGE OVER the link-time
  registries (files win on conflicts, suppress adds). No recompile needed.
- Lazy reload: conf_maybe_reload() stat()s the file and only re-reads when
  the mtime changed, so editing led.conf applies on the next processed
  event - no daemon restart, no rebuild. Verified live: adding telegram to
  [suppress] took effect on the very next enqueue without a restart.
  (Superceded by the inotify watcher in v2.17.)
- Core hooks: chgd.h declares conf_* getters; notify.c (mask_for,
  suppressed, arm_notification timings), charge.c (band thresholds +
  breath timing) and chgd.c (notif_max_sec) now consult them. Builtin
  defaults unchanged when the file is absent.

## Revision: modular source layout (2026-08-28)

1. chgd.c is no longer a monolith. Sources split into a stable CORE
   (chgd.c / chgd.h / util.c / tele.c / notify.c / charge.c) and
   extension MODS (mods/rules.c, mods/suppress.c, mods/ring.c,
   mods/dialer.c). Behavior is byte-for-byte the v2.6 logic.
2. Extension mechanism: REGISTER_RULE / REGISTER_SUPPRESSED /
   REGISTER_HANDLER / REGISTER_MODE macros. Each entry is a static
   const struct placed by __attribute__((section)) into chgd_rules /
   chgd_suppressed / chgd_handlers / chgd_modes; the core iterates the
   linker-synthesized __start_/__stop_ bounds. A new app color/handler
   = a new .c file under mods/ and a rebuild. No core edits, ever.
3. build.cmd compiles the core + every mods/*.c in one NDK clang call
   (single static binary). apply.cmd now rebuilds before pushing.
4. Old single-file sources archived under legacy/ as a fallback.

## Revision: outgoing-call rainbow (2026-08-27)

1. LED never lit on OUTGOING calls. The old trigger only armed the
   rainbow when mCallState==1 (RINGING / incoming), so dialing out
   (mCallState==2 / OFFHOOK) did nothing.
2. Fix: dialer posts a notification_enqueue for outgoing calls too
   (channel phone_ongoing_call). That same event-time probe now uses
   tele_active() (mCallState 1 OR 2) instead of tele_ringing() (only 1),
   so an active outgoing call arms the same rainbow. Still fully
   event-driven - a dialer ping triggers a one-shot dumpsys probe, no
   polling loop added.
3. Call direction is recorded (g_ring_incoming, from a tele_ringing()
   probe at arm time) so the end-resolution differs: incoming ->
   missed-call check (blue breath), outgoing -> straight back to the
   charge leds (a missed row can never appear for an outgoing call).
4. Bumped module to v2.6 / code 8.

## Revision: field-fixes (2026-08-24)

1. Rainbow died ~0.5s into a real ring: this ROM wakes the display for
   incoming calls and the screen-on guard killed the effect. Ring mode
   now ignores screen state completely (fb/lcd uevent path included);
   exits only on ring end or RING_MAX_SEC (120s).
2. Telegram breathing "stopped after a while" = NOTIF_MAX_SEC timeout,
   by design (was 600s, worker.sh parity). Raised to 1800s.

## Revision: incoming-call rainbow

Dialer pings are now cross-checked against telephony state
(dumpsys telephony.registry -> mCallState, any line):
- mCallState==RINGING at ping time (or during the verification window)
  -> rainbow mode: timer drops to 32ms; each tick interpolates toward
   the next of 8 palette anchors along a cosine-eased curve (16 steps
   per segment, ~4.1s full cycle), written straight into
   {red,green,blue}/brightness - colors glide, no hard switching.
   First state re-poll is deferred by RING_CHK_SEC so an instant
   hang-up still gets a short glow instead of a zero-length flash.
- Ring end (state leaves RINGING, polled every 1s) -> resolve outcome:
   fresh missed row -> blue missed-call breath; answered/reset ->
   back to charge leds. Screen-on disarms immediately as usual.
- NOTIF_MAX_SEC cap applies; repeated dialer pings during an active
   ring are ignored (no rainbow restart).
Test hooks: SIGWINCH = force rainbow (bypasses state check; on an idle
device it ends after the first 170ms tick - by design, real ringing
keeps it alive). Verify live with an actual incoming call.

## Revision: missed-call indication

Dialer pings (com.google.android.dialer) no longer arm the LED blindly.
Every dialer notification is verified against call_log via root
`content query`: a fresh (<=120s) MISSED row (type=3, new=1) arms blue
breathing; ringing/ongoing-call notifications are ignored. Verification
window: up to 4 checks x 2s (absorbs provider/db race), dedup by call_log
_id (notification updates do not re-arm). Timer policy gains a 2s
"call check" state between armed(1s)/retry(3s)/watchdog(300s).
Test hook: kill -HUP $(pidof chgd) = fake dialer ping (real call_log
check runs; insert a type=3/new=1 row to see it fire).
Note: this ROM's call_log provider rejects insert binds for description
and country columns; use type,new,number,date,duration only.

## Revision: trigger-driven timer (same day)

Replaced the fixed 30s timerfd with an adaptive single-shot policy:
- notification armed -> 1s (screen re-check; NOTIF_MAX_SEC cap unchanged)
- logdr disconnected -> 3s reconnect backoff (was: up to 30s dead air)
- idle, links up     -> 300s watchdog only (missed-uevent safety net)
All real transitions were already trigger-driven (netlink uevents for
power_supply/fb0, logdr stream for notifications); the tick is now just a
fallback and is re-armed via retune_timer() on arm/disarm/logdr drop.
Steady idle state: zero periodic LED work between watchdog ticks.

(Note: the event-log/logdr path in the entries above was removed entirely
in v2.14 - the socket transport is the daemon's only notification source.)