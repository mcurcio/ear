# Ignores SIGTERM (and so does the sleep it becomes), says it is ready, then waits.
trap '' TERM
echo ready
exec sleep 600
