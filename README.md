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
| D7 | 20 | 44 | Hangszórórelék (bal + jobb) tranzisztora |
| D8 | 8 | 7 | Hangszóróvédő: 2 optó (bal, jobb) közösítve, LOW = DC (47k felhúzó, 100 nF) |

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

## Hangszóróvédő

DC-érzékelő csatornánként (LTV814 / PC814: AC-bemenetű optó, mindkét polaritást érzékeli):

```
 erősítő kimenet ──[22k 0,25 W]──┬──────── LTV814 1
                                 │
                          47 µF bipoláris   (vagy 2×100 µF/16 V szembekötve)
                                 │
 erősítő föld ───────────────────┴──────── LTV814 2

 3V3 ──[47k]──┬── D8 ──[100 nF]── GND       D7 ──[470]──┬── B (NPN, pl. BC337)
              ├── LTV814 4 (bal)                         └─[10k]── GND
              └── LTV814 4 (jobb)            C ── hangszórórelék tekercse (+ dióda tekercsenként) ── relétáp
   LTV814 3 (mindkettő) ── ESP GND           E ── GND
```

- Szűrő: 22k × 47 µF ≈ 0,15 Hz; zene (20 Hz-en teljes kivezérlés is) nem kapcsol le, a tápra ragadt kimenet (±36 V) kb. 35–50 ms alatt jelez, a program szűrésével együtt kb. 60–70 ms alatt válnak le a hangszórók. DC-küszöb a 47k felhúzóval (és a belső felhúzóval) tipikusan kb. 3,5 V, a leggyengébb LTV814-gyel kb. 13 V (LTV-814A-val kb. 6 V).
- A két optó kimenete közösítve: bármelyik csatornán DC = LOW. A D8 C3-on indítási láb, ezért a felhúzó kötelező, és induláskor nem lehet LOW (bekapcsolatlan erősítőnél az optók nem vezetnek).
- Hangszórórelé: Finder 40.52 (8 A, DPDT). ±36 V-on egy érintkező kb. 3 A DC-t szakít meg biztosan, két sorba kötött érintkező 8 A-t (adatlap, H 40 görbe): csatornánként egy relé, a két érintkező sorban (erősítő → 14, 11 → 24, 21 → hangszóró +).
- A relé-tranzisztor bázisán 10k lehúzó: reset alatt a hangszórók biztosan le vannak választva. A 470 Ω-os bázisellenállással a BC337 két Finder tekercsét (2 × 59 mA) telítésben kapcsolja.
- **Bekapcsolás:** RL1 → 1 s → RL2 → 1 s (`SPK_DELAY_MS`) → hangszórók.
- **Kikapcsolás, hálózatkiesés, bármilyen hiba:** a hangszórók elsőként (ugyanabban a ms-ban) válnak le.
- **DC a kimeneten** (20 ms szűrés, `DC_FILTER_MS`): a hangszórók és a relék azonnal KI, hibajelzés (LED, napló, weboldal, Home Assistant). Utána csak gomb, távoli parancs vagy a trigger új éle kapcsol be; magától nem.
- **Bekapcsoláskor DC** (pl. bekapcsolási tranziens): a hangszórók legfeljebb 2 s-ig várnak (`DC_START_WAIT_MS`); ha addig nem szűnik meg, hiba.
- A rövid (20 ms alatti) tüske nem hiba. `SPEAKER_PROTECT = false`: a D7 / D8 nincs használva.

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
| `SPK_DELAY_MS` | 1 s | Az RL2 után ennyivel kapcsolnak a hangszórók |
| `RL2_MIN_ON_MS` | 2 s | Kikapcsolás előtt az RL2 legalább ennyi ideig be van húzva |
| `MIN_OFF_MS` | 5 s | Minimális kikapcsolt idő; az ez alatti kérést a program megjegyzi |
| `POWERUP_HOLD_MS` | 2 perc | Áramszünet után eddig nincs trigger, a gomb csak egyszer kapcsolhat be |
| `FAULT_LOCKOUT_MS` | 5 s | Hiba után eddig nincs bekapcsolás (ismétlődő hibánál duplázódik, legfeljebb `FAULT_LOCKOUT_MAX_MS` = 5 perc) |
| `MAINS_LOSS_MS` | 25 ms | Hálózatkiesés: a relék azonnal elengednek |

## Működés röviden

- **A minimális kikapcsolt idő alatt** a program megjegyzi a bekapcsolási kérést:
  - a gombét feltétel nélkül teljesíti;
  - a triggerét csak akkor, ha a trigger az idő végén még aktív.

  A függő kérés alatt a további gombnyomásokat figyelmen kívül hagyja.
- **Hálózatkiesés:** a relék azonnal elengednek. 5 s múlva, ha a trigger még aktív, a program újra bekapcsol lágyindítással.
  - **Sorozatos kiesés:** ha 3 kiesés jön úgy, hogy mindegyik az előzőtől számított 10 percen belül van, a program nem kapcsol vissza magától (a LED 3 s-onként kettőt villan). Gomb, weboldal, app és Home Assistant bekapcsol.
  - 10 perc kiesés nélkül a sorozat nullázódik; ha a trigger aktív, a program bekapcsol.
- **Áramszünet után:** 2 perc várakozás, utána aktív triggerre bekapcsol.
  - A trigger a várakozás alatt hatástalan.
  - Egy hosszú gombnyomás a várakozás alatt is bekapcsol (a boot utáni első 5 s-ban nyomva az 5 s leteltével), de csak egyszer: a további gombnyomás a várakozás végéig hatástalan, így gombbal addig kikapcsolni sem lehet.
  - A hátralévő időt RTC memóriában tartja, így egy watchdog-reset nem rövidíti le.
- **Hiba után:** 1 s villogás, a relék kikapcsolnak, majd 5 s tiltás.
  - A tiltás alatti gombnyomás elvész.
  - A tiltás végén aktív triggerre bekapcsol.
  - A beragadt gombot és az összehegedt relét a program elmenti, és újraindítás után is ellenőrzi.
- **Ismétlődő hiba** (beragadt gomb, zajos trigger, hibás RL1 kimenet): a tiltás minden újabb hibánál duplázódik (5, 10, 20 … s, legfeljebb 5 perc).
  - A 3. ismétlődő hibától a trigger nem kapcsol be automatikusan; a gomb és a távoli bekapcsolás működik (a LED 3 s-onként kettőt villan).
  - 10 perc nyugodt bemenet (a trigger zaja kikapcsolt relénél is figyelve) után a sorozat nullázódik; ha a trigger aktív, a program bekapcsol.
  - A szándékos 3 rövid gombnyomás és a hálózatkiesés nem számít a sorozatba.
- **Flash kímélése:** a statisztikát legfeljebb percenként menti (hálózatkiesésnél legfeljebb 30 s-onként); a halasztott mentést a program magától pótolja.

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
| `W` | Mentett hálózati beállítások törlése (vissza a `secrets.h` / AP módhoz) |

A soros kiírás külön taskban, nem blokkoló sorral működik: ha nincs csatlakoztatva gép, vagy a kimenet beragad, az üzenetek elvesznek, de a vezérlés nem áll meg, és nem okoz újraindulást.

## Hálózat: WiFi, weboldal, Home Assistant

Ki- és bekapcsolható a program elején: `#define HOME_ASSISTANT true` / `false`. `false` esetén a WiFi, a weboldal és az MQTT be sem fordul.

### WiFi és AP mód

- **Beállítások forrása:** a weboldalon mentett beállítás (NVS); ha nincs, a `secrets.h` (nem kötelező, minta: `secrets.example.h`); ha az sincs, azonnal AP mód.
- **AP mód:** ha 30 s alatt nem sikerül csatlakozni (vagy nincs beállítás), saját WiFi indul:
  - hálózat: `Erosito-XXXX`, jelszó: `erosito123` (vagy a `secrets.h`-ban megadott `AP_PASS`);
  - weboldal: `http://192.168.4.1` (a telefon általában magától megnyitja); AP módban az `erosito.local` név nem működik.
- Az AP alatt a program 30 s-onként újrapróbálja a routert (ha kliens van az AP-n, 5 percenként), így áramszünet után, amikor a router lassabban indul, magától csatlakozik. Csatlakozás után az AP leáll, amint 1 percig nincs rajta kliens.
- A beállítások mentése **nem indítja újra** az ESP-t (a relék nem engednek el): a program élőben csatlakozik újra.

### Fix IP-cím

- Megadható a `secrets.h`-ban (`STATIC_IP`, `STATIC_GW`, `STATIC_MASK`, `STATIC_DNS`), a weboldalon (AP módban is) és az iOS-appban; a mentett érték felülírja a `secrets.h`-t.
- **Üres IP-cím: DHCP.** Hibás beállításnál (rossz formátum, az átjáró nincs az alhálózatban, hálózati / szórási cím, nem összefüggő maszk) is DHCP; az ok a soros naplóban és a weboldal „IP-hiba” sorában látszik.
- Maszk üresen: 255.255.255.0; DNS üresen: az átjáró.
- Mentés után az ESP újraindulás nélkül csatlakozik újra (a relék nem engednek el); a weboldal és az iOS-app az új címre vált.
- A fix címet a router DHCP-tartományán kívül válaszd, hogy ne ütközzön más eszközzel.

### Weboldal

Normál üzemben az ESP IP-címén vagy a `http://erosito.local` címen érhető el (a soros `s` parancs kiírja az IP-t és az mDNS állapotát). Ha a core-ban nincs `ESPmDNS` (pl. 4.0.0 / ESP-IDF 6), a program beépített mDNS-válaszolót használ. Felhasználó: `admin`, jelszó: alapból az AP jelszava, a weboldalon módosítható (legalább 8 karakter).

- **Állapot** (2 s-onként frissül): be / ki, RL2, trigger, várakozás, hiba, figyelmeztetés, számlálók, optó, WiFi, IP, MQTT.
- **Vezérlés:** Bekapcsolás / Kikapcsolás (ugyanazok a szabályok, mint a gombnál), Trigger figyelés be / ki (mentve, mint a két rövid gombnyomás).
- **Beállítások:** WiFi név és jelszó, MQTT cím, felhasználó és jelszó, weboldal jelszó. Az üresen hagyott jelszómezők nem változnak; üres MQTT cím = nincs MQTT.
- **Elfelejtett jelszó:** a soros `W` parancs törli a mentett hálózati beállításokat (vissza a `secrets.h` / AP módhoz).

- **Bejelentkezés:** a weboldal belépő oldalt ad (jelszó), utána egy tartós süti 1 évig megjegyzi; áramszünet után is érvényes. Jelszócserekor a többi bejelentkezés érvénytelen lesz. Az iOS-app és más kliensek Basic jelszóval (felhasználó: `admin`) is elérik az `/api/...` címeket.
- **iPhone-on appként (Mac nélkül):** Safariban nyisd meg a `http://erosito.local` címet, lépj be, majd Megosztás → **Főképernyőhöz adás**. Saját ikonnal, teljes képernyőn indul, a belépést megjegyzi.

A weboldal sima HTTP-n működik: csak a helyi hálózaton használd.

### iOS-app

Az `ios/` mappában egy egyszerű iPhone-app van (SwiftUI): megkeresi az eszközt a helyi hálózaton (Bonjour, `_erosito._tcp`), be/ki kapcsol, trigger figyelés be/ki, állapot. Mac-en Xcode-dal, Windowsról a GitHub Actions által fordított `.ipa`-val és a Sideloadly-vel telepíthető: `ios/README.md`.

### Home Assistant (MQTT, AI-elemzés)

- **Automatikus felismerés:** az eszköz „Erősítő” néven jelenik meg: be/ki kapcsoló, trigger figyelés kapcsoló, trigger, probléma, üzemidő, számlálók, optó-értékek, WiFi jel.
- **Események** az `erosito/event` témán: indulás, be, ki, hálózatkiesés, hiba, öregedési figyelmeztetés.
- **Távoli parancsok:**
  - `erosito/power/set` `ON`: mint a hosszú gombnyomás (minimális kikapcsolt idő, hibatiltás; a 2 perces várakozás alatt egyszer, a gombbal és a weboldallal közösen); `OFF`: mindig kikapcsol (az RL2 2 s-os minimumával), és törli a függő bekapcsolást.
  - `erosito/trigger/set` `ON` / `OFF`: trigger figyelés be / ki (mentve).
- **Hangvezérlés:** a Beállítások > Hangasszisztensek > Entitások megosztása menüben oszd meg a `switch.erosito` kapcsolót; utána: „Kapcsold be az erősítőt”.
- **AI-elemzés:** a `homeassistant/erosito.yaml` csomag eseménynaplót vezet, és az Anthropic (Claude) integrációval
  - minden vasárnap 18:00-kor heti összefoglalót,
  - hiba, hálózatkiesés vagy figyelmeztetés után rövid elemzést küld értesítésként (legfeljebb 10 percenként egyet).

  Az API-kulcs csak a Home Assistantben van. A csomagban az `agent_id` értékét a saját Claude-ügynököd azonosítójára kell írni; ennek az ügynöknek ne adj Home Assistant-vezérlési jogot.

### Biztonság és táp

- A WiFi, a weboldal és az MQTT külön taskban fut, a vezérlés nem vár rájuk; hálózat vagy szerver nélkül az erősítő ugyanúgy működik.
- A WiFi adáskor ~300 mA-es áramcsúcsokat okoz. A segédtáp legalább 500 mA-es legyen, a 3,3 V-ra kerüljön 220–470 µF kondenzátor, különben az ESP újraindulhat (és áramszünetnek veszi).

## Hosszú üzem

A program hónapokig, évekig megszakítás nélkül futhat:

- minden időzítés kezdőidő + időtartam alapú, így a `millis()` 49,7 naponkénti túlcsordulása nem okoz hibát;
- a vezérlés futás közben nem foglal dinamikus memóriát (a WiFi- és MQTT-könyvtár a saját taskjában igen, a vezérléstől függetlenül);
- az NVS-be csak eseménykor és bekapcsolva óránként ír, ami a flash élettartamához képest elhanyagolható;
- **memória-őr** (belső RAM, mindkét lapon azonos határokkal; a `h` parancs kiírja az értékeket és a határokat, a weboldal és a Home Assistant is mutatja):

  | | szabad memória | legnagyobb blokk | szabad verem | teendő |
  |---|---|---|---|---|
  | figyelmeztetés | 20 kB | 8 kB | 512 B | figyelmeztetés (soros, web, Home Assistant, AI-elemzés) |
  | kritikus, 30 s-ig | 12 kB | 4 kB | 256 B | előbb a hálózat újraindul (a relék nem érintettek); ha 60 s alatt nem javul, szabályos újraindítás, amint a relék KI vannak |
  | vészhelyzet, 3 s-ig | 6 kB | 2 kB | 128 B | azonnali szabályos újraindítás: relék KI, mentés, újraindulás után (5 s) a relék visszakapcsolnak, ha be voltak |

  - Normál futásnál a szabad belső RAM kb. 150–200 kB (C3), illetve 200–280 kB (S3), így a határok messze vannak; az S3 PSRAM-ja nem számít bele (a WiFi nem használhatja).
  - Hurok ellen: 3 egymás utáni memória-újraindítás után csak figyelmeztet; 1 óra zavartalan futás nullázza a számlálót. Újraindulás után esemény megy a Home Assistantnak.
- a futásidő és az események életkora 64 bites órával számol; a Home Assistant csomag a 10 percnél régebbi eseményt (pl. hosszú MQTT-kimaradás után) nem elemzi.

## Fordítás

- Arduino-ESP32 core 3.x vagy újabb (fejlesztve: **4.0.0 / ESP-IDF 6.1**), alaplap: **XIAO_ESP32C3** vagy **XIAO_ESP32S3** (más alaplapra a program nem fordul le)
- Eszközök → **USB CDC On Boot: Enabled**
- Soros napló 115200 baud. Az `s` parancs a hálózatfigyelő optó LOW mintaszámát is kiírja (bekapcsolt állapotban normálisan kb. 28–37).
- `MAINS_CHECK = true`: optó nélkül állítsd `false`-ra.
