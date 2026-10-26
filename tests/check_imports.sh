#!/bin/sh
# Check that the .exe files import only functions present in Windows XP
# SP3. A function missing on XP would make startup fail on the real PC
# with "entry point not found": better to find out here.
export LC_ALL=C
OBJDUMP=${OBJDUMP:-i686-w64-mingw32-objdump}
LIST=tests/xp/xp-sp3-exports.txt
[ -f "$LIST" ] || { echo "  $LIST is missing: see tests/xp/make_exports.sh"; exit 2; }
[ $# -gt 0 ] || { echo "  usage: check_imports.sh EXE..."; exit 2; }
fail=0
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
for exe in "$@"; do
    [ -f "$exe" ] || { echo "  $exe is missing"; fail=1; continue; }
    # binutils up to 2.41 prints "vma hint name", 2.45 "vma ordinal hint name"
    # (with <none> for the ordinal): take the first field that is a name
    "$OBJDUMP" -p "$exe" | awk '
        /DLL Name:/ { dll = tolower($3); next }
        /^[[:space:]]*$/ { dll = "" }           # the imports of a DLL end here
        dll && $1 ~ /^[0-9a-f]+$/ {
            for (i = 2; i <= NF; i++)
                if ($i ~ /^[A-Za-z_?@][A-Za-z0-9_?@$.]*$/) { print dll " " $i; break }
        }
    ' | sort -u > "$tmp"
    if [ ! -s "$tmp" ]; then
        echo "  $exe: no imported functions read (objdump missing or format changed?)"
        exit 2
    fi
    missing=$(comm -23 "$tmp" "$LIST")
    if [ -n "$missing" ]; then
        echo "  $exe uses functions that XP SP3 lacks:"
        echo "$missing" | sed 's/^/    /'
        fail=1
    else
        echo "  $exe: $(wc -l < "$tmp") imported functions, all present in XP SP3"
    fi
done
exit $fail
