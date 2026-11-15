# Says "term" when SIGTERM comes, then holds until a line arrives on the FIFO named by its first argument, then exits 0:
# the moment between its stop and its end, held open for as long as the test needs it.
trap 'echo term; read line < "$1"; exit 0' TERM
echo ready
sleep 600 &
wait $!
