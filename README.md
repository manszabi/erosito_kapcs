# Erősítő bekapcsoló (XIAO ESP32-C3 / ESP32-S3)

Toroidtrafós erősítő be- és kikapcsolása gombbal vagy 12 V-os triggerrel, NTC-s lágyindítással, hálózatfigyeléssel és hibakezeléssel.

- **RL1** kapcsolja a hálózatot a trafóra a 30 Ω-os NTC-n keresztül (lágyindítás).
- **RL2** 1 s múlva rövidre zárja az NTC-t.

## Lábkiosztás

A program a XIAO D-lábneveit használja, így a bekötés **mindkét lapon azonos**; a GPIO-számot a kiválasztott alaplap határozza meg.

| Láb | C3 GPIO | S3 GPIO | Funkció |
|---|---|---|---|
| D1 | 3 | 2 | LED (tranzisztorral, HIGH = világít) |
| D2 | 4 | 3 | Gomb a GND-re (belső felhúzó) |
| D3 | 5 | 4 | Trigger optó, LOW = aktív (10k felhúzó) |
| D4 | 6 | 5 | Hálózatfigyelő optó, LOW = van hálózat (43k felhúzó) |
| D5 | 7 | 6 | RL2 tranzisztor |
| D10 | 10 | 9 | RL1 tranzisztor |

A relé-tranzisztorok bázisán legyen lehúzó ellenállás, hogy reset alatt ne kattanjanak be.

**XIAO ESP32-S3:** a WiFi-hez csatlakoztasd a mellékelt külső antennát; a két mag miatt a WiFi és a vezérlés külön magon fut. A WiFi adóteljesítménye C3-on 8,5 dBm, S3-on 15 dBm.

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

## Home Assistant (WiFi + MQTT, AI-elemzés)

Ki- és bekapcsolható a program elején: `#define HOME_ASSISTANT true` / `false`. `false` esetén a WiFi- és MQTT-kód be sem fordul.

- **Beállítás:** másold le a `secrets.example.h`-t `secrets.h` néven a sketch mappájába, és töltsd ki (WiFi, MQTT bróker címe, felhasználó, jelszó). A `secrets.h` nem kerül a gitbe; ha hiányzik, a program Home Assistant nélkül fordul.
- **Automatikus felismerés:** az eszköz „Erősítő” néven jelenik meg a Home Assistantben: kapcsoló, trigger, probléma, üzemidő, számlálók, optó-értékek, WiFi jel.
- **Események** az `erosito/event` témán: indulás, be, ki, hálózatkiesés, hiba, öregedési figyelmeztetés.
- **Távoli parancs** (`erosito/power/set`, `ON` / `OFF`):
  - `ON` úgy működik, mint a hosszú gombnyomás: minimális kikapcsolt idő, hibatiltás; a 2 perces várakozás alatt egyszer (a gombbal közösen).
  - `OFF` mindig kikapcsol (az RL2 2 s-os minimumával), és törli a függő bekapcsolást.
- **Hangvezérlés:** a Beállítások > Hangasszisztensek > Entitások megosztása menüben oszd meg a `switch.erosito` kapcsolót; utána: „Kapcsold be az erősítőt”.
- **AI-elemzés:** a `homeassistant/erosito.yaml` csomag eseménynaplót vezet, és az Anthropic (Claude) integrációval
  - minden vasárnap 18:00-kor heti összefoglalót,
  - hiba, hálózatkiesés vagy figyelmeztetés után rövid elemzést küld értesítésként (legfeljebb 10 percenként egyet).

  Az API-kulcs csak a Home Assistantben van. A csomagban az `agent_id` értékét a saját Claude-ügynököd azonosítójára kell írni; ennek az ügynöknek ne adj Home Assistant-vezérlési jogot.
- **Biztonság:** a WiFi és az MQTT külön taskban fut, a vezérlés nem vár rájuk; hálózat vagy szerver nélkül az erősítő ugyanúgy működik.
- **Táp:** a WiFi adáskor ~300 mA-es áramcsúcsokat okoz. A segédtáp legalább 500 mA-es legyen, a 3,3 V-ra kerüljön 220–470 µF kondenzátor, különben az ESP újraindulhat (és áramszünetnek veszi). Az adóteljesítmény csökkentve van (8,5 dBm).

## Hosszú üzem

A program hónapokig, évekig megszakítás nélkül futhat:

- minden időzítés kezdőidő + időtartam alapú, így a `millis()` 49,7 naponkénti túlcsordulása nem okoz hibát;
- a vezérlés futás közben nem foglal dinamikus memóriát (a WiFi- és MQTT-könyvtár a saját taskjában igen, a vezérléstől függetlenül);
- az NVS-be csak eseménykor és bekapcsolva óránként ír, ami a flash élettartamához képest elhanyagolható.

## Fordítás

- Arduino-ESP32 core 3.x vagy újabb (fejlesztve: **4.0.0 / ESP-IDF 6.1**), alaplap: **XIAO_ESP32C3** vagy **XIAO_ESP32S3** (más alaplapra a program nem fordul le)
- Eszközök → **USB CDC On Boot: Enabled**
- Soros napló 115200 baud. Az `s` parancs a hálózatfigyelő optó LOW mintaszámát is kiírja (bekapcsolt állapotban normálisan kb. 28–37).
- `MAINS_CHECK = true`: optó nélkül állítsd `false`-ra.
