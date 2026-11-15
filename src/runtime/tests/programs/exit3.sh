# Two lines on standard error, then exit status 3: the last line is what the end carries.
echo "first words" >&2
echo "last words" >&2
exit 3
