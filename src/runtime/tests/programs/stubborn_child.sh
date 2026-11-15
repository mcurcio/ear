# Starts a sleep that ignores SIGTERM, says its pid, then waits for it; this script itself ends on SIGTERM.
(trap '' TERM; exec sleep 600) &
echo "$!"
wait
