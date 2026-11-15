# Writes its feed ($1) three times, closing and opening it again each time.
printf one > "$1"
printf two > "$1"
printf three > "$1"
