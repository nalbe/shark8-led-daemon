#!/system/bin/sh
# led_hal_root v2.17: boot autostart of the LED daemon (chgd)
#
# Supervision moved into the standalone NLS app (com.bastet.lednls, shipped
# by customize.sh alongside the daemon): it checks chgd liveness via su
# (interval: [led] watchdog_ms in led.conf). The system rebinds that
# notification listener on its own, so it outlives any shell keepalive.
# This script only guarantees the daemon is up right after boot, before the
# app ever runs. keepalive.sh is no longer deployed.
MODDIR=${0%/*}
LOG=/data/local/tmp/ledfix.log

echo "$(date) module service.sh started" >> $LOG

# give the system a moment to settle at boot before the daemon starts
sleep 8

/data/adb/ksu/bin/busybox setsid $MODDIR/chgd >/data/local/tmp/chgd.err 2>&1 &
echo "$(date) service.sh: chgd launched" >> $LOG