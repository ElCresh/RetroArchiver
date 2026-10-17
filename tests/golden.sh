#!/bin/sh
# Exact comparison of dskdiff's output with the one recorded before the
# comparison was moved to core/compare.c.
#
#   ./golden.sh record   record (done once, with the old dskdiff)
#   ./golden.sh          compare
BIN=${BIN:-../build/dskdiff}
FIX=fixtures
DIR=golden
mode=${1:-check}
fail=0

case_() {
    name="$1"; shift
    out=$("$BIN" "$@" 2>&1; echo "exit=$?")
    if [ "$mode" = record ]; then
        printf '%s\n' "$out" > "$DIR/$name.txt"
    elif [ "$(cat "$DIR/$name.txt")" != "$out" ]; then
        echo "  FAIL golden $name"
        printf '%s\n' "$out" | diff "$DIR/$name.txt" - | sed 's/^/       /'
        fail=1
    fi
}

mkdir -p "$DIR"
head -c 300 /dev/zero | tr '\0' 'x' > $FIX/_nohdr.dsk
printf 'x' > $FIX/_truncated.dsk

case_ ident    $FIX/ident_a.dsk $FIX/ident_b.dsk
case_ extra    $FIX/extra_a.dsk $FIX/extra_b.dsk
case_ bad      $FIX/bad_a.dsk $FIX/bad_b.dsk
case_ short    $FIX/short_a.dsk $FIX/short_b.dsk
case_ dd40     $FIX/dd40_a.dsk $FIX/dd40_b.dsk
case_ dd40bad  $FIX/dd40bad_a.dsk $FIX/dd40bad_b.dsk
case_ ss40     $FIX/ss40_a.dsk $FIX/ss40_b.dsk
case_ good     $FIX/good_1.dsk $FIX/good_2.dsk $FIX/good_3.dsk
case_ weak     $FIX/weak_1.dsk $FIX/weak_2.dsk $FIX/weak_3.dsk
case_ grow     $FIX/grow_1.dsk $FIX/grow_2.dsk $FIX/grow_3.dsk
case_ forced   $FIX/dd40bad_a.dsk $FIX/dd40bad_b.dsk -c 20
case_ noargs
case_ onefile  $FIX/ident_a.dsk
case_ missing  $FIX/missing.dsk $FIX/ident_a.dsk
case_ truncated  $FIX/_truncated.dsk $FIX/ident_a.dsk
case_ nohdr2   $FIX/_nohdr.dsk $FIX/ident_a.dsk
case_ nohdr3   $FIX/_nohdr.dsk $FIX/ident_a.dsk $FIX/ident_b.dsk
case_ nohdrb   $FIX/ident_a.dsk $FIX/_nohdr.dsk

rm -f $FIX/_nohdr.dsk $FIX/_truncated.dsk
if [ "$mode" = record ]; then
    echo "  golden files recorded in $DIR"
elif [ $fail -eq 0 ]; then
    echo "  golden: dskdiff output same as before"
fi
exit $fail
