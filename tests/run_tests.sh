#!/bin/sh
# Test suite for dskdiff.
#
#   make test        generate the fixtures and run this suite
#   ./run_tests.sh   if the fixtures are already there
#   BIN=xp/dskdiff.sh ./run_tests.sh   the same cases on the XP VM
#
# Each case checks that the output contains the expected phrase.

BIN=${BIN:-../build/dskdiff}
FIX=fixtures
fail=0
n=0

check() {
    desc="$1"; expect="$2"; shift 2
    n=$((n + 1))
    out=$("$BIN" "$@" 2>&1)
    if printf '%s' "$out" | grep -q "$expect"; then
        printf '  ok   %s\n' "$desc"
    else
        printf '  FAIL %s\n' "$desc"
        printf '       expected: %s\n' "$expect"
        printf '%s\n' "$out" | sed 's/^/       | /'
        fail=1
    fi
}

absent() {
    desc="$1"; unwanted="$2"; shift 2
    n=$((n + 1))
    if "$BIN" "$@" 2>&1 | grep -q "$unwanted"; then
        printf '  FAIL %s\n' "$desc"
        fail=1
    else
        printf '  ok   %s\n' "$desc"
    fi
}

[ -x "$BIN" ] || { echo "$BIN is missing - run first: make native"; exit 2; }
[ -d "$FIX" ] || { echo "fixtures are missing - run first: python3 gen_edsk.py"; exit 2; }

echo
echo "two reads"
check "identical reads"              "IDENTICI"                    $FIX/ident_a.dsk $FIX/ident_b.dsk
check "noise in the extra cylinders" "cilindri 0-79 sono IDENTICI" $FIX/extra_a.dsk $FIX/extra_b.dsk
check "damage in the data area"      "differenze nei DATI"         $FIX/bad_a.dsk $FIX/bad_b.dsk
check "different geometries"         "dimensioni diverse"          $FIX/short_a.dsk $FIX/short_b.dsk

echo
echo "40-cylinder format"
check "720K, extra noise only"       "cilindri 0-39 sono IDENTICI" $FIX/dd40_a.dsk $FIX/dd40_b.dsk
check "720K, real damage"            "differenze nei DATI"         $FIX/dd40bad_a.dsk $FIX/dd40bad_b.dsk
check "720K single-sided"            "cilindri 0-39 sono IDENTICI" $FIX/ss40_a.dsk $FIX/ss40_b.dsk

echo
echo "three or more reads"
check "three reads, healthy disk"    "area dati e' IDENTICA"       $FIX/good_1.dsk $FIX/good_2.dsk $FIX/good_3.dsk
check "track without a majority"     "nessuna coppia concorde"     $FIX/weak_1.dsk $FIX/weak_2.dsk $FIX/weak_3.dsk
check "track with a majority"        "2 concordano"                $FIX/weak_1.dsk $FIX/weak_2.dsk $FIX/weak_3.dsk
check "growing track"                "DIMENSIONE DIVERSA"          $FIX/grow_1.dsk $FIX/grow_2.dsk $FIX/grow_3.dsk

echo
echo "regressions"
absent "no false positive on the size" "DIMENSIONE DIVERSA" \
       $FIX/weak_1.dsk $FIX/weak_2.dsk $FIX/weak_3.dsk
# noise tracks must not be counted as formatted:
# if detection breaks, 83 would show up here instead of 80
check  "detection ignores noise tracks"  "tracce formattate: 80 cilindri" \
       $FIX/extra_a.dsk $FIX/extra_b.dsk
check  "720K detection"                  "tracce formattate: 40 cilindri" \
       $FIX/dd40_a.dsk $FIX/dd40_b.dsk

echo
echo "arguments and errors"
check "no arguments"        "uso: dskdiff"
check "a single file"       "uso: dskdiff"         $FIX/ident_a.dsk
check "missing file"        "non riesco ad aprire" $FIX/missing.dsk $FIX/ident_a.dsk
printf 'x' > $FIX/_truncated.dsk
check "file too short"      "troppo corto"         $FIX/_truncated.dsk $FIX/ident_a.dsk
rm -f $FIX/_truncated.dsk
check "forced threshold -c" "differenze nei DATI"  $FIX/dd40bad_a.dsk $FIX/dd40bad_b.dsk -c 20

echo
if [ $fail -eq 0 ]; then
    echo "  $n cases, all green."
else
    echo "  $n cases, at least one FAIL."
fi
exit $fail
