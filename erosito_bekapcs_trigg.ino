// XIAO ESP32-C3 relé szekvenszer: gomb + 12 V trigger, RL1 azonnal, RL2 késleltetve, hálózat-visszajelzés, hibakezelés
// Gomb: hosszú nyomás = be/ki; 2 rövid nyomás = trigger tiltás/engedés (NVS); 3+ rövid nyomás = hiba
// Trigger: aktív élre be, inaktív élre ki; ha az RL2 előtt elmegy, a teljes RL1-RL2 ciklus lefut, utána ki
// LED: be = világít; hiba = FAULT_BLINK_MS villogás, utána minden KI; trigger tiltva + kézi bekapcsolás = 2 villanás
// Hiba után FAULT_LOCKOUT_MS ideig nincs bekapcsolás; utána beragadt gomb = újabb hiba, különben aktív triggerre be (AUTO_ON_AFTER_FAULT)
// Hardveres hibát (beragadt gomb, hálózat hiánya / KI relénél) az NVS tárolja: reset után az ok megszűnéséig nem kapcsol be magától
// Hálózat-visszajelzés (MAINS_CHECK): H11AA1M a relék kimenetén (LOW = van hálózat); az RL2 csak az RL1 visszajelzése után húz be
// Opto: 3 x 22k sorosan a hálózat felé, kimenet 47k felhúzóval a 3V3-ra; az 's' parancs a LOW mintaszámot is kiírja (~28-37 normális)
// Boot alatt a relék KI; aktív triggerre induláskor bekapcsol (TRIGGER_ON_BOOT, BROWNOUT_IS_CRASH); watchdog: WDT_MS
// Soros 115200, 's' = állapot; kötelező az "USB CDC On Boot: Enabled"; core: Arduino-ESP32 3.x / 4.x, board: XIAO_ESP32C3

#include <Arduino.h>
#include <Preferences.h>
#include <stdio.h>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_task_wdt.h>
#include <esp_system.h>

// Core 3.x / 4.x kell (a verzió-makró az Arduino.h-ból jön, ezért az include-ok után ellenőrzünk)
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || (ESP_ARDUINO_VERSION_MAJOR < 3)
#error "Arduino-ESP32 core 3.x kell (Eszkozok > Alaplap > Boards Manager: esp32 by Espressif Systems, 3.x)"
#endif
// USB CDC nélkül a Serial a GPIO20/21 UART-ra kerülne, ami ütközik az RL2 visszajelzővel
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || (ARDUINO_USB_CDC_ON_BOOT == 0)
#warning "Allitsd be: Eszkozok > USB CDC On Boot > Enabled (kulonben a Serial a GPIO20/21 UART-ra kerul, ami utkozik az RL2 visszajelzovel)"
#endif

// ---- Lábak (GPIO számok; kerülendő: GPIO2/8/9 strapping, GPIO21 UART TX) ----
const int PIN_LED  = 3;   // D1:  2N2222 bázis 1k-n át, 4,7k lehúzó (HIGH = világít)
const int PIN_BTN  = 4;   // D2:  gomb -> GND (belső felhúzó)
const int PIN_TRIG = 5;   // D3:  opto kimenet, LOW = aktív, külső 10k felhúzó 3V3-ra
const int PIN_RL1  = 10;  // D10: RL1 tranzisztor
const int PIN_RL2  = 7;   // D5:  RL2 tranzisztor
const int PIN_FB1  = 6;   // D4:  RL1 kimenet opto, LOW = van hálózat, 47k felhúzó 3V3-ra
const int PIN_FB2  = 20;  // D7:  RL2 kimenet opto (U0RXD: csak USB CDC On Boot mellett szabad)

// ---- Beállítások ----
const unsigned long RL2_DELAY_MS    = 400;    // RL2 késleltetése
const unsigned long DEBOUNCE_MS     = 30;     // bemenet szűrés
const uint32_t      WDT_MS          = 3000;   // watchdog időkorlát

const unsigned long LONG_PRESS_MS   = 400;    // hosszú nyomás határa
const unsigned long DOUBLE_GAP_MS   = 800;    // rövid nyomások közti max. szünet; ennyi csend után értékel
const uint8_t       SHORT_FAULT_COUNT = 3;    // ennyi rövid nyomás = hiba
const unsigned long FLASH_ON_MS     = 150;    // villanás: világít
const unsigned long FLASH_OFF_MS    = 150;    // villanás: sötét
const unsigned long FAULT_BLINK_MS  = 1000;   // hibavillogás hossza (közben a relék BE), utána minden KI
const unsigned long FAULT_HALF_MS   = 100;    // hibavillogás fél periódusa

const bool TRIGGER_ON_BOOT      = true;    // true: induláskor aktív triggerre bekapcsol
const bool BROWNOUT_IS_CRASH    = true;    // true: a brownout reset is fagyásnak számít

const unsigned long FAULT_LOCKOUT_MS = 5000; // hiba után ennyi ideig nincs bekapcsolás
const bool AUTO_ON_AFTER_FAULT  = true;    // true: a tiltás végén aktív triggerre bekapcsol
const unsigned long RL2_MIN_ON_MS = 100;   // teljes ciklus: az RL2 legalább ennyi ideig behúzva marad

// ---- Hálózati feszültség visszajelzés ----
const bool          MAINS_CHECK      = false;   // false: nincs hálózat-visszajelzés (opto nélkül ezt használd)
const unsigned long MAINS_WIN_MS     = 40;     // mérési ablak
const uint8_t       MAINS_MIN_LOW    = 8;      // ennyi LOW minta / ablak = van hálózat (~1 kHz mintavétel)
const unsigned long MAINS_FAULT_MS   = 150;    // ennyi ideig tartó eltérés = hiba
const unsigned long MAINS_OFF_GRACE_MS = 1000; // elengedés után ennyi ideig nem hiba a maradék feszültség

// ---- Diagnosztika ----
const unsigned long BTN_STUCK_MS = 10000;  // ennyi ideig nyomva tartott gomb = hiba
const unsigned long WINDOW_MS    = 1000;   // váltásszámláló ablak
const uint8_t       CHANGE_LIMIT = 4;      // trigger: ennyinél több váltás / ablak = hiba
const uint8_t       BTN_CHANGE_LIMIT = 8;  // gomb: ennyinél több váltás / ablak = hiba (a gesztusok miatt több)

// ---- Állapot ----
bool on = false;                // relék BE
bool rl2On = false;             // RL2 ténylegesen meghúzott
unsigned long onTime = 0;
bool trigOff = false;           // trigger tiltva (NVS)
bool faultActive = false;       // hiba-villogás folyamatban
bool faultHold = false;         // hibafigyelés szünetel a következő bekapcsolásig
unsigned long faultStart = 0;
int lastFault = 0;              // utolsó hiba kódja (F_*)
int storedFault = 0;            // NVS-ben tárolt hibakód
bool lockoutPending = false;    // hiba utáni tiltás (lejártakor egyszer kiértékeljük)
unsigned long lockoutUntil = 0;
bool offPending = false;        // trigger elment az RL2 előtt: kikapcsolás az RL2 után
unsigned long offAt = 0;
volatile bool statusReq = false;

// Gomb-gesztusok
bool pressConsumed = false;     // a nyomást már felhasználtuk (hosszú)
unsigned long pressStart = 0;
uint8_t shortCount = 0;         // rövid nyomások a sorozatban
unsigned long lastShortRel = 0;
bool burstTaint = false;        // hibás sorozat: a folytatása nem válthat

// LED
bool ledBase = false;           // alapszint (relék szerint)
uint8_t ledFlashes = 0;         // hátralévő villanások
bool ledFlashOn = false;
unsigned long ledT = 0;
bool ledFaultBlink = false;

// Hálózat-visszajelzés állapota (0 = RL1, 1 = RL2)
unsigned long fbWinStart[2] = {0, 0};   // az aktuális mérési ablak kezdete
uint8_t       fbLow[2]      = {0, 0};   // LOW minták az ablakban
uint8_t       fbCount[2]    = {0, 0};   // utolsó ablak LOW mintái (diagnosztika)
bool          fbValid[2]    = {false, false};   // van teljes ablak a parancs óta
bool          fbPresent[2]  = {false, false};   // az utolsó ablak szerint van hálózat
bool          fbBad[2]      = {false, false};   // eltért a várttól
bool          fbConfirmed[2] = {false, false};  // megjelent a hálózat a bekapcsolás után
unsigned long fbBadSince[2] = {0, 0};
unsigned long fbCmdT[2]     = {0, 0};   // utolsó relé-parancs ideje
int           mainsFaultCh  = 0;        // a hibás csatorna

Preferences prefs;

// ---- Soros napló: sor + külön task ----
struct LogMsg { char text[160]; };
QueueHandle_t logQ = nullptr;

// slog(): események, max. LOG_MAX_PER_SEC / s (a többit összesíti); slogc(): fontos üzenetek, nem korlátozott; tele sornál elvész
const uint8_t       LOG_MAX_PER_SEC = 5;
const unsigned long STATUS_MIN_MS   = 1000;

unsigned long logWinStart = 0;
uint8_t       logWinCount = 0;
unsigned long logSuppressed = 0;   // elnyomott üzenetek
unsigned long logDropped = 0;      // elveszett üzenetek
unsigned long lastStatus = 0;

void logPutV(const char *fmt, va_list ap) {
  if (!logQ) return;
  LogMsg m;
  vsnprintf(m.text, sizeof m.text, fmt, ap);
  if (xQueueSend(logQ, &m, 0) != pdTRUE) logDropped++;   // nem blokkol
}

void slogc(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void slogc(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logPutV(fmt, ap);
  va_end(ap);
}

// új másodperc-ablak; jelenti a kihagyott / elveszett üzeneteket
void logWindow() {
  unsigned long now = millis();
  if (now - logWinStart < 1000) return;
  logWinStart = now;
  logWinCount = 0;
  if (logSuppressed) {
    unsigned long n = logSuppressed;
    logSuppressed = 0;
    slogc("(%lu esemenyuzenet elnyomva: tul sok uzenet)\n", n);
  }
  if (logDropped) {
    unsigned long n = logDropped;
    logDropped = 0;
    slogc("(%lu uzenet elveszett: tele a sor)\n", n);
  }
}

void slog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void slog(const char *fmt, ...) {
  logWindow();
  if (logWinCount >= LOG_MAX_PER_SEC) { logSuppressed++; return; }
  logWinCount++;
  va_list ap;
  va_start(ap, fmt);
  logPutV(fmt, ap);
  va_end(ap);
}

void logTask(void *) {
  LogMsg m;
  for (;;) {
    if (xQueueReceive(logQ, &m, pdMS_TO_TICKS(50)) == pdTRUE) Serial.print(m.text);
    while (Serial.available() > 0) {
      int c = Serial.read();
      if (c == 's' || c == 'S') statusReq = true;
    }
  }
}

// ---- LED ----
void ledFlash(uint8_t n) {              // n villanás, utána alapszint
  ledFlashes = n;
  ledFlashOn = true;
  ledT = millis();
}

void ledUpdate() {
  unsigned long now = millis();
  bool level = ledBase;
  if (ledFaultBlink) {
    level = (((now - ledT) / FAULT_HALF_MS) % 2) == 0;
  } else if (ledFlashes > 0) {
    if (ledFlashOn) {
      if (now - ledT >= FLASH_ON_MS) { ledFlashOn = false; ledT = now; }
    } else if (now - ledT >= FLASH_OFF_MS) {
      ledFlashes--;
      if (ledFlashes > 0) { ledFlashOn = true; ledT = now; }
    }
    level = (ledFlashes > 0) ? ledFlashOn : ledBase;
  }
  digitalWrite(PIN_LED, level ? HIGH : LOW);
}

// Hibakódok és bemenet-azonosítók (int, nem enum: az Arduino IDE prototípusai miatt)
const int F_NONE = 0, F_STUCK = 1, F_CHATTER = 2, F_PRESSES = 3, F_NOMAINS = 4, F_MAINSOFF = 5;
const int IN_BTN = 0, IN_TRIG = 1, IN_FB1 = 2, IN_FB2 = 3;

// ---- Hálózati feszültség visszajelzés ----
// Az AC opto kimenete 100 Hz-es impulzusokban LOW: ablakonként (MAINS_WIN_MS) számoljuk a LOW mintákat
bool fbExpected(int ch) { return ch == 0 ? on : rl2On; }

// relé-parancs: új, tiszta mérés
void mainsCmd(int ch) {
  if (!MAINS_CHECK) return;
  unsigned long t = millis();
  fbWinStart[ch] = t;
  fbLow[ch] = 0;
  fbValid[ch] = false;
  fbBad[ch] = false;
  fbConfirmed[ch] = false;
  fbCmdT[ch] = t;
}

// függő eltérés törlése
void mainsClearBad() {
  fbBad[0] = false;
  fbBad[1] = false;
}

void mainsUpdate() {
  if (!MAINS_CHECK) return;
  unsigned long now = millis();
  for (int ch = 0; ch < 2; ch++) {
    if (digitalRead(ch == 0 ? PIN_FB1 : PIN_FB2) == LOW && fbLow[ch] < 255) fbLow[ch]++;
    if (now - fbWinStart[ch] < MAINS_WIN_MS) continue;
    fbWinStart[ch] = now;
    bool present = (fbLow[ch] >= MAINS_MIN_LOW);
    fbCount[ch] = fbLow[ch];
    fbLow[ch] = 0;
    fbPresent[ch] = present;
    fbValid[ch] = true;
    bool expected = fbExpected(ch);
    bool mismatch = (present != expected);
    if (mismatch && !expected && (now - fbCmdT[ch]) < MAINS_OFF_GRACE_MS) mismatch = false;   // türelmi idő kikapcsolás után
    if (mismatch) {
      if (!fbBad[ch]) { fbBad[ch] = true; fbBadSince[ch] = now; }
    } else {
      fbBad[ch] = false;
      if (expected && !fbConfirmed[ch]) {
        fbConfirmed[ch] = true;
        slog("RL%d kimenet: halozat OK (%lu ms)\n", ch + 1, now - fbCmdT[ch]);
      }
    }
  }
}

// hibakód, ha egy kimenet tartósan eltér a várttól (csatorna: mainsFaultCh)
int mainsFault() {
  if (!MAINS_CHECK) return F_NONE;
  unsigned long now = millis();
  for (int ch = 0; ch < 2; ch++) {
    if (fbBad[ch] && (now - fbBadSince[ch]) >= MAINS_FAULT_MS) {
      mainsFaultCh = ch;
      return fbExpected(ch) ? F_NOMAINS : F_MAINSOFF;
    }
  }
  return F_NONE;
}

void setState(bool s, const char *src) {
  if (s == on) return;
  on = s;
  offPending = false;
  if (on) {
    digitalWrite(PIN_RL1, HIGH);
    onTime = millis();                 // az RL2 a loop()-ban jön
    ledBase = true;
    mainsCmd(0);
    slog("RL1 BE (%s), RL2 %lu ms mulva\n", src, (unsigned long)RL2_DELAY_MS);
  } else {
    digitalWrite(PIN_RL2, LOW);        // mindkettő azonnal, RL2 előbb
    digitalWrite(PIN_RL1, LOW);
    rl2On = false;
    ledBase = false;
    ledFlashes = 0;
    mainsCmd(0);
    mainsCmd(1);
    slog("RL1+RL2 KI (%s)\n", src);
  }
}

// ---- Bemenet szűrés és hibafelismerés (az Arduino IDE miatt függvényparaméterben nincs saját típus: tömbindex / int) ----

struct Input {
  uint8_t pin;
  const char *name;
  unsigned long stuckMs;     // 0 = nincs beragadás-figyelés
  uint8_t changeLimit;       // hibahatár: váltás / ablak
  bool stable;               // szűrt
  bool raw;                  // nyers
  unsigned long t;           // utolsó nyers váltás ideje
  unsigned long lowSince;    // mióta LOW
  unsigned long winStart;    // ablak kezdete
  uint8_t changes;           // váltások az ablakban
};

Input inputs[2] = {
  { (uint8_t)PIN_BTN,  "GOMB",    BTN_STUCK_MS, BTN_CHANGE_LIMIT, HIGH, HIGH, 0, 0, 0, 0 },
  { (uint8_t)PIN_TRIG, "TRIGGER", 0,            CHANGE_LIMIT,     HIGH, HIGH, 0, 0, 0, 0 }
};
Input &btn  = inputs[IN_BTN];
Input &trig = inputs[IN_TRIG];

// true: a szűrt állapot változott
bool updateInput(int idx) {
  Input &in = inputs[idx];
  unsigned long now = millis();
  if (now - in.winStart >= WINDOW_MS) { in.winStart = now; in.changes = 0; }
  bool r = digitalRead(in.pin);
  if (r != in.raw) {
    in.raw = r;
    in.t = now;
    if (r == LOW) in.lowSince = now;
  }
  if (r != in.stable && (now - in.t) >= DEBOUNCE_MS) {
    in.stable = r;
    in.changes++;
    return true;
  }
  return false;
}

int checkFault(int idx) {
  const Input &in = inputs[idx];
  if (in.changes > in.changeLimit) return F_CHATTER;
  if (in.stuckMs && in.raw == LOW && (millis() - in.lowSince) >= in.stuckMs) return F_STUCK;
  return F_NONE;
}

// tiszta lap: bemenetek újraszinkronizálása, számlálók nullázása
void resyncInputs() {
  unsigned long now = millis();
  for (int i = 0; i < 2; i++) {
    Input &in = inputs[i];
    in.raw = in.stable = digitalRead(in.pin);
    in.t = now;
    in.lowSince = now;
    in.winStart = now;
    in.changes = 0;
  }
  shortCount = 0;
  pressConsumed = (btn.stable == LOW);   // a már lenyomott gomb nem új nyomás
  mainsClearBad();
}

// hardveres hiba mentése NVS-be (csak változáskor ír); a zaj- és nyomásszám-hibát nem mentjük
void saveFault(int f) {
  if (f != F_NONE && f != F_STUCK && f != F_NOMAINS && f != F_MAINSOFF) return;
  if (f == storedFault) return;
  if (!prefs.begin("relay", false)) return;
  if (prefs.putUChar("fault", (uint8_t)f) > 0) storedFault = f;
  prefs.end();
}

void startFault(int idx, int f) {
  lastFault = f;
  saveFault(f);
  faultActive = true;
  faultStart = millis();
  ledFaultBlink = true;
  ledFlashes = 0;
  ledT = faultStart;
  if (idx >= IN_FB1) {
    const char *nm = (idx == IN_FB1) ? "RL1" : "RL2";
    slogc("!!! %s kimenet HIBA: %s -> LED villog, majd relek es LED KI\n", nm,
          (f == F_NOMAINS)
              ? ((idx == IN_FB2) ? "nincs halozati feszultseg: az RL2 nem huzott be az RL1 utan (vagy elveszett)"
                                 : "nincs halozati feszultseg: az RL1 nem huzott be (vagy elveszett)")
              : "halozati feszultseg van KIKAPCSOLT rele mellett (ragadt / osszehegedt erintkezo?)");
    return;
  }
  const Input &in = inputs[idx];
  slogc("!!! %s bemenet HIBA: %s -> LED villog, majd relek es LED KI\n", in.name,
        (f == F_STUCK)   ? "tartosan LOW (beragadt gomb, zarlat a GND-re?)"
        : (f == F_PRESSES) ? "3 vagy tobb rovid nyomas egymas utan"
                         : "tul gyakori valtas (zaj, rossz kontaktus, hibas vezetek?)");
}

void endFault() {
  ledFaultBlink = false;
  setState(false, "hiba");             // relék KI
  ledBase = false;
  ledFlashes = 0;
  faultActive = false;
  faultHold = true;                    // a következő bekapcsolásig nincs hibafigyelés
  resyncInputs();
  lockoutPending = true;               // tiltás kezdete
  lockoutUntil = millis() + FAULT_LOCKOUT_MS;
  slogc("Hiba vege: relek es LED KI. %lu ms-ig nincs bekapcsolas, utana tiszta lappal indulhat.\n", (unsigned long)FAULT_LOCKOUT_MS);
}

// bekapcsolási kérés; hiba után az első kérés tiszta lappal indul
void switchOn(const char *src, bool manual) {
  if (on || faultActive) return;
  if (lockoutPending && (long)(millis() - lockoutUntil) < 0) {      // tiltás alatt eldobjuk
    slog("Bekapcsolas tiltva (%s): hiba utan meg %lu ms\n", src, (unsigned long)(lockoutUntil - millis()));
    return;
  }
  if (faultHold) {
    resyncInputs();
    faultHold = false;
    slog("Tiszta lap: hibafigyeles ujra aktiv\n");
  }
  if (MAINS_CHECK) {                   // KI relé kimenetén nem lehet hálózat
    for (int ch = 0; ch < 2; ch++) {
      if (fbValid[ch] && fbPresent[ch] && (millis() - fbCmdT[ch]) >= MAINS_OFF_GRACE_MS) {
        slogc("Bekapcsolas megtagadva: RL%d kimeneten halozati feszultseg van, pedig a rele KI\n", ch + 1);
        startFault(IN_FB1 + ch, F_MAINSOFF);
        return;
      }
    }
  }
  setState(true, src);
  if (manual && trigOff) ledFlash(2);  // tiltott trigger + kézi bekapcsolás: 2 villanás
}

// két rövid nyomás: trigger tiltás / engedés, NVS mentés
void toggleTrigOff() {
  trigOff = !trigOff;
  trig.changes = 0;                              // a tiltott időszak váltásai ne okozzanak hibát
  trig.winStart = millis();
  bool saved = prefs.begin("relay", false);      // írható mód: létrehozza a névteret
  if (saved) {
    saved = prefs.putBool("trigOff", trigOff) > 0;
    prefs.end();
  }
  ledFlash(trigOff ? 2 : 3);
  slogc("TRIGGER %s (ket rovid nyomas)%s\n", trigOff ? "TILTVA" : "ENGEDELYEZVE",
        saved ? ", elmentve" : ", FIGYELEM: a mentes NEM sikerult (NVS hiba)");
}

// gesztusok: hosszú = be/ki; pontosan 2 rövid = trigger váltás (a sorozat végén); 3+ rövid = hiba, tiltás nem változik
void buttonLogic(bool changed) {
  unsigned long now = millis();
  if (changed) {
    if (btn.stable == LOW) {
      pressStart = btn.t;
      pressConsumed = false;
    } else if (!pressConsumed) {
      if (shortCount == 0 && burstTaint && (long)(pressStart - lastShortRel) > (long)DOUBLE_GAP_MS) burstTaint = false;   // új sorozat
      shortCount++;
      lastShortRel = now;
      if (shortCount >= SHORT_FAULT_COUNT) {     // a trigger-váltás előtt
        shortCount = 0;
        burstTaint = true;                       // a folytatás nem válthat
        if (!faultHold) { startFault(IN_BTN, F_PRESSES); return; }
      }
    } else {
      lastShortRel = now;                        // felhasznált nyomás vége: a sorozat él
    }
  }
  if (btn.stable == LOW && !pressConsumed && (now - pressStart) >= LONG_PRESS_MS) {
    pressConsumed = true;                        // hosszú nyomás
    shortCount = 0;
    burstTaint = false;                              // elveti a függő sorozatot
    if (on) setState(false, "gomb (hosszu nyomas)");
    else switchOn("gomb", true);
  }
  // sorozat lezárása: gomb fent, DOUBLE_GAP_MS csend
  if (shortCount > 0 && btn.raw == HIGH && btn.stable == HIGH && (now - lastShortRel) > DOUBLE_GAP_MS) {
    uint8_t n = shortCount;
    shortCount = 0;
    if (burstTaint) { burstTaint = false; slog("hibas sorozat folytatasa: nincs muvelet\n"); }
    else if (n == 2) toggleTrigOff();
    else slog("%u rovid nyomas: nincs muvelet\n", (unsigned)n);
  }
}

void printStatus() {
  char lk[40] = "";
  if (lockoutPending && (long)(millis() - lockoutUntil) < 0)
    snprintf(lk, sizeof lk, " [tiltas: %lu ms]", (unsigned long)(lockoutUntil - millis()));
  slogc("ALLAPOT: RL1=%s RL2=%s LED=%s%s%s%s\n", on ? "BE" : "KI", rl2On ? "BE" : "KI",
        (ledFaultBlink) ? "hiba-villog" : (ledBase ? "vilagit" : "sotet"),
        faultHold ? " (hiba utan: figyeles szunetel)" : "", offPending ? " (kikapcs. fuggoben)" : "", lk);
  slogc("  GOMB:    %s\n", btn.stable == LOW ? "nyomva" : "nyugalom");
  slogc("  TRIGGER: %s, %s\n", trig.stable == LOW ? "aktiv (12 V)" : "nincs", trigOff ? "TILTVA" : "engedelyezve");
  if (MAINS_CHECK) {
    for (int ch = 0; ch < 2; ch++)
      slogc("  RL%d kimenet: halozat %s%s (LOW minta/ablak: %u, kuszob: %u)\n", ch + 1,
            !fbValid[ch] ? "meres folyamatban" : (fbPresent[ch] ? "VAN" : "nincs"),
            fbBad[ch] ? " (ELTERES a varttol)" : "", (unsigned)fbCount[ch], (unsigned)MAINS_MIN_LOW);
  } else {
    slogc("  Halozat-visszajelzes: KIKAPCSOLVA (MAINS_CHECK = false)\n");
  }
}

const char *resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "tapfeltoltes";
    case ESP_RST_EXT:      return "kulso reset";
    case ESP_RST_SW:       return "szoftveres ujrainditas";
    case ESP_RST_PANIC:    return "panic (fagyas/hiba)";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (taphiba)";
    default:               return "egyeb";
  }
}

void setup() {
  // relék és LED KI először
  digitalWrite(PIN_RL1, LOW);
  digitalWrite(PIN_RL2, LOW);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_RL1, OUTPUT);
  pinMode(PIN_RL2, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_RL1, LOW);
  digitalWrite(PIN_RL2, LOW);
  digitalWrite(PIN_LED, LOW);

  // soros napló
  Serial.begin(115200);
  logQ = xQueueCreate(16, sizeof(LogMsg));
  if (logQ) xTaskCreate(logTask, "slog", 4096, nullptr, 1, nullptr);

  // mentett beállítások
  bool nvsOk = prefs.begin("relay", false);
  if (nvsOk) {
    trigOff = prefs.getBool("trigOff", false);
    storedFault = prefs.getUChar("fault", 0);
    prefs.end();
  }

  // bemenetek
  pinMode(PIN_BTN,  INPUT_PULLUP);
  pinMode(PIN_TRIG, INPUT_PULLUP);
  if (MAINS_CHECK) {
    pinMode(PIN_FB1, INPUT_PULLUP);
    pinMode(PIN_FB2, INPUT_PULLUP);
    mainsCmd(0);
    mainsCmd(1);
    fbCmdT[0] -= MAINS_OFF_GRACE_MS;   // induláskor nincs türelmi idő
    fbCmdT[1] -= MAINS_OFF_GRACE_MS;
  }
  for (int i = 0; i < 50; i++) {       // a bemenetek beállnak, közben mintavétel
    mainsUpdate();
    delay(1);
  }
  resyncInputs();                      // a bootkor nyomva tartott gomb nem új nyomás

  // watchdog
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = WDT_MS;
  cfg.idle_core_mask = 0;              // csak a saját taskunk
  cfg.trigger_panic  = true;           // timeout -> újraindulás
  if (esp_task_wdt_init(&cfg) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&cfg);    // a core már inicializálta
  }
  esp_task_wdt_add(nullptr);           // a loop task figyelése

  // fagyás utáni újraindulás
  esp_reset_reason_t rr = esp_reset_reason();
  bool crashReset = (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT ||
                     rr == ESP_RST_WDT      || rr == ESP_RST_PANIC);
  if (BROWNOUT_IS_CRASH && rr == ESP_RST_BROWNOUT) crashReset = true;

  slogc("\n=== Rele szekvenszer indul ===\n");
  slogc("Indulas oka: %s\n", resetName(rr));
  slogc("GOMB: %s, TRIGGER: %s\n", btn.stable == LOW ? "LOW (nyomva?)" : "HIGH",
                                   trig.stable == LOW ? "LOW (aktiv)" : "HIGH (nincs)");
  slogc("Trigger bemenet: %s%s\n", trigOff ? "TILTVA" : "engedelyezve",
        nvsOk ? " (mentett beallitas)" : " (FIGYELEM: NVS hiba, alapertelmezett)");
  slogc("Halozat-visszajelzes: %s\n", MAINS_CHECK ? "BE (RL1: D4/GPIO6, RL2: D7/GPIO20)" : "KI (MAINS_CHECK = false)");

  if (storedFault != F_NONE) {
    // mentett hiba: nem kapcsol be magától, a tiltás végén ellenőrzi az okot
    lastFault = storedFault;
    faultHold = true;
    lockoutPending = true;
    lockoutUntil = millis() + FAULT_LOCKOUT_MS;
    slogc("Mentett hiba az elozo futasbol (%s): %lu ms-ig nincs bekapcsolas, utana az ok ellenorzese\n",
          storedFault == F_STUCK ? "beragadt gomb" : (storedFault == F_NOMAINS ? "nincs halozat a relek utan" : "halozat KI rele mellett"),
          (unsigned long)FAULT_LOCKOUT_MS);
  } else if (!trigOff && trig.stable == LOW && (crashReset || TRIGGER_ON_BOOT)) {
    // aktív triggerre: fagyás után mindig, egyébként TRIGGER_ON_BOOT szerint
    switchOn(crashReset ? "fagyas utani ujrainditas" : "indulaskor aktiv trigger", false);
  }
}

void loop() {
  esp_task_wdt_reset();                // csak élő loopnál etetjük
  unsigned long now = millis();

  bool btnChanged  = updateInput(IN_BTN);
  bool trigChanged = updateInput(IN_TRIG);
  mainsUpdate();

  if (faultActive) {
    // villogás: bemenetek nem számítanak, a végén minden KI
    if (btnChanged) lastShortRel = now;   // a hiba alatti nyomás is a sorozat része
    if (now - faultStart >= FAULT_BLINK_MS) endFault();
  } else {
    // hibafigyelés (tiltott triggert nem figyelünk)
    if (!faultHold) {
      int f = checkFault(IN_BTN);
      if (f != F_NONE) {
        startFault(IN_BTN, f);
      } else if (!trigOff) {
        f = checkFault(IN_TRIG);
        if (f != F_NONE) startFault(IN_TRIG, f);
      }
    }
    if (!faultActive && !faultHold) {    // hálózat-visszajelzés
      int mf = mainsFault();
      if (mf != F_NONE) startFault(IN_FB1 + mainsFaultCh, mf);
    }
    if (!faultActive) {
      buttonLogic(btnChanged);
    }
    if (!faultActive) {
      // trigger: aktív élre be, inaktív élre ki
      if (trigChanged && !trigOff) {
        if (trig.stable == LOW) {
          offPending = false;              // újra aktív: a függő kikapcsolás elmarad
          switchOn("trigger", false);
        } else if (on && !rl2On) {
          offPending = true;               // teljes ciklus: előbb az RL2 behúz, utána ki
          slog("Trigger elment az RL2 behuzasa elott: teljes ciklus, utana kikapcsol\n");
        } else {
          setState(false, "trigger");
        }
      }
    }
    // tiltás vége: ok ellenőrzése, mentett hiba törlése, automata bekapcsolás
    if (!faultActive && lockoutPending && (long)(now - lockoutUntil) >= 0) {
      lockoutPending = false;
      int offCh = -1;                    // KI relé kimenetén hálózat van
      if (MAINS_CHECK && lastFault == F_MAINSOFF) {
        for (int ch = 0; ch < 2; ch++) if (fbValid[ch] && fbPresent[ch]) offCh = ch;
      }
      if (btn.stable == LOW || btn.raw == LOW) {
        slog("Tiltas vege, de a gomb meg nyomva: beragadt gomb, a relek nem kapcsolnak be\n");
        startFault(IN_BTN, F_STUCK);
      } else if (offCh >= 0) {
        slog("Tiltas vege, de RL%d kimeneten meg van halozat KI relenel: a relek nem kapcsolnak be\n", offCh + 1);
        startFault(IN_FB1 + offCh, F_MAINSOFF);
      } else {
        int was = lastFault;
        lastFault = F_NONE;
        saveFault(F_NONE);               // az ok megszűnt / nem ellenőrizhető
        if (was == F_NOMAINS) {
          slog("A hiba (nincs halozat a relek utan) nem ellenorizheto relek nelkul: nincs automata ujrakapcsolas, uj gomb / trigger el kell\n");
        } else if (AUTO_ON_AFTER_FAULT && !trigOff && trig.stable == LOW) {
          switchOn("aktiv trigger hiba utan", false);
        }
      }
    }
  }

  // RL2 késleltetve (előjeles különbség: a "now" régebbi lehet az onTime-nál)
  if (on && !rl2On && !faultActive && (!MAINS_CHECK || fbConfirmed[0]) && (long)(now - onTime) >= (long)RL2_DELAY_MS) {
    digitalWrite(PIN_RL2, HIGH);       // (MAINS_CHECK: csak ha az RL1 kimenetén már van hálózat)
    rl2On = true;
    mainsCmd(1);
    if (offPending) offAt = millis() + RL2_MIN_ON_MS;   // teljes ciklus: minimum behúzási idő
    slog("RL2 BE\n");
  }
  if (offPending && rl2On && !faultActive && (long)(now - offAt) >= 0) {
    setState(false, "trigger elment (teljes ciklus vege)");
  }

  ledUpdate();

  // 's' parancs (legfeljebb STATUS_MIN_MS-onként)
  if (statusReq) {
    statusReq = false;
    if (millis() - lastStatus >= STATUS_MIN_MS) { lastStatus = millis(); printStatus(); }
  }

  logWindow();                         // összesítő sor

  delay(1);                            // CPU az idle tasknak
}