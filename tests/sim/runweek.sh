#!/bin/sh
# $1 bin, $2 seed, $3 nap, $4 allapotfajl, $5 opcio (N = nincs hosszu aramszunet)
rm -f "$4" "$4.all"; n=0
while true; do "$1" 68 "$2" "$3" "$4" $5 > "$4.out" 2>&1; rc=$?; n=$((n+1)); echo "--- szakasz $n (rc $rc)" >> "$4.all"; cat "$4.out" >> "$4.all"; [ $rc -eq 42 ] || break; done
echo "szakaszok: $n"; grep -v "^  \[" "$4.out" | grep -v "^$" | tail -4
