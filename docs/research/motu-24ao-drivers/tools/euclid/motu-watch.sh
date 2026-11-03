#!/bin/bash
# Death watcher for the MOTU 24Ao on euclid. Event-driven: follows the kernel log and exits at the
# FIRST failure line, writing /var/lib/motu-test/DEAD with that line. Every kernel line is kept in
# kernel.log. Run as the motu-watch unit; a test step checks DEAD before and after.
# `motu-watch --test` checks the patterns against known good and bad lines and exits.
set -u
DIR=/var/lib/motu-test

# A failure is a line naming a USB device port ("usb 5-2:", "usb 5-2.1:") with a failure word, or a
# host-controller error. Known harmless lines are excluded first.
BENIGN='duplicate endpoint|clock source [0-9]+ is not valid|New USB device|Product:|Manufacturer:|SerialNumber:|new high-speed USB device|registered new interface driver|deregistering interface driver|power/level is deprecated'
FAIL='usb [0-9]+-[0-9.]+: .*(error|fail|timed out|timeout|cannot|can.t|not responding|not accepting|unable|disconnect|reset|stall|-110|-71|-32|-19|-62|-108)|(xhci_hcd|ehci-pci|ehci_hcd).*(error|ERROR|WARN|died|halt|not responding)'

is_failure() { printf '%s\n' "$1" | grep -E -v -q "$BENIGN" && printf '%s\n' "$1" | grep -E -q "$FAIL"; }

if [ "${1:-}" = "--test" ]; then
  rc=0
  while IFS='|' read -r want line; do
    if is_failure "$line"; then got=trip; else got=pass; fi
    [ "$got" = "$want" ] || { echo "WRONG ($got, want $want): $line"; rc=1; }
  done <<'EOF'
pass|usbcore: registered new interface driver snd-usb-motu
pass|usbcore: deregistering interface driver snd-usb-motu
pass|usb 5-2: clock source 1 is not valid, cannot use
pass|usb 5-2: config 1 interface 4 altsetting 1 has a duplicate endpoint with address 0x9, skipping
pass|usb 5-2: new high-speed USB device number 6 using xhci_hcd
pass|usb 5-2: Product: 24Ao
trip|usb 5-2: usbfs: USBDEVFS_CONTROL failed cmd motu-alive rqt 128 rq 0 len 2 ret -110
trip|usb 5-2: rec urb error
trip|usb 5-2: Failed to set interrupt interval: -110
trip|usb 5-2: 1:1: cannot set freq 48000 to ep 0x9
trip|usb 5-2: 1:1: usb_set_interface failed (-110)
trip|usb 5-2: USB disconnect, device number 6
trip|usb 5-2: device descriptor read/64, error -71
trip|usb 5-2: device not accepting address 5, error -71
trip|usb 5-2: reset high-speed USB device number 6 using xhci_hcd
trip|xhci_hcd 0000:00:10.0: WARN Set TR Deq Ptr cmd failed due to incorrect slot or ep state.
EOF
  [ $rc = 0 ] && echo "motu-watch patterns: all cases right"
  exit $rc
fi

mkdir -p "$DIR"
# DEAD is never cleared here: only a person, after power-cycling the MOTU and seeing it enumerate
# afresh, removes it (sudo rm /var/lib/motu-test/DEAD).
[ -e "$DIR/DEAD" ] && { echo "MOTU-WATCH: DEAD already present: $(cat "$DIR/DEAD")"; exit 1; }
exec 3< <(journalctl -k -f -n 0 -o short-iso-precise)
JPID=$!
trap 'kill $JPID 2>/dev/null' EXIT
while IFS= read -r line <&3; do
  printf '%s\n' "$line" >> "$DIR/kernel.log"
  if is_failure "$line"; then
    printf '%s\n' "$line" > "$DIR/DEAD"
    echo "MOTU-WATCH tripped: $line"
    exit 1
  fi
done
