#!/system/bin/sh
# led_hal_root v3.7.0 installer hook (KernelSU / Magisk compatible)

ui_print "- ==============================="
ui_print "- Notification LED daemon v3.7.0"
ui_print "- AW2033 breathing LED, Shark8"
ui_print "- ==============================="

ui_print "- Setting permissions..."
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh"   0 0 0755
set_perm "$MODPATH/chgd"         0 0 0755

# led.conf is the canonical config shipped with the module - always
# overwritten on install, never merged. The repo template is the source of
# truth; edits belong in the repo, not on the device.

# Ship the notification bridge with the module: the standalone NotifyBridge APK
# (headless, no GUI) makes the notification LED work even without the GUI.
# NotifyBridge is the daemon's ONLY notification transport - without it there
# is no notification LED (no logcat/event-log fallback exists). The GUI is
# installed alongside as an optional configurator; its failure is
# non-fatal, the NotifyBridge app is required.
if [ -f "$MODPATH/notifybridge-release.apk" ]; then
    ui_print "- Installing NotifyBridge (notifybridge-release.apk)..."
    # drop the pre-rename orphan packages if they somehow survived
    pm uninstall com.bastet.lednls >/dev/null 2>&1
    pm uninstall com.bastet.notybridge >/dev/null 2>&1
    pm install -r "$MODPATH/notifybridge-release.apk" >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        ui_print "- !! NotifyBridge install failed (install it manually)"
    fi
fi
if [ -f "$MODPATH/led_gui-release.apk" ]; then
    ui_print "- Installing LED GUI (led_gui-release.apk)..."
    pm install -r "$MODPATH/led_gui-release.apk" >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        ui_print "- !! LED GUI install failed (install it manually)"
    fi
fi

# The daemon honours the system "Notification light" toggle
# (Settings -> Notifications -> Blink light, Settings.System
# notification_light_pulse). Fresh installs always start with it ON so
# the LED lights out of the box; the user (or the GUI) can trip it off
# later, and the daemon stops lighting notifications immediately.
settings put system notification_light_pulse 1 >/dev/null 2>&1

# notifybridge.json - config for the NotifyBridge listener that feeds
# this daemon (socket notify_bus + the pulse-setting watcher). Canonical
# copy lives in the repo; always overwritten on install, never merged -
# same policy as led.conf.
ui_print "- Deploying notifybridge.json..."
cp -f "$MODPATH/notifybridge.json" /data/local/tmp/notifybridge.json 2>/dev/null
chmod 644 /data/local/tmp/notifybridge.json 2>/dev/null

# clean stale runtime state from previous installs (no-op, harmless)
rm -f /data/local/tmp/led_chg /data/local/tmp/led_chg.tmp
rm -f /data/local/tmp/led_status /data/local/tmp/led_status.tmp
rm -f /data/local/tmp/led_chgd.lock

ui_print "- Per-event renderers: mode=off|solid|breath|wave"
ui_print "-          for charge / notify / call / voip / missed / alarm"
ui_print "- Colors: RGB-configurable in led.conf ([rules] per app)"
ui_print "- Charge defaults: lower=breath red, middle=breath amber,"
ui_print "-          upper=solid green (thresholds 70/95)"
ui_print "- Everything editable in led.conf, no rebuild"
ui_print "- Notifications: NotifyBridge (standalone) -"
ui_print "-          works without GUI, any build type"
ui_print "- GUI editable led.conf on the fly (optional)"
ui_print "- Done! Reboot to activate."