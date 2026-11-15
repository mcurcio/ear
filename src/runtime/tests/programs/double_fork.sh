# Leaves an orphan behind: a shell started in the background by a subshell that exits at once, as this script then
# does. The orphan leaves the program's process group (setsid), so the SIGKILL its group gets when this script is reaped
# does not reach it. Its pid is said by this script, once the orphan has given it and let go of the pipe; the orphan
# then becomes a sleep that ends on its own.
pid=$( (setsid sh -c 'echo "$$"; exec sleep 2 >/dev/null' &) )
echo "$pid"
exit 0
