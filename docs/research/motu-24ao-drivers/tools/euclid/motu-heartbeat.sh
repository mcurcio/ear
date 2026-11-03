#!/bin/sh
# USB heartbeat for the MOTU 24Ao on euclid (owner, 2026-11-02: "The heartbeat idea makes sense").
# Every INTERVAL seconds runs motu-alive once (GET_STATUS + GET_DESCRIPTOR on endpoint 0); logs each
# result with UTC time to heartbeat.log; the FIRST failure writes DEAD and exits. Never clears DEAD.
D=/var/lib/motu-test; INTERVAL=${1:-10}
[ -e "$D/DEAD" ] && { echo "DEAD already present"; exit 1; }
while :; do
  t=$(date -u +%Y-%m-%dT%H:%M:%SZ); out=$(/usr/local/bin/motu-alive 2>&1)
  case "$out" in
    ALIVE*) echo "$t ok $out" >> "$D/heartbeat.log" ;;
    *) echo "$t FAIL $out" >> "$D/heartbeat.log"; echo "$t heartbeat failed: $out" > "$D/DEAD"; exit 1 ;;
  esac
  sleep "$INTERVAL"
done
