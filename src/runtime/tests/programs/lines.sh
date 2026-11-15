# Lines on both pipes, then one 5000 bytes long on standard output.
echo "out 1"
echo "err 1" >&2
echo "out 2"
echo "err 2" >&2
head -c 5000 /dev/zero | tr '\0' x
echo
echo "out 3"
