#!/system/bin/sh
# led_hal_root: boot autostart of the LED daemon (chgd)
#
# No supervision anywhere: the NotyBridge app is a pure transport and
# restarts nothing. This script only guarantees the daemon is up right
# after boot; a crash mid-session stays down until reboot or manual
# start (service.sh again).
MODDIR=${0%/*}
LOG=/data/local/tmp/ledfix.log

echo "$(date) module service.sh started" >> $LOG

# give the system a moment to settle at boot before the daemon starts
sleep 8

/data/adb/ksu/bin/busybox setsid $MODDIR/chgd >/data/local/tmp/chgd.err 2>&1 &
echo "$(date) service.sh: chgd launched" >> $LOG