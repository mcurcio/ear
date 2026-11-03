#!/bin/sh
# Waits for the MOTU's NEXT fresh enumeration (a udev "add" of 07fd:0005), then files the old DEAD
# and heartbeat log as evidence and restarts motu-watch and the 10 s heartbeat. Run as root.
D=/var/lib/motu-test
stdbuf -oL udevadm monitor --kernel --subsystem-match=usb --property | perl -ne 'BEGIN{$|=1} $add=1 if /^ACTION=add/; $add=0 if /^$/; if ($add && /^PRODUCT=7fd\/5\//) { print; exit 0 }'
sleep 2
t=$(date -u +%Y%m%dT%H%M%SZ); mkdir -p "$D/evidence-$t"
mv "$D/DEAD" "$D/heartbeat.log" "$D/kernel.log" "$D/evidence-$t/" 2>/dev/null
systemctl stop motu-watch motu-heartbeat 2>/dev/null; systemctl reset-failed motu-watch motu-heartbeat 2>/dev/null
systemd-run --unit=motu-watch --quiet /usr/local/bin/motu-watch
systemd-run --unit=motu-heartbeat --quiet /usr/local/bin/motu-heartbeat 10
echo "$(date -u +%FT%TZ) re-armed after fresh enumeration; old evidence in evidence-$t"
