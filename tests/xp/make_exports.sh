#!/bin/sh
# Extract from the XP SP3 DLLs (copied with xpvm.ps1 pull into
# tests/xp/dll) the list of exported functions, one "dll function" line
# each, as tests/check_imports.sh reads it. Rerun only if the list of
# DLLs changes.
export LC_ALL=C
OBJDUMP=${OBJDUMP:-i686-w64-mingw32-objdump}
here=$(dirname "$0")
for f in "$here"/dll/*.dll; do
    dll=$(basename "$f" | tr 'A-Z' 'a-z')
    "$OBJDUMP" -p "$f" | awk -v dll="$dll" '
        /^\[Ordinal\/Name Pointer\] Table/ { on = 1; next }
        on && /^[[:space:]]*\[[[:space:]]*[0-9]+\]/ { print dll " " $NF; next }
        on && NF == 0 { on = 0 }'
done | sort -u > "$here/xp-sp3-exports.txt"
echo "  $(wc -l < "$here/xp-sp3-exports.txt") exported functions in $here/xp-sp3-exports.txt"
