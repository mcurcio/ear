# Starts a sleep in its own process group, says its pid, then waits for it.
sleep 600 &
echo "$!"
wait
