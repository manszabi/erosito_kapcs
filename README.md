# Erősítő bekapcsoló (XIAO ESP32-C3)

Toroidtrafós erősítő be- és kikapcsolása gombbal vagy 12 V-os triggerrel, NTC-s lágyindítással, hálózatfigyeléssel és hibakezeléssel.

- **RL1** kapcsolja a hálózatot a trafóra a 30 Ω-os NTC-n keresztül (lágyindítás).
- **RL2** 1 s múlva rövidre zárja az NTC-t.

## Lábkiosztás

| Láb | GPIO | Funkció |
|---|---|---|
| D1 | 3 | LED (tranzisztorral, HIGH = világít) |
| D2 | 4 | Gomb a GND-re (belső felhúzó) |
| D3 | 5 | Trigger optó, LOW = aktív (10k felhúzó) |
| D4 | 6 | Hálózatfigyelő optó, LOW = van hálózat (43k felhúzó) |
| D5 | 7 | RL2 tranzisztor |
| D10 | 10 | RL1 tranzisztor |

A relé-tranzisztorok bázisán legyen lehúzó ellenállás, hogy reset alatt ne kattanjanak be.

## Hálózatfigyelő optó

```
 RL1 kimenet (L) ──[43k]──[43k]── DB106 ~
 (az NTC elé)                     DB106 ~ ──[43k]── N
                                  DB106 + ── CNY17-3 1 (anód)
                                  DB106 − ── CNY17-3 2 (katód)

 3V3 ──[43k]──┬── D4
              └── CNY17-3 5 (kollektor)
                  CNY17-3 4 (emitter) ── GND, 6 (bázis) szabadon
```

Az ellenállások 0,6 W-osak. A hálózati és a 3,3 V-os oldal között legalább 6 mm legyen.

## Kezelés

- **Gomb, hosszú nyomás (≥ 0,4 s):** be / ki.
- **Gomb, 2 rövid nyomás:** a trigger tiltása vagy engedélyezése (megmarad újraindítás után is).
- **Gomb, 3+ rövid nyomás:** hibának számít.
- **Trigger:** aktív élre be, inaktív élre ki.
- **LED:**
  - világít = bekapcsolva;
  - gyors villogás = hiba;
  - 3 s-onként felvillan = áramszünet utáni várakozás;
  - 2 villanás = a trigger tiltva (kézi bekapcsoláskor és tiltáskor);
  - 4 villanás = a trigger engedélyezve.

## Időzítések

| Beállítás | Érték | Jelentés |
|---|---|---|
| `RL2_DELAY_MS` | 1 s | Lágyindítás: az RL1 után ennyivel húz be az RL2 |
| `RL2_MIN_ON_MS` | 2 s | Kikapcsolás előtt az RL2 legalább ennyi ideig be van húzva |
| `MIN_OFF_MS` | 5 s | Minimális kikapcsolt idő; az ez alatti kérést a program megjegyzi |
| `POWERUP_HOLD_MS` | 2 perc | Áramszünet után eddig nincs trigger, a gomb csak egyszer kapcsolhat be |
| `FAULT_LOCKOUT_MS` | 5 s | Hiba után eddig nincs bekapcsolás |
| `MAINS_LOSS_MS` | 25 ms | Hálózatkiesés: a relék azonnal elengednek |

## Működés röviden

- **A minimális kikapcsolt idő alatt** a program megjegyzi a bekapcsolási kérést:
  - a gombét feltétel nélkül teljesíti;
  - a triggerét csak akkor, ha a trigger az idő végén még aktív.

  A függő kérés alatt a további gombnyomásokat figyelmen kívül hagyja.
- **Hálózatkiesés:** a relék azonnal elengednek. 5 s múlva, ha a trigger még aktív, a program újra bekapcsol lágyindítással.
- **Áramszünet után:** 2 perc várakozás, utána aktív triggerre bekapcsol.
  - A trigger a várakozás alatt hatástalan.
  - Egy hosszú gombnyomás a várakozás alatt is bekapcsol (a boot utáni első 5 s-ban nyomva az 5 s leteltével), de csak egyszer: a további gombnyomás a várakozás végéig hatástalan, így gombbal addig kikapcsolni sem lehet.
  - A hátralévő időt RTC memóriában tartja, így egy watchdog-reset nem rövidíti le.
- **Hiba után:** 1 s villogás, a relék kikapcsolnak, majd 5 s tiltás.
  - A tiltás alatti gombnyomás elvész.
  - A tiltás végén aktív triggerre bekapcsol.
  - A beragadt gombot és az összehegedt relét a program elmenti, és újraindítás után is ellenőrzi.

## Állapotfigyelés

A program az NVS-be menti (bekapcsoláskor, kikapcsoláskor és bekapcsolva óránként):

- üzemidő, bekapcsolások száma, RL2 behúzások;
- relé-kapcsolások száma a legutóbbi relécsere óta;
- hálózatkiesések, hibák, áramszünet utáni indulások;
- az optó LOW mintaszámának hosszú távú átlaga és alapértéke (az első 5, legalább 10 s-os bekapcsolás átlaga).

**Figyelmeztetés** (indításkor és az `s` parancsnál is megjelenik):

- **relé kopik:** a kapcsolások száma eléri a `RELAY_LIFE` (100 000) 80%-át;
- **optó öregszik:** az átlag 25%-kal az alapérték alá esik;
- **optó jele gyenge:** az átlag 15 alá esik (a hibahatár 8).

**Soros parancsok** (indításkor a program kiírja őket, és bármely más karakterre is):

| Parancs | Hatás |
|---|---|
| `s` | Állapot |
| `h` | Állapotfigyelés részletesen |
| `B` | Optócsere után: új alapérték a következő 5 bekapcsolásból |
| `R` | Relécsere után: a relé-számláló nullázása |

A soros kiírás külön taskban, nem blokkoló sorral működik: ha nincs csatlakoztatva gép, vagy a kimenet beragad, az üzenetek elvesznek, de a vezérlés nem áll meg, és nem okoz újraindulást.

## Fordítás

- Arduino-ESP32 core 3.x, alaplap: **XIAO_ESP32C3**
- Eszközök → **USB CDC On Boot: Enabled**
- Soros napló 115200 baud. Az `s` parancs a hálózatfigyelő optó LOW mintaszámát is kiírja (bekapcsolt állapotban normálisan kb. 28–37).
- `MAINS_CHECK = true`: optó nélkül állítsd `false`-ra.
