# Ignores SIGTERM, as does the sleep it starts (an ignored signal stays ignored in a child); says the sleep's pid, then
# waits for it.
trap '' TERM
sleep 600 &
echo "$!"
wait
