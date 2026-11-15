# Says "term" when SIGTERM comes, then holds until a line arrives on the FIFO named by its first argument, then exits 0:
# the moment between its stop and its end, held open for as long as the test needs it. Its standard error goes nowhere:
# the stop's SIGTERM reaches its sleep too (one process group), and whether the shell says "Terminated" for it is a race
# that would otherwise become the end's last line.
exec 2>/dev/null
trap 'echo term; read line < "$1"; exit 0' TERM
echo ready
sleep 600 &
wait $!
