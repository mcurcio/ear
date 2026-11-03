#!/bin/sh
# Waits for MOTU's plug-in to log "init complete" for the device (it has just read the settings
# store), then at once saves the whole store (one read-only GET, like the plug-in's own) and starts
# the heartbeat. Triggered by MOTU's own log line, captured in maclog.txt by the motu-log job.
D=$HOME/motu-test; TAG=${1:-arrival}
tail -n 0 -F "$D/maclog.txt" | grep -m1 "init complete" >/dev/null
t=$(date -u +%Y%m%dT%H%M%SZ)
echo "$t device init complete seen" >> "$D/arrival.log"
curl -s -m 30 -o "$D/datastore-$TAG-$t.json" -w "datastore http=%{http_code} %{size_download}B %{time_total}s\n" \
  http://localhost:1280/0001f2fffe00f732/datastore >> "$D/arrival.log" 2>&1
echo "$(date -u +%Y%m%dT%H%M%SZ) saved datastore-$TAG-$t.json" >> "$D/arrival.log"
exec "$HOME/motu/bin/motu-mac-heartbeat" "$D" 30
