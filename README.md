# led_hal_root - Notification LED daemon for Blackview Shark 8

KernelSU module that drives the AW2033 RGB notification/charge LED on a
rooted Blackview Shark 8 running a ported Pixel GSI ROM. A single
event-driven daemon (`chgd`) programs the chip - no polling scripts exist
outside of two short device-side boots, a bounded screen-state fallback,
and the 60s charge-band recheck (see charge.c: this kernel fires a
POWER_SUPPLY uevent only on plug/unplug, so a mid-charge threshold
crossing would otherwise never repaint).

Both companion apps ship inside the flashable zip, but only one of them
is this project's:

- **NotifyBridge** (`notifybridge-release.apk`, `com.bastet.notifybridge`) - the required headless
  notification bridge. It is a **standalone project**
  (`android-notify-bridge` repo); this repo only consumes its `notifybridge-release.apk` artifact.
- **LED GUI** (`led_gui-release.apk`, `com.bastet.ledgui`) - the optional
  configurator. **This** project's app, lives here under `led_gui/`.

The AW2033 chip controller is also external: the packed `libaw2033.a` +
`aw2033.h` in `aw2033-driver/` are the only pieces of the standalone
[aw2033-driver](https://github.com/nalbe/aw2033-driver) repo that this
project ships (the chip driver and the `awctl` CLI are built there).

## What is in this repo

```
led_hal_root/   daemon core only (C sources, mods/, build.cmd) -> chgd
led_gui/        optional configurator app (com.bastet.ledgui)
module/         module packaging: customize.sh, service.sh, module.prop,
                led.conf, META-INF, README.txt, awctl prebuilt,
                notifybridge-release.apk (from android-notify-bridge) + led_gui-release.apk
release/        flashable zip output (led_hal_root-v<ver>.zip)
aw2033-driver/  libaw2033.a + aw2033.h ONLY (prebuilt chip controller)
```

Build scripts: `build_module.cmd` (full release), `build.cmd` (chgd
only), `install_core.cmd` (build + deploy to device), `install_gui.cmd`
(build GUI app), `install_nls.cmd` (consume the notify-bridge notifybridge-release.apk).

## Install

1. Download [`led_hal_root-v3.0.zip`](https://github.com/nalbe/shark8-led-daemon/releases/latest) (flashable KernelSU module)
2. Flash in KernelSU Manager -> Modules -> Install from storage
3. `customize.sh` installs both apps automatically (`pm install -r`,
   non-fatal on failure):
   - **NotifyBridge** - required for the notification/call/VoIP/alarm LEDs
   - **LED GUI** - optional configurator
4. Grant **NotifyBridge** Notification access (Settings -> Special app access
   -> Notification access -> NotifyBridge). LED GUI additionally needs root
   (KernelSU Manager) for its status/config screen.
5. Reboot

The `led.conf` shipped inside the module is canonical and is **always
overwritten** on every module install/update - if you customised it, keep
your edits in the repo copy, or use the GUI (which saves edits straight to
the live config file).

## The daemon

Everything animates inside the AW2033 chip; chgd only programs registers.
The kernel fires no uevent on backlight change, so screen state (and
hence most wakeups) comes from the bridge over the socket - the daemon is
asleep in idle.

### Events and renderers

Any event can use any hardware renderer. Each event owns its own
`[section]` in `led.conf` with `mode=off|solid|breath|wave`, and each
renderer's chip tuning lives in its own `[section.solid]`,
`[section.breath]`, `[section.wave]` sub-sections - per-channel current
(`cur`), `rise/hold/fall/offt` timing, breath repeat count and the wave
phase offsets `t0=r,g,b` (the staggering that produces the traveling
rainbow). Timing is owned by the chip sections; there is no fallback.

Events:

- **charge** - three bands (lower/middle/upper), each with its own
  renderer, thresholds in `[charge]` (`first_threshold` /
  `second_threshold`, order-free). Re-evaluated on `power_supply`
  uevents (plug/unplug), on SIGALRM/inotify, and - because this kernel
  silently tricks past capacity thresholds - on a 60s recheck that owns
  the idle channel while the charger is live. The recheck repaints only
  on a real band/color/mode change (fingerprint-gated) and dies the
  moment the status leaves Charging/Full.
- **notification** - two presets: `[notify]` for apps without a rule,
  `[notify.app]` for apps that have a `[rules]` color entry
- **ring** - SIM/dialer calls, incoming and outgoing
- **voip** - messenger calls (Telegram/WhatsApp/Viber/Signal)
- **missed** - missed-call LED
- **alarm** - alarm clock LED

Every one accepts any of the four renderers and its own color, timing,
current and caps. Colors, thresholds, renderer modes, chip timings,
per-channel current, caps and the suppress blacklist are all user-editable
in `led.conf` - no rebuild. The root daemon watches its config directory
with inotify and reloads on any write; the GUI's SIGALRM poke is an
explicit extra trigger feeding the same reload flag. Edits apply on the
next processed event.

### Notification pipeline

Notifications enter an in-memory priority pool (`mods/queue.c`, up to 32
entries); a single arbitrator decides who lights the LED, when, and for
how long. No per-entry timers - the pool is lazy-rechecked only on real
edges (the bridge's screen event, a cancel, or a cap deadline):

- **Selection is LIFO by recency** - the freshest post wins. A newer top
  **preempts** the current entry, which returns to the pool with its shown
  time accrued (`shown_ms`) and resumes from the leftover on its next
  turn, so alternating app traffic cannot blink forever.
- **Lifetime** is `created + [notify(.app)] notif_max_sec` (builtin
  default 1800s; the shipped `led.conf` sets 0 = unlimited). An entry that
  aged out is dropped at pick time; expiry also caps the accumulated show
  time.
- **Screen-on staging needs no timer.** A fresh top that lands on a lit
  screen parks as `Q_HOLD`. The bridge sends `SCREEN 0` the moment the
  screen falls, which flashes the park instantly - no poll. If the screen
  stays up past the grace window (`notify_screen_delay_ms`, default
  60000) the parked top is dropped - and only it. An event nobody sees on
  the lock screen can never flash an hour later.
- **Calls, alarms, missed and VoIP are untouchable planes.** While
  ring/voip/missed or alarm own the channel, the pool simply waits - it
  keeps accumulating, times still tick, and stale entries are dropped
  lazily. A notification backlog can never light the LED mid-call or
  override an alarm.
- **Cadence is reactive.** The core timer is armed to the next real
  deadline: exactly at the active entry's cap, a single `WATCHDOG_SEC`
  safety pass when there is no cap at all (`notif_max_sec=0`), and idle
  with no pool work. A 1s sysfs poll survives only as a screen-state
  fallback for when the bridge is down, bounded by the grace window.

### Notification transport

The daemon talks only to the standalone headless **NotifyBridge** app over
the abstract Unix socket `notify_bus`. The bridge config needed by the
module rides inside the zip as `module/notifybridge.json`
(`/data/local/tmp/notifybridge.json` on the device - deployed by
`customize.sh` on flash and `install_core.cmd` on dev-apply, always
overwritten like `led.conf`): the ENQ/CAN/RING/VOIP/SCREEN/PULSE rule set
plus the `notification_light_pulse` setting watcher that powers the
blink-light gate. The full rule language is configurable - see the
notify-bridge repo. The wire format the daemon expects:

```
ENQ <pkg> <id>        notification posted        -> pool push
CAN <pkg> <id>        notification removed       -> pool entry gone
CAN_ALL <pkg>         app cleared its posts
RING_ON <0|1>         SIM call (1=incoming)      -> call plane
RING_OFF              last SIM call notification
VOIP_ON <pkg>         messenger call             -> voip plane
VOIP_OFF <pkg>        messenger call notification gone
SCREEN <0|1>          screen off/on              -> park flash / grace
PULSE <0|1>           notification_light_pulse changed by any writer
```

On connect the client **replays its live state** (SCREEN, active
RING/VOIP, every active ENQ), so a daemon restart mid-call re-arms
cleanly. There is no logcat/event-log fallback - with the bridge absent,
no notification-side LED can light. Charge bands, the light-pulse gate and
the test hooks are daemon-side and unaffected.

### System gates

- **"Blink light" toggle** (Settings -> Notifications -> Blink light):
  when off, the daemon drops all notification LEDs at a single gate (call
  rainbows, alarms and charge are unaffected). The bridge watches
  `Settings.System` with a `ContentObserver` and pokes `PULSE 0` the
  moment any writer changes it, so a running notification LED goes off
  instantly - no polling. `customize.sh` turns the toggle on for fresh
  installs.

### Supervision

None. The v2.x daemon used to be kept alive by a supervisor built into
the bridge; it was removed when NotifyBridge was repackaged as a pure
transport (app v2.0.0) - chgd is no longer watched or restarted from
anywhere. If the daemon dies it stays down until reboot or a manual
start (`service.sh`). `service.sh` only guarantees the daemon is up right
after boot. No `WD` push, no `[led] watchdog_ms`, no root grant for the
app.
Set `trace_sysfs=1` in `[led]` for debug-only per-write sysfs tracing.

## LED GUI - optional configurator

The daemon + NotifyBridge work fine without it. The GUI is a Kotlin/Android
Views app (runs smooth at 120Hz on this firmware) with a follow-the-finger
swipe pager and six tabs:

- **Info** - root status (persistent explainer when su is hidden / "Open
  KernelSU Manager" button), daemon PID, bridge connect state (reads
  `notifybridge.status`) + one-tap Notification access grant, live LED swatch with the active renderer/engine and chip imax,
  test hooks (fake Telegram / incoming / outgoing / Charge cycle /
  Disarm), log tail with the `[led] logging` toggle
- **Charge** - thresholds (`[charge]` first/second) + one renderer card
  per band (LOW/MID/HIGH): mode, color, per-channel current,
  breathing/wave timing
- **Notification** - the two presets, each fully editable: **default**
  (`[notify]`, apps without a rule) and **app** (`[notify.app]`,
  rule-matched apps share this renderer; per-app colors live in the
  `pkg=r,g,b` `[rules]` list + suppressed packages, one per line)
- **Call** - two views: **in-call** (`[ring]` renderer, test-hold
  duration, max cap, color/timing) and **missed** (`[missed]` LED)
- **VoIP** - `[voip]` renderer, safety cap, messenger package list
- **Alarm** - `[alarm]` renderer, color, max duration

All changes save to `led.conf` and SIGALRM the daemon - they take effect
on the next daemon event, no restart, no rebuild. Requires root
(KernelSU). Build with `install_gui.cmd` or install the bundled
`led_gui-release.apk`.

### Screenshots

| Info | Charge |
| --- | --- |
| <img src="screenshots/01-info.png" width="270"> | <img src="screenshots/02-charge.png" width="270"> |

| Call | Alarm |
| --- | --- |
| <img src="screenshots/03-call.png" width="270"> | <img src="screenshots/04-alarm.png" width="270"> |

| Notification | Notification (alternate capture) |
| --- | --- |
| <img src="screenshots/06-notification.png" width="270"> | <img src="screenshots/07-notification.png" width="270"> |

| Alarm (alternate capture) | LED GUI |
| --- | --- |
| <img src="screenshots/05-alarm.png" width="270"> | <img src="screenshots/08.png" width="270"> |

## Build from source

Requires [Android NDK r27d](https://developer.android.com/ndk/downloads).
Edit `NDK_CC` in `led_hal_root/build.cmd` if your NDK is elsewhere, then
build the flashable module zip in one step from the repo root:

```
build_module.cmd
```

This compiles chgd, merges `led_hal_root/` + `module/` (led.conf,
service.sh, customize.sh, the two APKs, the prebuilt awctl) and writes
`release\led_hal_root-v<version>.zip`.

`led_hal_root/build.cmd` alone builds the daemon binary only: every `*.c`
plus `mods/*.c` linked against the prebuilt chip controller
(`aw2033-driver/libaw2033.a` + `aw2033.h`).

The chip sources are **not** here - rebuild `libaw2033.a` and `awctl` in
the standalone [aw2033-driver](https://github.com/nalbe/aw2033-driver)
repo and drop the artifacts into `aw2033-driver/` / `module/awctl`
(`build_module.cmd` packages `module/awctl` as-is, no rebuild). Similarly,
the NotifyBridge app builds in the standalone android-notify-bridge repo;
`install_nls.cmd` only copies its `notifybridge-release.apk` to
`module/notifybridge-release.apk`.

## Deploy

With the device connected and `adb root` working:

```
install_core.cmd
```

This rebuilds, pushes all module files, and restarts the daemon.

## Test hooks

```bash
kill -USR1 $(pidof chgd)   # fake Telegram notification (test mode, ignores screen)
kill -HUP  $(pidof chgd)   # test INCOMING call (renderer held until Disarm / [ring] max_sec)
kill -WINCH $(pidof chgd)  # test OUTGOING call (same renderer, held)
kill -QUIT  $(pidof chgd)  # cycle charge bands: lower -> middle -> upper -> none
kill -USR2  $(pidof chgd)  # "Blink light" toggle OFF: invalidate pulse cache + disarm notify LED
kill -CONT  $(pidof chgd)  # truncate /data/local/tmp/ledd.log
kill -ALRM  $(pidof chgd)  # force led.conf reload + re-apply visible state (GUI save); edits are also auto-detected via inotify
```

## Status files (world-readable, no su needed on the read side)

- `/data/local/tmp/led_status` - the live LED state: `mode`/`band`/`pkg`/
  `color=`/`engine=`, where `engine` (`off`/`solid`/`breath`/`wave`) tells
  a consumer whether the raw brightness node reflects what the eye sees -
  chip-driven patterns read a constant peak, solid reads the real color.
- `/data/local/tmp/notifybridge.status` - bridge connect state
  (`connected=1|0`), written by the daemon on accept/EOF and after every
  reload.

## The chip debugging CLI - awctl

`awctl` is the raw CLI for the AW2033 chip (probe/dump/off/solid/breathe/
wave/fade/cur/imax/freq/exp/syncmode/reg/patst/state), for hands-on LED
debugging that bypasses all daemon policy. It ships prebuilt in the
module as `module/awctl`; sources, build command and the full command
table live in the standalone [aw2033-driver](https://github.com/nalbe/aw2033-driver)
repo.

## Release history

See [PATCHNOTES.md](PATCHNOTES.md) - kept to the daemon core and the GUI
(notify-bridge and aw2033-driver keep their own changelogs).

## Architecture

```
core.c    - main loop: select() over netlink uevents, the NLS client socket
            and the adaptive one-shot timerfd; NLS command pipeline
            (ENQ/CAN/CAN_ALL, RING_ON/OFF, VOIP_ON/OFF, PULSE);
            signal test hooks; lockfile
led.c     - per-event renderer adapter: resolves [sec] mode=off|solid|breath|wave
            and programs the AW2033 chip through aw2033.h (the ONLY LED writer;
            all animation runs on-chip)
config.c  - led.conf parser + generic key-value store for mods
util.c    - logging, sysfs helpers, led_status + notifybridge.status files, screen
            detection, notification_light_pulse gate

mods/
  charge.c  - charge band eval ([charge] thresholds only) + per-band renderer
              apply; idle repaint on power_supply uevents / refresh / the
              60s charging recheck (the kernel uevent gap)
  queue.c   - notification priority pool: LIFO pick, screen-on Q_HOLD staging,
              preemption with resume-credit, lazy grace/expiry, cap accrue
  notify.c  - notification gate (suppress -> per-app color -> [notify]/
              [notify.app] renderer), owns the armed channel + pool heartbeat
  ring.c    - SIM call mode (RING_ON/RING_OFF), [ring] renderer, missed-check
              handover on call end
  dialer.c  - call_log missed-call verification -> [missed] LED
  tele.c    - child-process capture helpers (call_log query, settings get)
  voip.c    - messenger call mode (VOIP_ON/VOIP_OFF), [voip] renderer, safety cap
  alarm.c   - alarm clock LED, [alarm] renderer
```

Adding a feature = a new file under `mods/`, no core edits. Extensions use
`REGISTER_RULE` / `REGISTER_HANDLER` / `REGISTER_MODE` /
`REGISTER_MODE_WAKE` / `REGISTER_UEVENT` / `REGISTER_REFRESH` macros that
place entries into linker sections. `config.c` is a generic key-value
store: every unknown `[section] key=value` in led.conf is readable via
`conf_get_str` / `conf_get_int`, so a mod owns its own config section
without config.c knowing the key exists.