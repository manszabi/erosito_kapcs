// XIAO ESP32-C3 erősítő-bekapcsoló: gomb + 12 V trigger, RL1 (NTC lágyindítás) majd RL2 (NTC áthidalás), hálózatfigyelés, hibakezelés

#include <Arduino.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_attr.h>

// ---- Home Assistant (WiFi + MQTT): false = a hálózati kód be sem fordul ----
#define HOME_ASSISTANT true
#if HOME_ASSISTANT
#if __has_include("secrets.h")
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_URI, MQTT_USER, MQTT_PASS (minta: secrets.example.h)
#include <WiFi.h>
#include <mqtt_client.h>
#define NET_ENABLED 1
#else
#warning "secrets.h nincs a sketch mappaban: Home Assistant kikapcsolva (lasd secrets.example.h)"
#define NET_ENABLED 0
#endif
#else
#define NET_ENABLED 0
#endif

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || (ESP_ARDUINO_VERSION_MAJOR < 3)
#error "Arduino-ESP32 core 3.x kell (Boards Manager: esp32 by Espressif Systems)"
#endif
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || (ARDUINO_USB_CDC_ON_BOOT == 0)
#warning "Allitsd be: Eszkozok > USB CDC On Boot > Enabled (kulonben a soros naplo nem az USB-n jelenik meg)"
#endif

// ---- Lábak ----
const int PIN_LED  = 3;   // D1:  LED tranzisztor (HIGH = világít)
const int PIN_BTN  = 4;   // D2:  gomb GND-re, belső felhúzó
const int PIN_TRIG = 5;   // D3:  trigger opto, LOW = aktív, 10k felhúzó
const int PIN_RL1  = 10;  // D10: RL1 tranzisztor
const int PIN_RL2  = 7;   // D5:  RL2 tranzisztor
const int PIN_FB1  = 6;   // D4:  RL1 kimenet opto (CNY17-3 + DB106 + 3x43k), LOW = van hálózat, 43k felhúzó

// ---- Relé időzítések ----
const unsigned long RL2_DELAY_MS     = 1000;   // RL1 után ennyivel húz be az RL2 (NTC lágyindítás)
const unsigned long RL2_MIN_ON_MS    = 2000;   // kikapcsolás előtt az RL2 legalább ennyit legyen behúzva
const unsigned long MIN_OFF_MS       = 5000;   // minimális kikapcsolt idő (a boottól is számít)
const unsigned long POWERUP_HOLD_MS  = 120000UL; // áramszünet után ennyi ideig nincs trigger (a gomb egyszer bekapcsolhat)
const bool          TRIGGER_ON_BOOT  = true;   // induláskor aktív triggerre bekapcsol
const bool          BROWNOUT_IS_POWERLOSS = true; // brownout reset = áramszünet (különben fagyás)

// ---- Gomb, LED ----
const unsigned long DEBOUNCE_MS      = 30;
const unsigned long LONG_PRESS_MS    = 400;    // hosszú nyomás = be / ki
const unsigned long DOUBLE_GAP_MS    = 800;    // rövid nyomások közti max. szünet
const uint8_t       SHORT_FAULT_COUNT = 3;     // ennyi rövid nyomás = hiba (2 = trigger tiltás / engedés)
const unsigned long FLASH_ON_MS      = 150;
const unsigned long FLASH_OFF_MS     = 150;
const unsigned long HOLD_FLASH_PERIOD_MS = 3000; // áramszünet utáni várakozás: felvillanás periódusa
const unsigned long HOLD_FLASH_MS    = 40;

// ---- Hibakezelés ----
const unsigned long FAULT_BLINK_MS   = 1000;   // hibavillogás hossza
const unsigned long FAULT_HALF_MS    = 100;
const unsigned long FAULT_LOCKOUT_MS = 5000;   // hiba után ennyi ideig nincs bekapcsolás
const bool          AUTO_ON_AFTER_FAULT = true; // tiltás végén aktív triggerre bekapcsol
const unsigned long BTN_STUCK_MS     = 10000;  // ennyi ideig nyomott gomb = beragadt
const unsigned long WINDOW_MS        = 1000;   // váltásszámláló ablak
const uint8_t       CHANGE_LIMIT     = 4;      // trigger: max. váltás / ablak
const uint8_t       BTN_CHANGE_LIMIT = 8;      // gomb: max. váltás / ablak
const uint32_t      WDT_MS           = 3000;

// ---- Hálózatfigyelés (RL1 kimenet) ----
const bool          MAINS_CHECK      = true;
const unsigned long MAINS_WIN_MS     = 40;     // mérési ablak (~40 minta)
const uint8_t       MAINS_MIN_LOW    = 8;      // ennyi LOW minta / ablak = van hálózat (normál: ~28-37)
const unsigned long MAINS_FAULT_MS   = 150;    // ennyi ideig tartó eltérés = hiba
const unsigned long MAINS_OFF_GRACE_MS = 1000; // kikapcsolás után ennyi ideig nem hiba a maradék feszültség
const unsigned long MAINS_LOSS_MS    = 25;     // bekapcsolva ennyi folyamatos HIGH = hálózatkiesés
const unsigned long MAINS_GAP_MS     = 5;      // ennél hosszabb mintavételi szünet nem számít kiesésnek

// ---- Állapotfigyelés (öregedés) ----
const uint32_t      RELAY_LIFE       = 100000; // relé élettartam (kapcsolás), a gyártói adatlap szerint állítható
const uint8_t       RELAY_WARN_PCT   = 80;     // figyelmeztetés ennyi %-nál
const uint8_t       OPTO_DROP_PCT    = 25;     // figyelmeztetés: az opto átlaga ennyi %-kal az alapérték alatt
const uint8_t       OPTO_MIN_AVG     = 15;     // figyelmeztetés: az opto átlaga ez alatt (a hibahatár MAINS_MIN_LOW)
const uint8_t       OPTO_BASE_SESSIONS = 5;    // az első ennyi bekapcsolás átlaga lesz az alapérték
const uint16_t      OPTO_MIN_WINDOWS = 250;    // egy bekapcsolás csak ennyi ablak (~10 s) felett számít
const unsigned long STATS_SAVE_MS    = 3600000UL; // bekapcsolva óránként mentés

// ---- Home Assistant ----
#define NET_ID   "erosito"                     // MQTT témák és eszközazonosító
const unsigned long NET_STATE_MS     = 60000;  // állapot küldése ennyi időnként (és minden változáskor)

static_assert(OPTO_MIN_AVG > MAINS_MIN_LOW, "OPTO_MIN_AVG > MAINS_MIN_LOW kell");
static_assert(MAINS_LOSS_MS >= 20, "MAINS_LOSS_MS >= 20 ms kell (2 felperiodus)");
static_assert(MAINS_GAP_MS < MAINS_LOSS_MS, "MAINS_GAP_MS < MAINS_LOSS_MS kell");
static_assert(MAINS_FAULT_MS > MAINS_WIN_MS, "MAINS_FAULT_MS > MAINS_WIN_MS kell");
static_assert(RL2_DELAY_MS > MAINS_WIN_MS + MAINS_FAULT_MS, "RL2_DELAY_MS: az RL1 visszajelzes az RL2 elott derul ki");
static_assert(MAINS_OFF_GRACE_MS < MIN_OFF_MS, "MAINS_OFF_GRACE_MS < MIN_OFF_MS kell");
static_assert(LONG_PRESS_MS < DOUBLE_GAP_MS, "LONG_PRESS_MS < DOUBLE_GAP_MS kell");
static_assert(WDT_MS >= 1000, "WDT_MS >= 1000 kell (NVS iras)");

// ---- Állapot ----
bool on = false;                // relék BE (RL1)
bool rl2On = false;
unsigned long onTime = 0;
unsigned long rl2Time = 0;
bool rl2Settled = false;        // az RL2 már legalább RL2_MIN_ON_MS-ig behúzva
unsigned long offTime = 0;      // utolsó kikapcsolás (0 = boot)
bool minOffActive = true;       // a minimális kikapcsolt idő fut (bootkor is)
bool powerHold = false;         // áramszünet utáni várakozás
bool holdBtnUsed = false;       // a várakozás alatt egy hosszú gombnyomás bekapcsolhat (csak egyszer)
unsigned long holdStart = 0;    // időzítések: kezdet + hossz (millis() túlcsordulás-biztos)
unsigned long holdDur = 0;
bool trigOff = false;           // trigger tiltva (NVS)
bool faultActive = false;       // hibavillogás
bool faultHold = false;         // bemenet-hibafigyelés szünetel a következő bekapcsolásig
unsigned long faultStart = 0;
int lastFault = 0;
int storedFault = 0;            // NVS-ben tárolt hiba
bool lockoutPending = false;    // hiba utáni tiltás
unsigned long lockoutStart = 0;
unsigned long lockoutDur = 0;
bool offPending = false;        // kikapcsolás az RL2 minimum idő után
bool offByBtn = false;          // a függő kikapcsolást a gomb kérte
bool onReqBtn = false;          // függő bekapcsolás: gomb (feltétel nélkül)
bool onReqTrig = false;         // függő bekapcsolás: trigger (ha még aktív)
const char *onReqSrc = "";
volatile bool statusReq = false;
volatile bool healthReq = false;    // 'h': állapotfigyelés kiírása
volatile bool baseReq = false;      // 'B': új opto alapérték (optocsere után)
volatile bool relayReq = false;     // 'R': relé számláló nullázása (relécsere után)
volatile bool helpReq = false;      // bármely más karakter: parancslista
volatile bool remoteOnReq = false;  // Home Assistant: bekapcsolás (mint a gomb)
volatile bool remoteOffReq = false; // Home Assistant: kikapcsolás
volatile bool netStateDirty = true; // a Home Assistant felé küldendő állapot változott
void netEvent(const char *type, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// Állapotfigyelés: NVS-ben (blob), bekapcsoláskor, kikapcsoláskor és óránként mentve
const uint32_t STATS_VER = 1;
struct Stats {
  uint32_t ver;
  uint32_t onCount;       // bekapcsolások (RL1)
  uint32_t rl2Count;      // RL2 behúzások
  uint32_t relayCount;    // RL1 kapcsolások relécsere óta
  uint32_t onSeconds;     // összes bekapcsolt idő
  uint32_t mainsLoss;     // hálózatkiesések
  uint32_t faults;        // egyéb hibák
  uint32_t powerUps;      // áramszünet utáni indulások
  float    optoAvg;       // opto LOW minta hosszú távú átlaga
  float    optoBase;      // alapérték (új opto)
  uint32_t optoBaseN;     // az alapértékbe számított bekapcsolások
};
Stats st = {};
bool stDirty = false;
bool stCounting = false;        // bekapcsolt idő számlálása fut
unsigned long stAcctT = 0;
unsigned long stAcctMs = 0;
unsigned long stLastSave = 0;
uint32_t sessSum = 0;           // aktuális bekapcsolás opto mintái
uint32_t sessN = 0;
float lastSessMean = 0;

bool pressConsumed = false;
unsigned long pressStart = 0;
uint8_t shortCount = 0;
unsigned long lastShortRel = 0;
bool burstTaint = false;        // hibás nyomássorozat folytatása nem válthat

bool ledBase = false;
uint8_t ledFlashes = 0;
bool ledFlashOn = false;
unsigned long ledT = 0;
bool ledFaultBlink = false;

unsigned long fbWinStart = 0;
uint8_t       fbLow = 0;
uint8_t       fbCount = 0;      // utolsó ablak LOW mintái
bool          fbValid = false;  // van teljes ablak a relé-parancs óta
bool          fbPresent = false;
bool          fbBad = false;    // eltér a várttól
bool          fbConfirmed = false; // bekapcsolás után megjelent a hálózat
unsigned long fbLastLow = 0;
unsigned long fbLastSample = 0;
unsigned long fbBadSince = 0;
unsigned long fbCmdT = 0;       // utolsó relé-parancs
bool          fbGraceOver = false; // letelt a kikapcsolás utáni türelmi idő

Preferences prefs;

// Áramszünet utáni várakozás hátralévő ideje: reset után megmarad, tápvesztéskor elvész
const uint32_t HOLD_MAGIC = 0x484F4C44UL;
RTC_NOINIT_ATTR uint32_t rtcHoldLeft;
RTC_NOINIT_ATTR uint32_t rtcHoldCheck;
unsigned long lastHoldSave = 0;

void holdSave(unsigned long left) {
  rtcHoldLeft = (uint32_t)left;
  rtcHoldCheck = HOLD_MAGIC ^ rtcHoldLeft;
}
void holdClear() {
  rtcHoldLeft = 0;
  rtcHoldCheck = 0;
}
unsigned long holdLoad() {
  if (rtcHoldCheck != (HOLD_MAGIC ^ rtcHoldLeft) || rtcHoldLeft > POWERUP_HOLD_MS) return 0;
  return rtcHoldLeft;
}

// ---- Soros napló (külön task; slog: max. 5 / s, slogc: korlátlan) ----
struct LogMsg { char text[160]; };
QueueHandle_t logQ = nullptr;
const uint8_t       LOG_MAX_PER_SEC = 5;
const unsigned long STATUS_MIN_MS   = 1000;
unsigned long logWinStart = 0;
uint8_t       logWinCount = 0;
unsigned long logSuppressed = 0;
unsigned long logDropped = 0;
unsigned long lastStatus = 0;

void logPutV(const char *fmt, va_list ap) {
  if (!logQ) return;
  LogMsg m;
  vsnprintf(m.text, sizeof m.text, fmt, ap);
  if (xQueueSend(logQ, &m, 0) != pdTRUE) logDropped++;
}

void slogc(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void slogc(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logPutV(fmt, ap);
  va_end(ap);
}

void logWindow() {
  unsigned long now = millis();
  if (now - logWinStart < 1000) return;
  logWinStart = now;
  logWinCount = 0;
  if (logSuppressed) {
    unsigned long n = logSuppressed;
    logSuppressed = 0;
    slogc("(%lu esemenyuzenet elnyomva)\n", n);
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
      else if (c == 'h' || c == 'H') healthReq = true;
      else if (c == 'B') baseReq = true;
      else if (c == 'R') relayReq = true;
      else if (c > ' ') helpReq = true;
    }
  }
}

// ---- LED ----
void ledFlash(uint8_t n) {
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
  } else if (powerHold && !on) {
    level = (now % HOLD_FLASH_PERIOD_MS) < HOLD_FLASH_MS;
  }
  digitalWrite(PIN_LED, level ? HIGH : LOW);
}

// Hibakódok (int, nem enum: az Arduino IDE prototípusai miatt)
const int F_NONE = 0, F_STUCK = 1, F_CHATTER = 2, F_PRESSES = 3, F_NOMAINS = 4, F_MAINSOFF = 5, F_MAINSLOST = 6;
const int IN_BTN = 0, IN_TRIG = 1, IN_FB1 = 2;

// ---- Hálózatfigyelés: az opto a nullátmeneteknél rövid ideig HIGH, egyébként LOW ----
void mainsCmd() {
  if (!MAINS_CHECK) return;
  unsigned long t = millis();
  fbWinStart = t;
  fbLow = 0;
  fbValid = false;
  fbBad = false;
  fbConfirmed = false;
  fbCmdT = t;
  fbGraceOver = false;
  fbLastLow = t;
}

void mainsUpdate() {
  if (!MAINS_CHECK) return;
  unsigned long now = millis();
  if (now - fbLastSample > MAINS_GAP_MS) fbLastLow = now;   // mintavételi szünet: nincs adat
  fbLastSample = now;
  if (digitalRead(PIN_FB1) == LOW) {
    fbLastLow = now;
    if (fbLow < 255) fbLow++;
  }
  if (!fbGraceOver && now - fbCmdT >= MAINS_OFF_GRACE_MS) fbGraceOver = true;
  if (now - fbWinStart < MAINS_WIN_MS) return;
  fbWinStart = now;
  bool present = (fbLow >= MAINS_MIN_LOW);
  fbCount = fbLow;
  fbLow = 0;
  fbPresent = present;
  fbValid = true;
  if (on && fbConfirmed && present) { sessSum += fbCount; sessN++; }
  bool mismatch = (present != on);
  if (mismatch && !on && !fbGraceOver) mismatch = false;
  if (mismatch) {
    if (!fbBad) { fbBad = true; fbBadSince = now; }
  } else {
    fbBad = false;
    if (on && !fbConfirmed) {
      fbConfirmed = true;
      slog("RL1 kimenet: halozat OK (%lu ms)\n", now - fbCmdT);
    }
  }
}

bool mainsLost() {
  return MAINS_CHECK && on && fbConfirmed && (millis() - fbLastLow) >= MAINS_LOSS_MS;
}

int mainsFault() {
  if (!MAINS_CHECK) return F_NONE;
  if (mainsLost()) return F_MAINSLOST;
  if (fbBad && (millis() - fbBadSince) >= MAINS_FAULT_MS) return on ? F_NOMAINS : F_MAINSOFF;
  return F_NONE;
}

// ---- Állapotfigyelés ----
void statsSave() {
  if (!stDirty) return;
  if (!prefs.begin("relay", false)) return;
  if (prefs.putBytes("stats", &st, sizeof st) == sizeof st) stDirty = false;
  prefs.end();
  stLastSave = millis();
}

void statsAccount() {   // bekapcsolt idő hozzáadása
  if (!stCounting) return;
  unsigned long now = millis();
  stAcctMs += now - stAcctT;
  stAcctT = now;
  if (stAcctMs >= 1000) { st.onSeconds += stAcctMs / 1000; stAcctMs %= 1000; stDirty = true; }
}

void statsFold() {   // a bekapcsolás opto átlagának beszámítása
  if (sessN >= OPTO_MIN_WINDOWS) {
    float m = (float)sessSum / sessN;
    lastSessMean = m;
    st.optoAvg = (st.optoAvg <= 0) ? m : (0.8f * st.optoAvg + 0.2f * m);
    if (st.optoBaseN < OPTO_BASE_SESSIONS) {
      st.optoBase = (st.optoBase * st.optoBaseN + m) / (st.optoBaseN + 1);
      st.optoBaseN++;
    }
    stDirty = true;
  }
  sessSum = 0;
  sessN = 0;
}

// figyelmeztetések szövege (üres = rendben)
int statsWarnings(char *buf, size_t len) {
  int n = 0;
  buf[0] = 0;
  if (st.relayCount >= (uint64_t)RELAY_LIFE * RELAY_WARN_PCT / 100) {
    n++;
    snprintf(buf + strlen(buf), len - strlen(buf), " rele kopik (%lu kapcsolas);", (unsigned long)st.relayCount);
  }
  if (MAINS_CHECK && st.optoAvg > 0) {
    if (st.optoAvg < OPTO_MIN_AVG) {
      n++;
      snprintf(buf + strlen(buf), len - strlen(buf), " opto jele gyenge (%u);", (unsigned)(st.optoAvg + 0.5f));
    } else if (st.optoBaseN >= OPTO_BASE_SESSIONS && st.optoAvg < st.optoBase * (100 - OPTO_DROP_PCT) / 100) {
      n++;
      snprintf(buf + strlen(buf), len - strlen(buf), " opto oregszik (%u%%-kal az alapertek alatt);",
               (unsigned)(100 - st.optoAvg * 100 / st.optoBase + 0.5f));
    }
  }
  return n;
}

int lastWarnCount = -1;   // új figyelmeztetésnél esemény a Home Assistant felé
void checkWarnings() {
  char w[120];
  int n = statsWarnings(w, sizeof w);
  if (lastWarnCount >= 0 && n > lastWarnCount) netEvent("warning", "Öregedés:%s", w);
  lastWarnCount = n;
}

void printHealth() {
  statsAccount();
  unsigned long h10 = st.onSeconds / 360;   // óra x10
  slogc("ALLAPOTFIGYELES:\n");
  slogc("  Uzemido: %lu,%lu ora, bekapcsolasok: %lu (RL2: %lu)\n", h10 / 10, h10 % 10,
        (unsigned long)st.onCount, (unsigned long)st.rl2Count);
  slogc("  Rele: %lu / %lu kapcsolas (%lu %%)\n", (unsigned long)st.relayCount, (unsigned long)RELAY_LIFE,
        (unsigned long)((uint64_t)st.relayCount * 100 / RELAY_LIFE));
  slogc("  Halozatkieses: %lu, hiba: %lu, aramszunet utani indulas: %lu\n", (unsigned long)st.mainsLoss,
        (unsigned long)st.faults, (unsigned long)st.powerUps);
  if (MAINS_CHECK) {
    unsigned a = (unsigned)(st.optoAvg * 10 + 0.5f), b = (unsigned)(st.optoBase * 10 + 0.5f),
             l = (unsigned)(lastSessMean * 10 + 0.5f);
    if (st.optoBaseN == 0)
      slogc("  Opto LOW minta: meg nincs adat (legalabb ~10 s bekapcsolas kell)\n");
    else if (st.optoBaseN >= OPTO_BASE_SESSIONS)
      slogc("  Opto LOW minta: atlag %u,%u, alapertek %u,%u, utolso %u,%u\n", a / 10, a % 10, b / 10, b % 10, l / 10, l % 10);
    else
      slogc("  Opto LOW minta: atlag %u,%u, alapertek: gyujtes (%lu / %u bekapcsolas)\n", a / 10, a % 10,
            (unsigned long)st.optoBaseN, (unsigned)OPTO_BASE_SESSIONS);
  }
  char w[120];
  if (statsWarnings(w, sizeof w)) slogc("  FIGYELMEZTETES:%s\n", w);
  else slogc("  Allapot: rendben\n");
}

void setState(bool s, const char *src) {
  if (s == on) return;
  on = s;
  offPending = false;
  offByBtn = false;
  onReqBtn = false;
  onReqTrig = false;
  if (on) {
    digitalWrite(PIN_RL1, HIGH);
    onTime = millis();
    ledBase = true;
    mainsCmd();
    slog("RL1 BE (%s), RL2 %lu ms mulva\n", src, RL2_DELAY_MS);
    netEvent("on", "Bekapcsolás (%s)", src);
    st.onCount++;
    st.relayCount++;
    stAcctT = onTime;
    stCounting = true;
    sessSum = 0;
    sessN = 0;
    stDirty = true;
    statsSave();
  } else {
    digitalWrite(PIN_RL2, LOW);
    digitalWrite(PIN_RL1, LOW);
    rl2On = false;
    rl2Settled = false;
    offTime = millis();
    minOffActive = true;
    ledBase = false;
    ledFlashes = 0;
    mainsCmd();
    slog("RL1+RL2 KI (%s)\n", src);
    netEvent("off", "Kikapcsolás (%s)", src);
    statsAccount();
    stCounting = false;
    statsFold();
    statsSave();
    checkWarnings();
  }
}

// ---- Bemenetek: szűrés, beragadás- és zajfigyelés ----
struct Input {
  uint8_t pin;
  const char *name;
  unsigned long stuckMs;     // 0 = nincs beragadás-figyelés
  uint8_t changeLimit;
  bool stable;
  bool raw;
  unsigned long t;           // utolsó nyers váltás
  unsigned long lowSince;
  unsigned long winStart;
  uint8_t changes;
};

Input inputs[2] = {
  { (uint8_t)PIN_BTN,  "GOMB",    BTN_STUCK_MS, BTN_CHANGE_LIMIT, HIGH, HIGH, 0, 0, 0, 0 },
  { (uint8_t)PIN_TRIG, "TRIGGER", 0,            CHANGE_LIMIT,     HIGH, HIGH, 0, 0, 0, 0 }
};
Input &btn  = inputs[IN_BTN];
Input &trig = inputs[IN_TRIG];

bool updateInput(int idx) {   // true: a szűrt állapot változott
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

void resyncInputs() {   // tiszta lap: bemenetek újraolvasása, számlálók nullázása
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
  pressConsumed = (btn.stable == LOW);   // a már nyomott gomb nem új nyomás
  fbBad = false;
}

// ---- Hibakezelés ----
void saveFault(int f) {   // csak az ellenőrizhető hardverhibát mentjük (beragadt gomb, hálózat KI relénél)
  if (f != F_NONE && f != F_STUCK && f != F_MAINSOFF) return;
  if (f == storedFault) return;
  if (!prefs.begin("relay", false)) return;
  if (prefs.putUChar("fault", (uint8_t)f) > 0) storedFault = f;
  prefs.end();
}

const char *faultName(int f) {
  switch (f) {
    case F_STUCK:     return "beragadt gomb / zárlat";
    case F_CHATTER:   return "túl gyakori váltás (zaj, kontakthiba)";
    case F_PRESSES:   return "3 vagy több rövid gombnyomás";
    case F_NOMAINS:   return "nincs hálózat az RL1 után";
    case F_MAINSOFF:  return "hálózat van kikapcsolt relénél (összehegedt érintkező?)";
    case F_MAINSLOST: return "hálózatkiesés";
    default:          return "nincs";
  }
}

void startFault(int idx, int f) {
  onReqBtn = false;
  onReqTrig = false;
  if (f == F_MAINSLOST) st.mainsLoss++;
  else st.faults++;
  stDirty = true;
  if (idx == IN_FB1) {   // hálózati hiba: relék azonnal KI
    bool retry = on && !offPending;
    setState(false, (f == F_MAINSLOST) ? "halozatkieses" : "halozati hiba");
    if (f == F_MAINSLOST && retry) {
      onReqTrig = true;
      onReqSrc = "trigger halozatkieses utan";
    }
  }
  lastFault = f;
  saveFault(f);
  if (f == F_MAINSLOST) netEvent("mains_loss", "Hálózatkiesés bekapcsolt állapotban%s", onReqTrig ? ", újrakapcsolás aktív triggerre" : "");
  else netEvent("fault", "%s: %s", idx == IN_FB1 ? "RL1 kimenet" : inputs[idx].name, faultName(f));
  faultActive = true;
  faultStart = millis();
  ledFaultBlink = true;
  ledFlashes = 0;
  ledT = faultStart;
  if (idx == IN_FB1) {
    if (f == F_MAINSLOST)
      slogc("!!! HALOZATKIESES: relek AZONNAL KI%s\n",
            onReqTrig ? ", MIN_OFF_MS utan aktiv triggerre ujra be" : " (kikapcsolas volt fuggoben: nincs ujrakapcsolas)");
    else
      slogc("!!! RL1 kimenet HIBA: %s -> relek KI\n",
            (f == F_NOMAINS) ? "nincs halozat (az RL1 nem huzott be, vagy nincs halozat)"
                             : "halozat van KIKAPCSOLT relenel (osszehegedt erintkezo?)");
    return;
  }
  const Input &in = inputs[idx];
  slogc("!!! %s HIBA: %s -> LED villog, majd relek KI\n", in.name,
        (f == F_STUCK) ? "tartosan LOW (beragadt / zarlat)"
        : (f == F_PRESSES) ? "3 vagy tobb rovid nyomas"
                           : "tul gyakori valtas (zaj, rossz kontaktus)");
}

void endFault() {
  ledFaultBlink = false;
  setState(false, "hiba");
  statsSave();
  ledBase = false;
  ledFlashes = 0;
  faultActive = false;
  netStateDirty = true;
  resyncInputs();
  if (lastFault == F_MAINSLOST) {   // kiesés: nincs tiltás, csak a minimális kikapcsolt idő
    lastFault = F_NONE;
    slogc("Halozatkieses: villogas vege\n");
    return;
  }
  faultHold = true;
  lockoutPending = true;
  lockoutStart = millis();
  lockoutDur = FAULT_LOCKOUT_MS;
  slogc("Hiba vege: relek KI, %lu ms-ig nincs bekapcsolas\n", FAULT_LOCKOUT_MS);
}

// ---- Be / ki kérések ----
unsigned long timeLeft(unsigned long start, unsigned long dur) {   // túlcsordulás-biztos hátralévő idő
  unsigned long el = millis() - start;
  return (el >= dur) ? 0 : (dur - el);
}

unsigned long minOffLeft() {
  if (on || !minOffActive) return 0;
  return timeLeft(offTime, MIN_OFF_MS);
}

unsigned long lockoutLeft() {
  return lockoutPending ? timeLeft(lockoutStart, lockoutDur) : 0;
}

void requestOff(const char *src, bool byBtn) {   // az RL2 legalább RL2_MIN_ON_MS-ig behúzva marad
  if (!on) return;
  if (rl2Settled) {
    setState(false, src);
    return;
  }
  if (byBtn) offByBtn = true;
  if (offPending) return;
  offPending = true;
  if (rl2On) {
    slog("Kikapcsolas (%s) %lu ms mulva\n", src, timeLeft(rl2Time, RL2_MIN_ON_MS));
  } else {
    slog("Kikapcsolas (%s) az RL2 utan %lu ms-mal\n", src, RL2_MIN_ON_MS);
  }
}

bool btnBusy() { return onReqBtn || (offPending && offByBtn); }   // függő gombos kérés alatt a gomb hatástalan

void switchOn(const char *src, bool manual) {
  if (on || faultActive) return;
  if (lockoutLeft() > 0) {   // hiba utáni tiltás: a kérés elvész
    slog("Bekapcsolas tiltva (%s): meg %lu ms\n", src, lockoutLeft());
    return;
  }
  unsigned long left = minOffLeft();
  if (left > 0) {   // minimális kikapcsolt idő: a kérés megmarad
    if (manual) onReqBtn = true;
    else onReqTrig = true;
    onReqSrc = src;
    slog("Bekapcsolas (%s) %lu ms mulva%s\n", src, left, manual ? "" : ", ha a trigger meg aktiv");
    return;
  }
  if (faultHold) {
    resyncInputs();
    faultHold = false;
  }
  if (MAINS_CHECK && fbValid && fbPresent && fbGraceOver) {
    startFault(IN_FB1, F_MAINSOFF);
    return;
  }
  setState(true, src);
  if (manual && trigOff) ledFlash(2);   // tiltott trigger + kézi bekapcsolás
}

void toggleTrigOff() {
  trigOff = !trigOff;
  trig.changes = 0;
  trig.winStart = millis();
  bool saved = prefs.begin("relay", false);
  if (saved) {
    saved = prefs.putBool("trigOff", trigOff) > 0;
    prefs.end();
  }
  netStateDirty = true;
  ledFlash(trigOff ? 2 : 4);   // tiltva: 2, engedélyezve: 4 villanás
  slogc("TRIGGER %s%s\n", trigOff ? "TILTVA" : "ENGEDELYEZVE", saved ? "" : " (FIGYELEM: NVS mentes sikertelen)");
}

// Gomb: hosszú = be / ki; 2 rövid = trigger tiltás / engedés; 3+ rövid = hiba
void buttonLogic(bool changed) {
  unsigned long now = millis();
  if (btnBusy()) {
    if (changed && btn.stable == LOW) {
      pressConsumed = true;
      slog("Gomb hatastalan: fuggo %s\n", onReqBtn ? "bekapcsolas" : "kikapcsolas");
    }
    shortCount = 0;
    burstTaint = false;
    return;
  }
  if (changed) {
    if (btn.stable == LOW) {
      pressStart = btn.t;
      pressConsumed = false;
    } else if (!pressConsumed) {
      shortCount++;
      lastShortRel = now;
      if (shortCount >= SHORT_FAULT_COUNT) {
        shortCount = 0;
        burstTaint = true;
        if (!faultHold) { startFault(IN_BTN, F_PRESSES); return; }
      }
    } else {
      lastShortRel = now;
    }
  }
  if (btn.stable == LOW && !pressConsumed && (now - pressStart) >= LONG_PRESS_MS) {
    pressConsumed = true;
    shortCount = 0;
    burstTaint = false;
    if (on) requestOff("gomb", true);
    else switchOn("gomb", true);
  }
  if (shortCount > 0 && btn.raw == HIGH && btn.stable == HIGH && (now - lastShortRel) > DOUBLE_GAP_MS) {
    uint8_t n = shortCount;
    shortCount = 0;
    if (burstTaint) burstTaint = false;
    else if (n == 2) toggleTrigOff();
  }
  if (burstTaint && shortCount == 0 && btn.stable == HIGH && now - lastShortRel > DOUBLE_GAP_MS) burstTaint = false;   // új sorozat
}

// ---- Home Assistant: WiFi + MQTT külön taskban; a vezérlés csak nem blokkoló sorba ír ----
#if NET_ENABLED
struct NetEvt { char type[12]; char text[100]; };
QueueHandle_t netQ = nullptr;
esp_mqtt_client_handle_t mqtt = nullptr;
volatile bool mqttUp = false;
volatile bool netNeedDisc = false;
const char *T_STATE  = NET_ID "/state";
const char *T_EVENT  = NET_ID "/event";
const char *T_CMD    = NET_ID "/power/set";
const char *T_STATUS = NET_ID "/status";

void netEvent(const char *type, const char *fmt, ...) {
  netStateDirty = true;
  if (!netQ) return;
  NetEvt e;
  strncpy(e.type, type, sizeof e.type - 1);
  e.type[sizeof e.type - 1] = 0;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.text, sizeof e.text, fmt, ap);
  va_end(ap);
  xQueueSend(netQ, &e, 0);   // tele sornál elvész, nem vár
}

void netPub(const char *topic, const char *payload, bool retain) {
  if (mqtt && mqttUp) esp_mqtt_client_enqueue(mqtt, topic, payload, 0, 1, retain ? 1 : 0, true);
}

void mqttHandler(void *, esp_event_base_t, int32_t id, void *data) {
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
  if (id == MQTT_EVENT_CONNECTED) {
    mqttUp = true;
    netNeedDisc = true;
    esp_mqtt_client_subscribe_single(mqtt, T_CMD, 1);
  } else if (id == MQTT_EVENT_DISCONNECTED) {
    mqttUp = false;
  } else if (id == MQTT_EVENT_DATA && !e->retain && e->topic_len == (int)strlen(T_CMD) &&
             strncmp(e->topic, T_CMD, e->topic_len) == 0) {
    if (e->data_len == 2 && strncmp(e->data, "ON", 2) == 0) remoteOnReq = true;
    else if (e->data_len == 3 && strncmp(e->data, "OFF", 3) == 0) remoteOffReq = true;
  }
}

// Home Assistant MQTT discovery: egy entitás
void netDisc(const char *comp, const char *obj, const char *name, const char *extra) {
  char topic[80], js[512];
  snprintf(topic, sizeof topic, "homeassistant/%s/" NET_ID "/%s/config", comp, obj);
  snprintf(js, sizeof js,
           "{\"name\":%s,\"uniq_id\":\"" NET_ID "_%s\",\"stat_t\":\"" NET_ID "/state\","
           "\"val_tpl\":\"{{value_json.%s}}\",\"avty_t\":\"" NET_ID "/status\"%s,"
           "\"dev\":{\"ids\":[\"" NET_ID "\"],\"name\":\"Erősítő\",\"mf\":\"DIY\",\"mdl\":\"XIAO ESP32-C3 bekapcsoló\"}}",
           name, obj, obj, extra);
  netPub(topic, js, true);
}

void netDiscovery() {
  netDisc("switch", "power", "null", ",\"cmd_t\":\"" NET_ID "/power/set\",\"ic\":\"mdi:amplifier\"");
  netDisc("binary_sensor", "rl2", "\"NTC áthidalva (RL2)\"", ",\"ent_cat\":\"diagnostic\"");
  netDisc("binary_sensor", "trigger", "\"Trigger\"", "");
  netDisc("binary_sensor", "problem", "\"Probléma\"", ",\"dev_cla\":\"problem\"");
  netDisc("sensor", "fault", "\"Hiba\"", "");
  netDisc("sensor", "warning", "\"Figyelmeztetés\"", "");
  netDisc("sensor", "on_hours", "\"Üzemidő\"", ",\"unit_of_meas\":\"h\",\"dev_cla\":\"duration\",\"stat_cla\":\"total_increasing\"");
  netDisc("sensor", "on_count", "\"Bekapcsolások\"", ",\"stat_cla\":\"total_increasing\"");
  netDisc("sensor", "relay_count", "\"Relé kapcsolások\"", ",\"stat_cla\":\"total\",\"ent_cat\":\"diagnostic\"");
  netDisc("sensor", "mains_loss", "\"Hálózatkiesések\"", ",\"stat_cla\":\"total_increasing\"");
  netDisc("sensor", "faults", "\"Hibák\"", ",\"stat_cla\":\"total_increasing\"");
  netDisc("sensor", "power_ups", "\"Áramszünet utáni indulások\"", ",\"stat_cla\":\"total_increasing\",\"ent_cat\":\"diagnostic\"");
  netDisc("sensor", "opto_now", "\"Optó minta\"", ",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\"");
  netDisc("sensor", "opto_avg", "\"Optó átlag\"", ",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\"");
  netDisc("sensor", "opto_base", "\"Optó alapérték\"", ",\"ent_cat\":\"diagnostic\"");
  netDisc("sensor", "rssi", "\"WiFi jel\"", ",\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"ent_cat\":\"diagnostic\"");
}

void netState() {
  char w[120], js[640];
  statsWarnings(w, sizeof w);
  unsigned long h10 = st.onSeconds / 360;
  unsigned a = (unsigned)(st.optoAvg * 10 + 0.5f), b = (unsigned)(st.optoBase * 10 + 0.5f);
  bool flt = faultActive || lockoutPending;
  snprintf(js, sizeof js,
           "{\"power\":\"%s\",\"rl2\":\"%s\",\"trigger\":\"%s\",\"trig_enabled\":%s,\"hold\":%s,"
           "\"fault\":\"%s\",\"warning\":\"%s\",\"problem\":\"%s\",\"on_hours\":%lu.%lu,\"on_count\":%lu,"
           "\"relay_count\":%lu,\"mains_loss\":%lu,\"faults\":%lu,\"power_ups\":%lu,\"opto_now\":%u,"
           "\"opto_avg\":%u.%u,\"opto_base\":%u.%u,\"rssi\":%d}",
           on ? "ON" : "OFF", rl2On ? "ON" : "OFF", trig.stable == LOW ? "ON" : "OFF", trigOff ? "false" : "true",
           powerHold ? "true" : "false", flt ? faultName(lastFault) : "nincs", w[0] ? w + 1 : "nincs",
           (flt || w[0]) ? "ON" : "OFF", h10 / 10, h10 % 10, (unsigned long)st.onCount, (unsigned long)st.relayCount,
           (unsigned long)st.mainsLoss, (unsigned long)st.faults, (unsigned long)st.powerUps, (unsigned)fbCount,
           a / 10, a % 10, b / 10, b % 10, (int)WiFi.RSSI());
  netPub(T_STATE, js, true);
}

void netTask(void *) {
  WiFi.persistent(false);   // ne írja minden induláskor a flash-be
  WiFi.setHostname(NET_ID);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);   // kisebb áramcsúcs; a XIAO C3-on így stabilabb is
  bool started = false;
  unsigned long lastState = 0;
  for (;;) {
    if (!started && WiFi.status() == WL_CONNECTED) {
      esp_mqtt_client_config_t cfg = {};
      cfg.broker.address.uri = MQTT_URI;
      cfg.credentials.username = MQTT_USER;
      cfg.credentials.authentication.password = MQTT_PASS;
      cfg.credentials.client_id = NET_ID;
      cfg.session.last_will.topic = T_STATUS;
      cfg.session.last_will.msg = "offline";
      cfg.session.last_will.qos = 1;
      cfg.session.last_will.retain = 1;
      cfg.session.keepalive = 30;
      cfg.network.reconnect_timeout_ms = 10000;
      mqtt = esp_mqtt_client_init(&cfg);
      if (mqtt) {
        esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, mqttHandler, nullptr);
        esp_mqtt_client_start(mqtt);
        started = true;
      }
    }
    if (mqttUp) {
      if (netNeedDisc) {
        netNeedDisc = false;
        netPub(T_STATUS, "online", true);
        netDiscovery();
        netStateDirty = true;
      }
      NetEvt e;
      while (xQueueReceive(netQ, &e, 0) == pdTRUE) {
        char js[160];
        snprintf(js, sizeof js, "{\"type\":\"%s\",\"text\":\"%s\"}", e.type, e.text);
        netPub(T_EVENT, js, false);
      }
      if (netStateDirty || millis() - lastState >= NET_STATE_MS) {
        netStateDirty = false;
        lastState = millis();
        netState();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void netStart() {
  netQ = xQueueCreate(16, sizeof(NetEvt));
  if (netQ) xTaskCreate(netTask, "net", 6144, nullptr, 1, nullptr);
}

void netStatusLine() {
  slogc("  Home Assistant: WiFi %s (%d dBm), MQTT %s\n", WiFi.status() == WL_CONNECTED ? "OK" : "nincs",
        (int)WiFi.RSSI(), mqttUp ? "OK" : "nincs");
}
#else
void netEvent(const char *, const char *, ...) {}
void netStart() {}
void netStatusLine() {}
#endif

void printHelp() {
  slogc("Parancsok: s = allapot, h = allapotfigyeles, B = opto alapertek torles (optocsere), R = rele szamlalo nullazas (relecsere)\n");
}

void printStatus() {
  char lk[48] = "";
  if (powerHold)
    snprintf(lk, sizeof lk, " [aramszunet utan: %lu s]", timeLeft(holdStart, holdDur) / 1000);
  else if (lockoutLeft() > 0)
    snprintf(lk, sizeof lk, " [tiltas: %lu ms]", lockoutLeft());
  char rq[48] = "";
  if (onReqBtn || onReqTrig)
    snprintf(rq, sizeof rq, " [bekapcs. %s, %lu ms]", onReqBtn ? "gomb" : "trigger", minOffLeft());
  slogc("ALLAPOT: RL1=%s RL2=%s LED=%s%s%s%s\n", on ? "BE" : "KI", rl2On ? "BE" : "KI",
        ledFaultBlink ? "hiba" : (ledBase ? "be" : "ki"), offPending ? " [kikapcs. fuggoben]" : "", rq, lk);
  slogc("  GOMB: %s, TRIGGER: %s (%s)\n", btn.stable == LOW ? "nyomva" : "fent",
        trig.stable == LOW ? "aktiv" : "nincs", trigOff ? "TILTVA" : "engedelyezve");
  if (MAINS_CHECK)
    slogc("  RL1 kimenet: %s%s, LOW minta: %u (kuszob %u)\n",
          !fbValid ? "meres..." : (fbPresent ? "halozat VAN" : "halozat nincs"),
          fbBad ? " (ELTERES)" : "", (unsigned)fbCount, (unsigned)MAINS_MIN_LOW);
  char w[120];
  if (statsWarnings(w, sizeof w)) slogc("  FIGYELMEZTETES:%s ('h' = reszletek)\n", w);
  netStatusLine();
}

const char *resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "tapfeltoltes";
    case ESP_RST_EXT:      return "kulso reset";
    case ESP_RST_SW:       return "szoftveres ujrainditas";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    default:               return "egyeb";
  }
}

void setup() {
  digitalWrite(PIN_RL1, LOW);
  digitalWrite(PIN_RL2, LOW);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_RL1, OUTPUT);
  pinMode(PIN_RL2, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_RL1, LOW);   // core 3.x: a pinMode előtti írás hatástalan
  digitalWrite(PIN_RL2, LOW);
  digitalWrite(PIN_LED, LOW);

  Serial.begin(115200);
  logQ = xQueueCreate(24, sizeof(LogMsg));   // boot üzenetekhez is elég; tele sornál az üzenet elvész, a vezérlés nem áll meg
  if (logQ) xTaskCreate(logTask, "slog", 4096, nullptr, 1, nullptr);

  bool nvsOk = prefs.begin("relay", false);
  if (nvsOk) {
    trigOff = prefs.getBool("trigOff", false);
    storedFault = prefs.getUChar("fault", 0);
    if (prefs.getBytes("stats", &st, sizeof st) != sizeof st || st.ver != STATS_VER) {
      memset(&st, 0, sizeof st);
      st.ver = STATS_VER;
    }
    prefs.end();
  }
  if (storedFault != F_NONE && storedFault != F_STUCK && storedFault != F_MAINSOFF) {   // régi / nem ellenőrizhető kód
    saveFault(F_NONE);
    storedFault = F_NONE;
  }

  pinMode(PIN_BTN,  INPUT_PULLUP);
  pinMode(PIN_TRIG, INPUT_PULLUP);
  if (MAINS_CHECK) {
    pinMode(PIN_FB1, INPUT);   // külső 43k felhúzó
    mainsCmd();
    fbGraceOver = true;   // induláskor nincs türelmi idő
  }
  for (int i = 0; i < 50; i++) {
    mainsUpdate();
    delay(1);
  }
  resyncInputs();

  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = WDT_MS;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic  = true;
  if (esp_task_wdt_init(&cfg) == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&cfg);
  esp_task_wdt_add(nullptr);

  esp_reset_reason_t rr = esp_reset_reason();
  bool crashReset = (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT || rr == ESP_RST_PANIC ||
                     (rr == ESP_RST_BROWNOUT && !BROWNOUT_IS_POWERLOSS));
  bool powerLoss = (rr == ESP_RST_POWERON) || (rr == ESP_RST_BROWNOUT && BROWNOUT_IS_POWERLOSS);
  unsigned long holdMs = powerLoss ? POWERUP_HOLD_MS : holdLoad();   // reset a várakozás alatt: folytatódik
  if (holdMs > 0) {
    powerHold = true;
    holdStart = millis();
    holdDur = holdMs;
    holdSave(holdMs);
    lastHoldSave = millis();
  } else {
    holdClear();
  }

  slogc("\n=== Erosito bekapcsolo indul (%s) ===\n", resetName(rr));
  slogc("TRIGGER: %s, %s; halozatfigyeles: %s\n", trig.stable == LOW ? "aktiv" : "nincs",
        trigOff ? "TILTVA" : "engedelyezve", MAINS_CHECK ? "BE" : "KI");
  if (!nvsOk) slogc("FIGYELEM: NVS hiba, alapertelmezett beallitasok\n");
  if (powerHold) slogc("Aramszunet utan: %lu s varakozas\n", holdMs / 1000);
  if (powerLoss) { st.powerUps++; stDirty = true; statsSave(); }
  printHelp();
  netStart();
  netEvent("boot", "Indulás: %s%s", resetName(rr), powerHold ? ", áramszünet utáni várakozás" : "");
  checkWarnings();
  if (lastWarnCount > 0) netEvent("warning", "Öregedési figyelmeztetés induláskor");
  {
    char w[120];
    if (statsWarnings(w, sizeof w)) slogc("FIGYELMEZTETES:%s ('h' = reszletek)\n", w);
  }

  if (storedFault != F_NONE) {   // mentett hiba: a tiltás végén ellenőrzi az okot
    unsigned long wait = powerHold ? holdMs : FAULT_LOCKOUT_MS;
    lastFault = storedFault;
    faultHold = true;
    lockoutPending = true;
    lockoutStart = millis();
    lockoutDur = wait;
    slogc("Mentett hiba (%s): %lu ms mulva ellenorzes\n",
          storedFault == F_STUCK ? "beragadt gomb" : "halozat KI relenel", wait);
  } else if (!powerHold && !trigOff && trig.stable == LOW && (crashReset || TRIGGER_ON_BOOT)) {
    switchOn(crashReset ? "fagyas utan" : "indulaskor aktiv trigger", false);
  }
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();

  bool btnChanged  = updateInput(IN_BTN);
  bool trigChanged = updateInput(IN_TRIG);
  mainsUpdate();
  if (rl2On && !rl2Settled && now - rl2Time >= RL2_MIN_ON_MS) rl2Settled = true;   // állapotok rögzítése: régi
  if (minOffActive && !on && now - offTime >= MIN_OFF_MS) minOffActive = false;     // időbélyeg nem zavarhat be

  if (faultActive) {   // hibavillogás
    if (mainsLost()) {   // bemenet-hiba alatt (relék még BE) is azonnal KI
      setState(false, "halozatkieses hiba alatt");
      slogc("!!! HALOZATKIESES hiba alatt: relek AZONNAL KI\n");
    }
    if (btnChanged) lastShortRel = now;
    remoteOnReq = remoteOffReq = false;   // hiba alatt a távoli parancs elvész
    if (now - faultStart >= FAULT_BLINK_MS) endFault();
  } else if (powerHold) {   // áramszünet utáni várakozás: trigger hatástalan, a gomb egyszer bekapcsolhat
    if (timeLeft(holdStart, holdDur) > 0) {
      if (now - lastHoldSave >= 1000) {
        lastHoldSave = now;
        holdSave(timeLeft(holdStart, holdDur));
      }
      int mf = mainsFault();
      if (on && mf != F_NONE) startFault(IN_FB1, mf);
      if (!faultActive && !holdBtnUsed) {
        if (btnChanged && btn.stable == LOW) {
          pressStart = btn.t;
          pressConsumed = false;
        }
        if (btn.stable == LOW && !pressConsumed && (now - pressStart) >= LONG_PRESS_MS) {
          pressConsumed = true;
          holdBtnUsed = true;
          switchOn("gomb aramszunet utan", true);
          if (on || onReqBtn) slogc("Gomb a varakozas alatt elfogadva: tovabbi gombnyomas a varakozas vegeig hatastalan\n");
        }
      }
      if (remoteOnReq) {   // távoli bekapcsolás: mint a gomb, a várakozás alatt is egyszer
        remoteOnReq = false;
        if (!faultActive && !on && !holdBtnUsed) {
          holdBtnUsed = true;
          switchOn("tavoli aramszunet utan", true);
        }
      }
      if (remoteOffReq) {   // távoli kikapcsolás mindig engedett
        remoteOffReq = false;
        onReqBtn = false;
        requestOff("tavoli", true);
      }
      if (!faultActive && !on && onReqBtn && minOffLeft() == 0) {
        onReqBtn = false;
        switchOn("gomb aramszunet utan", true);
      }
    } else {
      powerHold = false;
      netStateDirty = true;
      holdClear();
      resyncInputs();
      slogc("Aramszunet utani varakozas vege\n");
      if (!lockoutPending && TRIGGER_ON_BOOT && !trigOff && trig.stable == LOW)
        switchOn("aktiv trigger aramszunet utan", false);
    }
  } else {
    int mf = mainsFault();
    if (mf != F_NONE && (on || !faultHold)) startFault(IN_FB1, mf);
    if (!faultActive && !faultHold) {
      int f = checkFault(IN_BTN);
      if (f == F_NONE && !trigOff) {
        f = checkFault(IN_TRIG);
        if (f != F_NONE) startFault(IN_TRIG, f);
      } else if (f != F_NONE) {
        startFault(IN_BTN, f);
      }
    }
    if (!faultActive) buttonLogic(btnChanged);
    if (remoteOnReq) {   // távoli bekapcsolás: mint a gomb
      remoteOnReq = false;
      if (!faultActive && !on && !btnBusy()) switchOn("tavoli", true);
    }
    if (remoteOffReq) {   // távoli kikapcsolás: a függő bekapcsolást is törli
      remoteOffReq = false;
      if (on) requestOff("tavoli", true);
      else if (onReqBtn || onReqTrig) { onReqBtn = onReqTrig = false; slog("Fuggo bekapcsolas torolve (tavoli)\n"); }
    }
    if (trigChanged) netStateDirty = true;
    if (!faultActive && trigChanged && !trigOff) {   // trigger: aktív élre be, inaktív élre ki
      if (trig.stable == LOW) {
        if (offPending && !offByBtn) {
          offPending = false;
          slog("Trigger ujra aktiv: kikapcsolas elmarad\n");
        }
        switchOn("trigger", false);
      } else {
        requestOff("trigger", false);
      }
    }
    if (!faultActive && !on && (onReqBtn || onReqTrig) && minOffLeft() == 0) {   // függő bekapcsolás
      bool byBtn = onReqBtn;
      const char *src = onReqSrc;
      onReqBtn = false;
      onReqTrig = false;
      if (byBtn) switchOn("gomb", true);
      else if (!trigOff && trig.stable == LOW) switchOn(src, false);
      else slog("Fuggo bekapcsolas elmarad: a trigger nem aktiv\n");
    }
    if (!faultActive && lockoutPending && lockoutLeft() == 0) {   // tiltás vége
      lockoutPending = false;
      netStateDirty = true;
      bool offMains = MAINS_CHECK && fbValid && fbPresent;
      if (btn.stable == LOW || btn.raw == LOW) {
        slog("Tiltas vege: a gomb meg nyomva\n");
        startFault(IN_BTN, F_STUCK);
      } else if (offMains) {
        slog("Tiltas vege: RL1 kimeneten halozat van KI relenel\n");
        startFault(IN_FB1, F_MAINSOFF);
      } else {
        int was = lastFault;
        lastFault = F_NONE;
        saveFault(F_NONE);
        if (was == F_NOMAINS) slog("Nincs automatikus ujrakapcsolas: gomb vagy uj trigger kell\n");
        else if (AUTO_ON_AFTER_FAULT && !trigOff && trig.stable == LOW) switchOn("aktiv trigger hiba utan", false);
      }
    }
  }

  // RL2: késleltetve, és csak ha az RL1 kimenetén már van hálózat
  if (on && !rl2On && !faultActive && (!MAINS_CHECK || fbConfirmed) && now - onTime >= RL2_DELAY_MS) {
    digitalWrite(PIN_RL2, HIGH);
    rl2On = true;
    rl2Time = millis();
    rl2Settled = false;
    st.rl2Count++;
    stDirty = true;
    netStateDirty = true;
    slog("RL2 BE\n");
  }
  if (offPending && rl2Settled && !faultActive) setState(false, "fuggo kikapcsolas");

  ledUpdate();

  if (statusReq) {
    statusReq = false;
    if (millis() - lastStatus >= STATUS_MIN_MS) { lastStatus = millis(); printStatus(); }
  }
  if (helpReq) {
    helpReq = false;
    if (millis() - lastStatus >= STATUS_MIN_MS) { lastStatus = millis(); printHelp(); }
  }
  if (healthReq) {
    healthReq = false;
    if (millis() - lastStatus >= STATUS_MIN_MS) { lastStatus = millis(); printHealth(); }
  }
  if (baseReq) {   // optocsere után: új alapérték az következő bekapcsolásokból
    baseReq = false;
    st.optoAvg = 0;
    st.optoBase = 0;
    st.optoBaseN = 0;
    lastSessMean = 0;
    stDirty = true;
    statsSave();
    slogc("Opto alapertek torolve: az kovetkezo %u bekapcsolasbol ujra kepzodik\n", (unsigned)OPTO_BASE_SESSIONS);
  }
  if (relayReq) {   // relécsere után
    relayReq = false;
    st.relayCount = 0;
    stDirty = true;
    statsSave();
    slogc("Rele szamlalo nullazva\n");
  }
  if (on && now - stLastSave >= STATS_SAVE_MS) {   // bekapcsolva óránként
    statsAccount();
    statsFold();
    statsSave();
    stLastSave = now;
    checkWarnings();
  }
  logWindow();
  delay(1);
}
