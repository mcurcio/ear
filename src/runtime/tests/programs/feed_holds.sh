# Writes "live" to its feed ($1), then holds until a line arrives on the FIFO named by $2, then exits 0: its feed's bytes
# can be told only while it runs.
printf live > "$1"
read line < "$2"
