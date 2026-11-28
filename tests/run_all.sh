#!/bin/sh
# Az erősítő-bekapcsoló firmware tesztjei a gépen (ESP nélkül): fordítás, forgatókönyvek, terhelés, többhetes szimuláció.
# Használat:  tests/run_all.sh            (gyors, kb. 5-10 perc)
#             FULL=1 tests/run_all.sh     (teljes: 24 órás terhelés, 4 hetes és 55 napos szimuláció; kb. 30-60 perc)
# Kell: g++ (C++17), sh, sed, md5sum.
set -u
T=$(cd "$(dirname "$0")" && pwd); ROOT=$(dirname "$T"); INO="$ROOT/erosito_bekapcs_trigg.ino"
B="$T/build"; mkdir -p "$B"; cd "$B" || exit 1
cp "$T/sim/sim.cpp" "$T/sim/week.inc" "$T/sim/mk32.sh" "$T/sim/runweek.sh" "$B/"
cp "$T/sim/test_secrets.h" "$B/secrets.h"; cp "$ROOT/icon_png.h" "$B/"
chmod +x mk32.sh runweek.sh
FULL=${FULL:-0}; FAIL=0
ok()  { echo "  OK    $*"; }
bad() { echo "  HIBA  $*"; FAIL=1; }
CXX="g++ -std=gnu++17"

echo "== 1. Fordítás (C3 / S3 x Home Assistant be / ki x ESPmDNS van / nincs), -Wall -Wextra"
for board in C3 S3; do for ha in false true; do for md in nincs van; do
  F=""; [ $board = S3 ] && F="-DSIM_S3"; I="-I$T/stub"; [ $md = van ] && I="-I$T/stub_mdns -I$T/stub"
  sed "s/#define HOME_ASSISTANT true/#define HOME_ASSISTANT $ha/" "$INO" > cur.ino
  n=$($CXX -fsyntax-only -Wall -Wextra $F -x c++ -include Arduino.h $I -I. cur.ino 2>&1 | grep -c 'error\|warning')
  [ "$n" = 0 ] && ok "$board HA=$ha mDNS=$md" || bad "$board HA=$ha mDNS=$md: $n hiba / figyelmeztetes"
done; done; done

echo "== 2. Szimulátor fordítása (32 bites időtípusokkal, mint az ESP-n)"
for board in C3 S3; do for ha in false true; do
  F=""; [ $board = S3 ] && F="-DSIM_S3"
  sed "s/#define HOME_ASSISTANT true/#define HOME_ASSISTANT $ha/" "$INO" > cur.ino && ./mk32.sh cur.ino
  if $CXX -O2 -w $F -I"$T/stub_mdns" -I"$T/stub" -I. sim.cpp -o sim_${board}_$ha 2> build_err.txt; then ok "sim_${board}_$ha"; else bad "sim_${board}_$ha"; head -5 build_err.txt; fi
done; done

echo "== 3. Alap-forgatókönyvek (19 db): a relék időzítése egyezik a várttal"
for v in C3_false C3_true S3_false S3_true; do
  for i in $(seq 1 19); do echo "S$i: $(./sim_$v $i | grep -o '\] >>> HW: RL1=. RL2=.\|^  \[ *[0-9.]*\] >>> HW' | wc -l) $(./sim_$v $i | grep -o '\[ *[0-9.]*\] >>> HW: RL1=. RL2=.' | md5sum | cut -c1-8)"; done > base_$v.txt
  d=$(diff "$T/sim/expected_baseline.txt" base_$v.txt | grep -c '^>'); [ "$d" = 0 ] && ok "$v" || bad "$v: $d eltérő forgatókönyv"
done

echo "== 4. Hálózat, weboldal, MQTT (20-28, Home Assistanttal)"
for v in C3_true S3_true; do for i in $(seq 20 28); do ./sim_$v $i > net_${v}_$i.txt 2>&1 || bad "$v S$i (kilépési kód)"; done; ok "$v S20-S28 lefutott (kimenet: tests/build/net_*.txt)"; done

echo "== 5. Célzott tesztek"
for v in C3_false C3_true S3_false S3_true; do
  for t in "62" "64 A" "64 B" "64 C" "64 D" "64 E" "64 F" "65" "66" "67" "67 R" "67 H" "69"; do
    r=$(./sim_$v $t 2>&1 | tail -1); case "$r" in *RENDBEN*) ;; *) bad "$v $t: $r";; esac
  done; ok "$v: sorozatos kiesés, memória-őr, hangszóróvédő, tápfigyelés, mentett súlyos hiba, gomb (5-8 s: hálózat törlés)"
done
sed -n '/IPCFG_PURE_BEGIN/,/IPCFG_PURE_END/p' "$INO" > ip_pure.inc
$CXX -O2 -w "$T/sim/iptest.cpp" -I. -o iptest && r=$(./iptest | tail -1) && case "$r" in *helyes*) ok "fix IP ellenőrzés: $r";; *) bad "fix IP: $r";; esac

echo "== 6. Tartós hiba 24 óráig (NVS-írás, relékapcsolás)"
for m in 1 2 3; do echo "  $(./sim_C3_false 61 $m | grep '^begin\|^mod')"; done

echo "== 7. Terheléses teszt (véletlen ingerek, invariánsok ms-onként)"
H=1; SEEDS="11 22"; [ "$FULL" = 1 ] && H=24 && SEEDS="11 22 33 44 55"
for v in C3_false C3_true S3_false S3_true; do for rr in 1 2 7; do for s in $SEEDS; do
  ./sim_$v 60 $s $H $rr > stress_${v}_${s}_$rr.txt 2>&1
  n=$(grep -o "sertes [0-9]*" stress_${v}_${s}_$rr.txt | head -1 | cut -d' ' -f2); [ "$n" = 0 ] || bad "terhelés $v seed $s reset $rr: $n sértés"
done; done; done; ok "terhelés: ${H} óra x seedek ($SEEDS) x 3 reset-ok x 4 változat"

echo "== 8. Memória (napi heap-mérés app-lekérdezéssel, WiFi / MQTT kiesésekkel)"
DAYS=3; [ "$FULL" = 1 ] && DAYS=30
r=$(./sim_C3_true 63 $DAYS | tail -1); case "$r" in *"NEM NO"*) ok "$r";; *) bad "$r";; esac

echo "== 9. Többhetes valósághű szimuláció (áramszünetek ESP-újraindulással, DC / táp hibák, nyugtázás)"
WD=7; [ "$FULL" = 1 ] && WD=28
for v in C3_false S3_true; do
  ./runweek.sh ./sim_$v 101 $WD week_$v.st > week_$v.res 2>&1
  n=$(grep -o "sertes [0-9]*" week_$v.st.out | head -1 | cut -d' ' -f2); [ "$n" = 0 ] && ok "$v: $WD nap, 0 sértés" || bad "$v: $WD nap, $n sértés (week_$v.st.all)"
done
if [ "$FULL" = 1 ]; then
  ./runweek.sh ./sim_C3_true 404 55 week55.st N > week55.res 2>&1
  n=$(grep -o "sertes [0-9]*" week55.st.out | head -1 | cut -d' ' -f2); [ "$n" = 0 ] && ok "55 nap újraindulás nélkül (millis()-túlcsordulás), 0 sértés" || bad "55 nap: $n sértés"
fi

echo; [ $FAIL = 0 ] && echo "MINDEN TESZT RENDBEN" || echo "VAN HIBÁS TESZT (részletek fent és a tests/build mappában)"
exit $FAIL
