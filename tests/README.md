# Tesztek (ESP nélkül, a gépen)

A firmware-t (`erosito_bekapcs_trigg.ino`) egy szimulátor futtatja: az Arduino / ESP-IDF függvényeket egyszerű pótlások (`stub/`) helyettesítik, az idő ezredmásodpercenként lép, az időtípusok 32 bitesek, mint az ESP-n (a `millis()` 49,7 napnál túlcsordul). A relék, a LED, a bemenetek, az opto-jelek, a hálózat (WiFi, weboldal, MQTT), az NVS és az RTC-memória is szimulált.

## Futtatás

Linuxon (vagy Windowson WSL-ben), a repó gyökeréből:

```bash
tests/run_all.sh
```

Gyors változat, kb. 2–5 perc. A teljes (24 órás terhelés, 4 hetes és 55 napos szimuláció, kb. 30–60 perc):

```bash
FULL=1 tests/run_all.sh
```

Kell hozzá: `g++` (C++17), `sed`, `md5sum`. A kimenetek a `tests/build/` mappába kerülnek (nincs a gitben). A végén: „MINDEN TESZT RENDBEN”, különben a hibás lépés neve.

## Mit ellenőriz

1. **Fordítás** mind a 8 változatban (C3 / S3 × Home Assistant be / ki × ESPmDNS van / nincs), `-Wall -Wextra`, figyelmeztetés nélkül.
2. **Alap-forgatókönyvek (19 db):** bekapcsolás, RL2 késleltetés, minimális kikapcsolt idő, hálózatkiesés, áramszünet utáni várakozás, hibák; a relék időzítése egyezik a várttal (`sim/expected_baseline.txt`).
3. **Hálózat (20–28):** WiFi / AP mód, weboldal, bejelentkezés, MQTT, fix IP.
4. **Célzott tesztek:** sorozatos hálózatkiesés, memória-őr (6 eset), hangszóróvédő, erősítő-tápfigyelés, mentett DC / táp hiba (újraindulás után és az áramszünet utáni várakozás alatt is), fix IP ellenőrzés (21 eset).
5. **Tartós hiba 24 óráig:** beragadt gomb, zajos trigger, 10 s-onkénti hálózatkiesés: NVS-írások és relékapcsolások száma.
6. **Terheléses teszt:** véletlen gombnyomások (pergéssel, beragadással), trigger (zajjal), hálózati zavarok, DC- és táptüskék, távoli parancsok.
7. **Memória:** napi heap-mérés app-lekérdezéssel (2 s-onként), weboldallal, mentésekkel, WiFi- és MQTT-kiesésekkel.
8. **Többhetes valósághű szimuláció:** napi 1–3 zenehallgatás (trigger / gomb / app), rövid és hosszú áramszünetek (az ESP valóban újraindul: az NVS megmarad, a RAM nem), watchdog-újraindulás, 1–3 napig fennálló DC- és táphibák gombos nyugtázással, a trigger kézi visszakapcsolása. A `FULL=1` változatban 55 nap újraindulás nélkül is (a `millis()` túlcsordul).

A 6. és 8. pontban ezredmásodpercenként ellenőrzött szabályok (bármelyik megsértése hiba):

- az RL2 soha nem aktív RL1 nélkül; legkorábban 1 s-mal az RL1 után, csak jelen lévő hálózatnál;
- a minimális kikapcsolt idő (5 s) és az RL2 2 s-os minimuma (hiba nélküli kikapcsolásnál) megmarad;
- hálózatkiesésnél az RL2 40 ms-on, az RL1 300 ms-on belül elenged;
- a hangszórók csak RL2 + 1 s után, DC nélkül, rendben lévő erősítő-táppal, hiba nélkül kapcsolnak; rövid (szűrés alatti) tüske nem hiba;
- hiba utáni tiltás alatt, illetve az áramszünet utáni várakozás alatt kézi kérés nélkül nincs bekapcsolás;
- mentett DC / táp hiba mellett a fizikai gomb nélkül soha nincs bekapcsolás;
- a LED követi a reléket, hibánál villog, a különleges állapotokat (várakozás, szünetelő automatika, mentett hiba) jelzi.

## Egyes tesztek kézzel

```bash
cd tests/build
./sim_C3_true 67           # mentett DC / táp hiba (célzott)
./sim_S3_false 60 11 24 1   # terhelés: seed 11, 24 óra, áramszünet utáni indulás
./runweek.sh ./sim_C3_false 101 28 het.st   # 4 hét; a teljes napló: het.st.all
LOUD=1 ./sim_C3_true 65     # a firmware soros naplójával együtt
```

A szimulátor a valódi hardvert nem helyettesíti: az opto jelalakját, a relék pergését, a zavarokat és a valódi WiFi-könyvtár memóriahasználatát csak az ESP-n lehet ellenőrizni.
