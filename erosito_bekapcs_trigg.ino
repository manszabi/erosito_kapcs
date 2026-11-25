// XIAO ESP32-C3 relé szekvenszer: gomb + 12 V trigger, RL1 azonnal, RL2 késleltetve, hálózat-visszajelzés, hibakezelés
// Gomb: hosszú nyomás = be/ki; 2 rövid nyomás = trigger tiltás/engedés (NVS); 3+ rövid nyomás = hiba
// Trigger: aktív élre be, inaktív élre ki; mindig teljes RL1-RL2 ciklus: az RL2 legalább RL2_MIN_ON_MS-ig behúzva marad, csak utána ki
// LED: be = világít; hiba = FAULT_BLINK_MS villogás, utána minden KI; trigger tiltva + kézi bekapcsolás = 2 villanás
// Hiba után FAULT_LOCKOUT_MS ideig nincs bekapcsolás; utána beragadt gomb = újabb hiba, különben aktív triggerre be (AUTO_ON_AFTER_FAULT)
// Hardveres hibát (beragadt gomb, hálózat hiánya / KI relénél) az NVS tárolja: reset után az ok megszűnéséig nem kapcsol be magától
// Hálózat-visszajelzés (MAINS_CHECK): H11AA1M csak az RL1 kimenetén (LOW = van hálózat); az RL2 csak az RL1 visszajelzése után húz be
// (az RL2 a toroid lágyindító NTC-jét zárja rövidre, utána külön visszajelzés nem kell)
// Opto: 3 x 22k sorosan a hálózat felé, kimenet 47k felhúzóval a 3V3-ra; az 's' parancs a LOW mintaszámot is kiírja (~28-37 normális)
// Boot alatt a relék KI; aktív triggerre induláskor bekapcsol (TRIGGER_ON_BOOT); watchdog: WDT_MS
// Áramszünet után (tápfeltöltés / brownout reset) POWERUP_HOLD_MS ideig a gomb és a trigger hatástalan; utána a beállítások szerint
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
// USB CDC nélkül a Serial a GPIO20/21 UART-ra kerülne, a soros napló nem az USB-n jelenne meg
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || (ARDUINO_USB_CDC_ON_BOOT == 0)
#warning "Allitsd be: Eszkozok > USB CDC On Boot > Enabled (kulonben a soros naplo nem az USB-n jelenik meg)"
#endif

// ---- Lábak (GPIO számok; kerülendő: GPIO2/8/9 strapping, GPIO21 UART TX) ----
const int PIN_LED  = 3;   // D1:  2N2222 bázis 1k-n át, 4,7k lehúzó (HIGH = világít)
const int PIN_BTN  = 4;   // D2:  gomb -> GND (belső felhúzó)
const int PIN_TRIG = 5;   // D3:  opto kimenet, LOW = aktív, külső 10k felhúzó 3V3-ra
const int PIN_RL1  = 10;  // D10: RL1 tranzisztor
const int PIN_RL2  = 7;   // D5:  RL2 tranzisztor
const int PIN_FB1  = 6;   // D4:  RL1 kimenet opto, LOW = van hálózat, 47k felhúzó 3V3-ra

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
const bool BROWNOUT_IS_POWERLOSS = true;   // true: a brownout reset áramszünetnek számít (POWERUP_HOLD_MS); false: fagyásnak
const unsigned long POWERUP_HOLD_MS = 120000UL; // áramszünet után ennyi ideig a gomb és a trigger hatástalan (0 = nincs)

const unsigned long FAULT_LOCKOUT_MS = 5000; // hiba után ennyi ideig nincs bekapcsolás
const bool AUTO_ON_AFTER_FAULT  = true;    // true: a tiltás végén aktív triggerre bekapcsol
const unsigned long RL2_MIN_ON_MS = 2000;  // trigger: az RL2 legalább ennyi ideig behúzva marad kikapcsolás előtt

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
unsigned long rl2Time = 0;      // RL2 behúzásának ideje
bool powerHold = false;         // áramszünet utáni várakozás: gomb és trigger hatástalan
unsigned long powerHoldUntil = 0;
bool trigOff = false;           // trigger tiltva (NVS)
bool faultActive = false;       // hiba-villogás folyamatban
bool faultHold = false;         // hibafigyelés szünetel a következő bekapcsolásig
unsigned long faultStart = 0;
int lastFault = 0;              // utolsó hiba kódja (F_*)
int storedFault = 0;            // NVS-ben tárolt hibakód
bool lockoutPending = false;    // hiba utáni tiltás (lejártakor egyszer kiértékeljük)
unsigned long lockoutUntil = 0;
bool offPending = false;        // trigger elment: kikapcsolás az RL2 minimális behúzási ideje után
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

// Hálózat-visszajelzés állapota (csak RL1 kimenet)
unsigned long fbWinStart  = 0;      // az aktuális mérési ablak kezdete
uint8_t       fbLow       = 0;      // LOW minták az ablakban
uint8_t       fbCount     = 0;      // utolsó ablak LOW mintái (diagnosztika)
bool          fbValid     = false;  // van teljes ablak a parancs óta
bool          fbPresent   = false;  // az utolsó ablak szerint van hálózat
bool          fbBad       = false;  // eltért a várttól
bool          fbConfirmed = false;  // megjelent a hálózat a bekapcsolás után
unsigned long fbBadSince  = 0;
unsigned long fbCmdT      = 0;      // utolsó relé-parancs ideje

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
const int IN_BTN = 0, IN_TRIG = 1, IN_FB1 = 2;

// ---- Hálózati feszültség visszajelzés ----
// Az AC opto kimenete 100 Hz-es impulzusokban LOW: ablakonként (MAINS_WIN_MS) számoljuk a LOW mintákat
// relé-parancs: új, tiszta mérés
void mainsCmd() {
  if (!MAINS_CHECK) return;
  unsigned long t = millis();
  fbWinStart = t;
  fbLow = 0;
  fbValid = false;
  fbBad = false;
  fbConfirmed = false;
  fbCmdT = t;
}

// függő eltérés törlése
void mainsClearBad() {
  fbBad = false;
}

void mainsUpdate() {
  if (!MAINS_CHECK) return;
  unsigned long now = millis();
  if (digitalRead(PIN_FB1) == LOW && fbLow < 255) fbLow++;
  if (now - fbWinStart < MAINS_WIN_MS) return;
  fbWinStart = now;
  bool present = (fbLow >= MAINS_MIN_LOW);
  fbCount = fbLow;
  fbLow = 0;
  fbPresent = present;
  fbValid = true;
  bool expected = on;
  bool mismatch = (present != expected);
  if (mismatch && !expected && (now - fbCmdT) < MAINS_OFF_GRACE_MS) mismatch = false;   // türelmi idő kikapcsolás után
  if (mismatch) {
    if (!fbBad) { fbBad = true; fbBadSince = now; }
  } else {
    fbBad = false;
    if (expected && !fbConfirmed) {
      fbConfirmed = true;
      slog("RL1 kimenet: halozat OK (%lu ms)\n", now - fbCmdT);
    }
  }
}

// hibakód, ha az RL1 kimenete tartósan eltér a várttól
int mainsFault() {
  if (!MAINS_CHECK) return F_NONE;
  if (fbBad && (millis() - fbBadSince) >= MAINS_FAULT_MS) return on ? F_NOMAINS : F_MAINSOFF;
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
    mainsCmd();
    slog("RL1 BE (%s), RL2 %lu ms mulva\n", src, (unsigned long)RL2_DELAY_MS);
  } else {
    digitalWrite(PIN_RL2, LOW);        // mindkettő azonnal, RL2 előbb
    digitalWrite(PIN_RL1, LOW);
    rl2On = false;
    ledBase = false;
    ledFlashes = 0;
    mainsCmd();
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
  if (idx == IN_FB1) {
    slogc("!!! RL1 kimenet HIBA: %s -> LED villog, majd relek es LED KI\n",
          (f == F_NOMAINS) ? "nincs halozati feszultseg: az RL1 nem huzott be (vagy elveszett)"
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
  if (MAINS_CHECK && fbValid && fbPresent && (millis() - fbCmdT) >= MAINS_OFF_GRACE_MS) {   // KI relé kimenetén nem lehet hálózat
    slogc("Bekapcsolas megtagadva: RL1 kimeneten halozati feszultseg van, pedig a rele KI\n");
    startFault(IN_FB1, F_MAINSOFF);
    return;
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
  char lk[64] = "";
  if (powerHold)
    snprintf(lk, sizeof lk, " [aramszunet utani varakozas: %lu s]", (unsigned long)((powerHoldUntil - millis()) / 1000));
  else if (lockoutPending && (long)(millis() - lockoutUntil) < 0)
    snprintf(lk, sizeof lk, " [tiltas: %lu ms]", (unsigned long)(lockoutUntil - millis()));
  slogc("ALLAPOT: RL1=%s RL2=%s LED=%s%s%s%s\n", on ? "BE" : "KI", rl2On ? "BE" : "KI",
        (ledFaultBlink) ? "hiba-villog" : (ledBase ? "vilagit" : "sotet"),
        faultHold ? " (hiba utan: figyeles szunetel)" : "", offPending ? " (kikapcs. fuggoben)" : "", lk);
  slogc("  GOMB:    %s\n", btn.stable == LOW ? "nyomva" : "nyugalom");
  slogc("  TRIGGER: %s, %s\n", trig.stable == LOW ? "aktiv (12 V)" : "nincs", trigOff ? "TILTVA" : "engedelyezve");
  if (MAINS_CHECK) {
    slogc("  RL1 kimenet: halozat %s%s (LOW minta/ablak: %u, kuszob: %u)\n",
          !fbValid ? "meres folyamatban" : (fbPresent ? "VAN" : "nincs"),
          fbBad ? " (ELTERES a varttol)" : "", (unsigned)fbCount, (unsigned)MAINS_MIN_LOW);
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
    mainsCmd();
    fbCmdT -= MAINS_OFF_GRACE_MS;      // induláskor nincs türelmi idő
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
  bool powerLoss = (rr == ESP_RST_POWERON) || (rr == ESP_RST_BROWNOUT && BROWNOUT_IS_POWERLOSS);
  if (rr == ESP_RST_BROWNOUT && !BROWNOUT_IS_POWERLOSS) crashReset = true;
  if (powerLoss && POWERUP_HOLD_MS > 0) {
    powerHold = true;
    powerHoldUntil = millis() + POWERUP_HOLD_MS;
  }

  slogc("\n=== Rele szekvenszer indul ===\n");
  slogc("Indulas oka: %s\n", resetName(rr));
  slogc("GOMB: %s, TRIGGER: %s\n", btn.stable == LOW ? "LOW (nyomva?)" : "HIGH",
                                   trig.stable == LOW ? "LOW (aktiv)" : "HIGH (nincs)");
  slogc("Trigger bemenet: %s%s\n", trigOff ? "TILTVA" : "engedelyezve",
        nvsOk ? " (mentett beallitas)" : " (FIGYELEM: NVS hiba, alapertelmezett)");
  slogc("Halozat-visszajelzes: %s\n", MAINS_CHECK ? "BE (RL1 kimenet: D4/GPIO6)" : "KI (MAINS_CHECK = false)");

  if (powerHold) {
    slogc("Aramszunet utan: %lu s-ig a gomb es a trigger hatastalan\n", (unsigned long)(POWERUP_HOLD_MS / 1000));
  }
  if (storedFault != F_NONE) {
    // mentett hiba: nem kapcsol be magától, a tiltás végén ellenőrzi az okot (áramszünet után a várakozás végén)
    unsigned long wait = powerHold ? POWERUP_HOLD_MS : FAULT_LOCKOUT_MS;
    lastFault = storedFault;
    faultHold = true;
    lockoutPending = true;
    lockoutUntil = millis() + wait;
    slogc("Mentett hiba az elozo futasbol (%s): %lu ms-ig nincs bekapcsolas, utana az ok ellenorzese\n",
          storedFault == F_STUCK ? "beragadt gomb" : (storedFault == F_NOMAINS ? "nincs halozat az RL1 utan" : "halozat KI rele mellett"),
          wait);
  } else if (powerHold) {
    // a várakozás végén a loop() dönt (TRIGGER_ON_BOOT)
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

  if (powerHold) {
    // áramszünet utáni várakozás: gomb és trigger hatástalan, hibafigyelés szünetel
    if ((long)(now - powerHoldUntil) >= 0) {
      powerHold = false;
      resyncInputs();                    // a várakozás alatti nyomások / váltások nem számítanak
      slogc("Aramszunet utani varakozas vege: gomb es trigger ujra aktiv\n");
      if (lockoutPending) {
        // mentett hiba: a tiltás-vége ág értékel (AUTO_ON_AFTER_FAULT)
      } else if (TRIGGER_ON_BOOT && !trigOff && trig.stable == LOW) {
        switchOn("aktiv trigger aramszunet utan", false);
      }
    }
  } else if (faultActive) {
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
      if (mf != F_NONE) startFault(IN_FB1, mf);
    }
    if (!faultActive) {
      buttonLogic(btnChanged);
    }
    if (!faultActive) {
      // trigger: aktív élre be, inaktív élre ki; mindig teljes ciklus (RL2 legalább RL2_MIN_ON_MS)
      if (trigChanged && !trigOff) {
        if (trig.stable == LOW) {
          if (offPending) slog("Trigger ujra aktiv: a fuggo kikapcsolas elmarad\n");
          offPending = false;              // újra aktív: a függő kikapcsolás elmarad
          switchOn("trigger", false);
        } else if (on && !rl2On) {
          offPending = true;               // előbb az RL2 behúz, RL2_MIN_ON_MS után ki
          slog("Trigger elment az RL2 behuzasa elott: teljes ciklus, utana kikapcsol\n");
        } else if (on && (long)(now - rl2Time) < (long)RL2_MIN_ON_MS) {
          offPending = true;               // az RL2 még nem volt behúzva RL2_MIN_ON_MS-ig
          offAt = rl2Time + RL2_MIN_ON_MS;
          slog("Trigger elment: kikapcsolas %lu ms mulva (RL2 minimum ido)\n", (unsigned long)(offAt - now));
        } else {
          setState(false, "trigger");
        }
      }
    }
    // tiltás vége: ok ellenőrzése, mentett hiba törlése, automata bekapcsolás
    if (!faultActive && lockoutPending && (long)(now - lockoutUntil) >= 0) {
      lockoutPending = false;
      bool offMains = MAINS_CHECK && lastFault == F_MAINSOFF && fbValid && fbPresent;   // KI relé kimenetén hálózat van
      if (btn.stable == LOW || btn.raw == LOW) {
        slog("Tiltas vege, de a gomb meg nyomva: beragadt gomb, a relek nem kapcsolnak be\n");
        startFault(IN_BTN, F_STUCK);
      } else if (offMains) {
        slog("Tiltas vege, de RL1 kimeneten meg van halozat KI relenel: a relek nem kapcsolnak be\n");
        startFault(IN_FB1, F_MAINSOFF);
      } else {
        int was = lastFault;
        lastFault = F_NONE;
        saveFault(F_NONE);               // az ok megszűnt / nem ellenőrizhető
        if (was == F_NOMAINS) {
          slog("A hiba (nincs halozat az RL1 utan) nem ellenorizheto relek nelkul: nincs automata ujrakapcsolas, uj gomb / trigger el kell\n");
        } else if (AUTO_ON_AFTER_FAULT && !trigOff && trig.stable == LOW) {
          switchOn("aktiv trigger hiba utan", false);
        }
      }
    }
  }

  // RL2 késleltetve (előjeles különbség: a "now" régebbi lehet az onTime-nál)
  if (on && !rl2On && !faultActive && (!MAINS_CHECK || fbConfirmed) && (long)(now - onTime) >= (long)RL2_DELAY_MS) {
    digitalWrite(PIN_RL2, HIGH);       // (MAINS_CHECK: csak ha az RL1 kimenetén már van hálózat)
    rl2On = true;
    rl2Time = millis();
    if (offPending) offAt = rl2Time + RL2_MIN_ON_MS;   // teljes ciklus: minimum behúzási idő
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