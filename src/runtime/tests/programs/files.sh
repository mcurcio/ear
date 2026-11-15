# Each file given: its mode, then its text, as it was when the program began.
for file in "$@"; do
	echo "mode $(stat -c %a "$file")"
	echo "text $(cat "$file")"
done
