// XIAO ESP32-C3 / ESP32-S3 erősítő-bekapcsoló: gomb + 12 V trigger, RL1 (NTC lágyindítás) majd RL2 (NTC áthidalás), hálózatfigyelés, hibakezelés, weboldal, Home Assistant

#define HOME_ASSISTANT true   // false: a WiFi, a weboldal és az MQTT be sem fordul

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
#include <esp_timer.h>
#include <esp_heap_caps.h>

// ======================================================================= Platform
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || (ESP_ARDUINO_VERSION_MAJOR < 3)
#error "Arduino-ESP32 core 3.x vagy ujabb kell (fejlesztve: 4.0.0 / ESP-IDF 6.1)"
#endif
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || (ARDUINO_USB_CDC_ON_BOOT == 0)
#warning "Allitsd be: Eszkozok > USB CDC On Boot > Enabled (kulonben a soros naplo nem az USB-n jelenik meg)"
#endif

#if CONFIG_IDF_TARGET_ESP32S3
#define BOARD_NAME "XIAO ESP32-S3"
#define WIFI_TX_POWER WIFI_POWER_15dBm    // külső antenna: nagyobb teljesítmény is stabil
#elif CONFIG_IDF_TARGET_ESP32C3
#define BOARD_NAME "XIAO ESP32-C3"
#define WIFI_TX_POWER WIFI_POWER_8_5dBm   // a XIAO C3-on így stabil, és kisebb az áramcsúcs
#else
#error "Csak XIAO ESP32-C3 vagy XIAO ESP32-S3 alaplapra (Eszkozok > Alaplap)"
#endif
#if !defined(ARDUINO_XIAO_ESP32C3) && !defined(ARDUINO_XIAO_ESP32S3)
#warning "Nem XIAO alaplap van kivalasztva: ellenorizd, hogy a D-labak a XIAO kiosztas szerintiek-e"
#endif
const BaseType_t AUX_CORE = 0;   // napló és hálózat: C3-on az egyetlen mag, S3-on a vezérléstől (1. mag) külön
const UBaseType_t AUX_PRIO = 0;  // a vezérlés (1) alatt: C3-on a hálózat sem tudja kiéheztetni a vezérlést

#if HOME_ASSISTANT
#if !__has_include(<mqtt_client.h>)
#error "Az ESP-MQTT (mqtt_client.h) nincs ebben a core-ban: allitsd a HOME_ASSISTANT-ot false-ra"
#endif
#if __has_include("secrets.h")
#include "secrets.h"   // alapértékek (minta: secrets.example.h); a weboldalon mentett beállítás felülírja
#endif
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <mqtt_client.h>
#if __has_include(<esp_random.h>)
#include <esp_random.h>
#endif
#include "icon_png.h"   // a weboldal ikonja (tools/make_icon.py)
#if __has_include(<ESPmDNS.h>)
#include <ESPmDNS.h>   // Bonjour: a telefonos app és a http://erosito.local így találja meg
#define MDNS_ENABLED 1
#else
#include <lwip/sockets.h>   // ESPmDNS nélkül (pl. core 4.0 / IDF 6): beépített mDNS válaszoló
#define MDNS_ENABLED 0
#endif
#define NET_ENABLED 1
#else
#define NET_ENABLED 0
#endif

// ======================================================================= Lábak (XIAO D-nevek: a bekötés mindkét lapon azonos)
const int PIN_LED  = D1;   // LED tranzisztor (HIGH = világít)                           C3: GPIO3,  S3: GPIO2
const int PIN_BTN  = D2;   // gomb GND-re, belső felhúzó                                 C3: GPIO4,  S3: GPIO3
const int PIN_TRIG = D3;   // trigger opto, LOW = aktív, 10k felhúzó                     C3: GPIO5,  S3: GPIO4
const int PIN_FB1  = D4;   // RL1 kimenet opto (CNY17-3 + DB106 + 3x43k), LOW = hálózat  C3: GPIO6,  S3: GPIO5
const int PIN_RL2  = D5;   // RL2 tranzisztor                                            C3: GPIO7,  S3: GPIO6
const int PIN_RL1  = D10;  // RL1 tranzisztor                                            C3: GPIO10, S3: GPIO9

// ======================================================================= Beállítások
// Relék, indulás
const unsigned long RL2_DELAY_MS     = 1000;     // RL1 után ennyivel húz be az RL2 (NTC lágyindítás)
const unsigned long RL2_MIN_ON_MS    = 2000;     // kikapcsolás előtt az RL2 legalább ennyit legyen behúzva
const unsigned long MIN_OFF_MS       = 5000;     // minimális kikapcsolt idő (a boottól is számít)
const unsigned long POWERUP_HOLD_MS  = 120000UL; // áramszünet után ennyi ideig nincs trigger (a gomb egyszer bekapcsolhat)
const bool          TRIGGER_ON_BOOT  = true;     // induláskor aktív triggerre bekapcsol
const bool          BROWNOUT_IS_POWERLOSS = true; // brownout reset = áramszünet (különben fagyás)

// Gomb, LED
const unsigned long DEBOUNCE_MS      = 30;
const unsigned long LONG_PRESS_MS    = 400;      // hosszú nyomás = be / ki
const unsigned long DOUBLE_GAP_MS    = 800;      // rövid nyomások közti max. szünet
const uint8_t       SHORT_FAULT_COUNT = 3;       // ennyi rövid nyomás = hiba (2 = trigger tiltás / engedés)
const unsigned long FLASH_ON_MS      = 150;
const unsigned long FLASH_OFF_MS     = 150;
const unsigned long HOLD_FLASH_PERIOD_MS = 3000; // áramszünet utáni várakozás: felvillanás periódusa
const unsigned long HOLD_FLASH_MS    = 40;

// Hibakezelés
const unsigned long FAULT_BLINK_MS   = 1000;     // hibavillogás hossza
const unsigned long FAULT_HALF_MS    = 100;
const unsigned long FAULT_LOCKOUT_MS = 5000;     // hiba után ennyi ideig nincs bekapcsolás
const bool          AUTO_ON_AFTER_FAULT = true;  // tiltás végén aktív triggerre bekapcsol
const unsigned long FAULT_LOCKOUT_MAX_MS = 300000UL; // ismétlődő hibánál a tiltás duplázódik, legfeljebb eddig
const unsigned long FAULT_STREAK_RESET_MS = 600000UL; // ennyi hibamentes idő után a sorozat nullázódik
const uint8_t       FAULT_STREAK_PAUSE = 3;      // ennyi ismétlődő hiba után a trigger nem kapcsol be automatikusan
const uint8_t       LOSS_STREAK_PAUSE = 3;       // ennyi hálózatkiesés FAULT_STREAK_RESET_MS-on belül: nincs automatikus visszakapcsolás
const unsigned long BTN_STUCK_MS     = 10000;    // ennyi ideig nyomott gomb = beragadt
const unsigned long WINDOW_MS        = 1000;     // váltásszámláló ablak
const uint8_t       CHANGE_LIMIT     = 4;        // trigger: max. váltás / ablak
const uint8_t       BTN_CHANGE_LIMIT = 8;        // gomb: max. váltás / ablak
const uint32_t      WDT_MS           = 3000;

// Hálózatfigyelés (RL1 kimenet)
const bool          MAINS_CHECK      = true;
const unsigned long MAINS_WIN_MS     = 40;       // mérési ablak (~40 minta)
const uint8_t       MAINS_MIN_LOW    = 8;        // ennyi LOW minta / ablak = van hálózat (normál: ~28-37)
const unsigned long MAINS_FAULT_MS   = 150;      // ennyi ideig tartó eltérés = hiba
const unsigned long MAINS_OFF_GRACE_MS = 1000;   // kikapcsolás után ennyi ideig nem hiba a maradék feszültség
const unsigned long MAINS_LOSS_MS    = 25;       // bekapcsolva ennyi folyamatos HIGH = hálózatkiesés
const unsigned long MAINS_GAP_MS     = 5;        // ennél hosszabb mintavételi szünet nem számít kiesésnek

// Állapotfigyelés (öregedés)
const uint32_t      RELAY_LIFE       = 100000;   // relé élettartam (kapcsolás), a gyártói adatlap szerint állítható
const uint8_t       RELAY_WARN_PCT   = 80;       // figyelmeztetés ennyi %-nál
const uint8_t       OPTO_DROP_PCT    = 25;       // figyelmeztetés: az opto átlaga ennyi %-kal az alapérték alatt
const uint8_t       OPTO_MIN_AVG     = 15;       // figyelmeztetés: az opto átlaga ez alatt (a hibahatár MAINS_MIN_LOW)
const uint8_t       OPTO_BASE_SESSIONS = 5;      // az első ennyi mérési szakasz átlaga lesz az alapérték
const uint16_t      OPTO_MIN_WINDOWS = 250;      // egy szakasz csak ennyi ablak (~10 s) felett számít
const unsigned long STATS_SAVE_MS    = 3600000UL; // bekapcsolva óránként mentés
const unsigned long STATS_MIN_GAP_MS = 60000;    // két NVS-mentés között legalább ennyi idő (flash kímélése)

// Soros napló, Home Assistant
const uint8_t       LOG_MAX_PER_SEC  = 5;        // slog() korlát; slogc() korlátlan
const unsigned long STATUS_MIN_MS    = 1000;     // soros parancsok legfeljebb ennyi időnként
#define NET_ID "erosito"                         // MQTT témák, hostnév, AP név eleje
const unsigned long NET_STATE_MS     = 60000;    // MQTT állapot küldése ennyi időnként (és minden változáskor)
const unsigned long STA_TIMEOUT_MS   = 30000;    // ennyi sikertelen csatlakozás után AP mód
const unsigned long STA_RETRY_MS     = 30000;    // AP mód alatt ennyi időnként újrapróbálja a routert
const unsigned long AP_LINGER_MS     = 60000;    // csatlakozás után az AP ennyi idő múlva leáll (ha nincs rajta kliens)
const unsigned long STA_RETRY_BUSY_MS = 300000;  // AP-n lévő kliens mellett ritkábban próbálja a routert (pl. megjegyzett AP a telefonon)
const unsigned long MQTT_RETRY_MS    = 10000;    // sikertelen MQTT-kliens indítás után ennyi múlva újra
const unsigned long EVENT_MAX_AGE_S  = 600;      // a Home Assistant csomag ennél régebbi eseményt nem elemez
const uint32_t      HEAP_WARN_BYTES  = 20000;    // figyelmeztetés, ha a legkisebb szabad memória ez alá esett
const uint32_t      HEAP_BLOCK_WARN  = 8000;     // figyelmeztetés: a legnagyobb szabad blokk ez alatt (töredezettség)
const uint32_t      STACK_WARN_BYTES = 512;      // figyelmeztetés: egy task szabad verme ez alatt

static_assert(OPTO_MIN_AVG > MAINS_MIN_LOW, "OPTO_MIN_AVG > MAINS_MIN_LOW kell");
static_assert(MAINS_LOSS_MS >= 20, "MAINS_LOSS_MS >= 20 ms kell (2 felperiodus)");
static_assert(MAINS_GAP_MS < MAINS_LOSS_MS, "MAINS_GAP_MS < MAINS_LOSS_MS kell");
static_assert(MAINS_FAULT_MS > MAINS_WIN_MS, "MAINS_FAULT_MS > MAINS_WIN_MS kell");
static_assert(RL2_DELAY_MS > MAINS_WIN_MS + MAINS_FAULT_MS, "RL2_DELAY_MS: az RL1 visszajelzes az RL2 elott derul ki");
static_assert(MAINS_OFF_GRACE_MS < MIN_OFF_MS, "MAINS_OFF_GRACE_MS < MIN_OFF_MS kell");
static_assert(LONG_PRESS_MS < DOUBLE_GAP_MS, "LONG_PRESS_MS < DOUBLE_GAP_MS kell");
static_assert(WDT_MS >= 1000, "WDT_MS >= 1000 kell (NVS iras)");

// ======================================================================= Típusok, kódok (int, nem enum: az Arduino IDE prototípusai miatt)
const int F_NONE = 0, F_STUCK = 1, F_CHATTER = 2, F_PRESSES = 3, F_NOMAINS = 4, F_MAINSOFF = 5, F_MAINSLOST = 6;
const int IN_BTN = 0, IN_TRIG = 1, IN_FB1 = 2;

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

const uint32_t STATS_VER = 1;
struct Stats {               // NVS blob: bekapcsoláskor, kikapcsoláskor és óránként mentve
  uint32_t ver;
  uint32_t onCount;          // bekapcsolások (RL1)
  uint32_t rl2Count;         // RL2 behúzások
  uint32_t relayCount;       // RL1 kapcsolások relécsere óta
  uint32_t onSeconds;        // összes bekapcsolt idő
  uint32_t mainsLoss;        // hálózatkiesések
  uint32_t faults;           // egyéb hibák
  uint32_t powerUps;         // áramszünet utáni indulások
  float    optoAvg;          // opto LOW minta hosszú távú átlaga
  float    optoBase;         // alapérték (új opto)
  uint32_t optoBaseN;        // az alapértékbe számított szakaszok
};

// ======================================================================= Állapot
// Relék, időzítések (kezdet + hossz: millis() túlcsordulás-biztos; a letelt állapotokat jelzők rögzítik)
bool on = false;                // relék BE (RL1)
bool rl2On = false;
bool rl2Settled = false;        // az RL2 már legalább RL2_MIN_ON_MS-ig behúzva
unsigned long onTime = 0, rl2Time = 0;
unsigned long offTime = 0;      // utolsó kikapcsolás (0 = boot)
bool minOffActive = true;       // a minimális kikapcsolt idő fut (bootkor is)
bool powerHold = false;         // áramszünet utáni várakozás
bool holdBtnUsed = false;       // a várakozás alatt egy kézi (gomb / távoli) bekapcsolás engedett
unsigned long holdStart = 0, holdDur = 0, lastHoldSave = 0;

// Kérések
bool offPending = false;        // kikapcsolás az RL2 minimum idő után
bool offByBtn = false;          // a függő kikapcsolást a gomb / távoli parancs kérte
bool onReqBtn = false;          // függő bekapcsolás: gomb / távoli (feltétel nélkül)
bool onReqTrig = false;         // függő bekapcsolás: trigger (ha még aktív)
const char *onReqSrc = "";
bool trigOff = false;           // trigger tiltva (NVS)

// Hibák
bool faultActive = false;       // hibavillogás
bool faultHold = false;         // bemenet-hibafigyelés szünetel a következő bekapcsolásig
unsigned long faultStart = 0;
int lastFault = F_NONE;
int storedFault = F_NONE;       // NVS-ben tárolt hiba
bool lockoutPending = false;    // hiba utáni tiltás
unsigned long lockoutStart = 0, lockoutDur = 0;
bool pauseLogged = false;           // a szünetelés üzenete egyszer
uint8_t lossStreak = 0;             // egymás utáni hálózatkiesések
unsigned long lossLastT = 0;        // az utolsó hálózatkiesés
uint8_t faultStreak = 0;            // egymás utáni hardverhibák (zaj, beragadás, RL1 kimenet)
unsigned long faultQuietT = 0;      // az utolsó hiba vége (a sorozat nullázásához)
bool noMainsWait = false;           // F_NOMAINS után: csak gomb / távoli / új trigger él kapcsol be

// Bemenetek, gomb
Input inputs[2] = {
  { (uint8_t)PIN_BTN,  "GOMB",    BTN_STUCK_MS, BTN_CHANGE_LIMIT, HIGH, HIGH, 0, 0, 0, 0 },
  { (uint8_t)PIN_TRIG, "TRIGGER", 0,            CHANGE_LIMIT,     HIGH, HIGH, 0, 0, 0, 0 }
};
Input &btn  = inputs[IN_BTN];
Input &trig = inputs[IN_TRIG];
bool pressConsumed = false;
unsigned long pressStart = 0, lastShortRel = 0;
uint8_t shortCount = 0;
bool burstTaint = false;        // hibás nyomássorozat folytatása nem válthat

// LED
bool ledBase = false, ledFlashOn = false, ledFaultBlink = false;
uint8_t ledFlashes = 0;
unsigned long ledT = 0;

// Hálózatfigyelés
unsigned long fbWinStart = 0, fbLastLow = 0, fbLastSample = 0, fbBadSince = 0, fbCmdT = 0;
uint8_t fbLow = 0, fbCount = 0;   // aktuális / utolsó ablak LOW mintái
bool fbValid = false;           // van teljes ablak a relé-parancs óta
bool fbPresent = false;
bool fbBad = false;             // eltér a várttól
bool fbConfirmed = false;       // bekapcsolás után megjelent a hálózat
bool fbGraceOver = false;       // letelt a kikapcsolás utáni türelmi idő

// Állapotfigyelés
Preferences prefs;
Stats st = {};
bool stDirty = false, stCounting = false;
unsigned long stAcctT = 0, stAcctMs = 0, stLastSave = 0;
uint32_t sessSum = 0, sessN = 0;  // aktuális mérési szakasz opto mintái
float lastSessMean = 0;
int lastWarnCount = -1;         // új figyelmeztetésnél esemény a Home Assistant felé

// Más taskokból érkező kérések (soros, MQTT) és a Home Assistant felé küldendő változás
volatile bool statusReq = false, healthReq = false, helpReq = false;
volatile bool baseReq = false;      // 'B': új opto alapérték (optocsere után)
volatile bool relayReq = false;     // 'R': relé számláló nullázása (relécsere után)
volatile bool remoteOnReq = false, remoteOffReq = false;
volatile int8_t remoteTrigReq = -1;   // weboldal / Home Assistant: trigger figyelés 1 = be, 0 = ki
volatile bool netResetReq = false;    // 'W': hálózati beállítások törlése
volatile bool netStateDirty = true;
void netEvent(const char *type, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void switchOn(const char *src, bool manual);   // előre deklarálva: a hibakezelés is hívja

// Taskok (verem-figyeléshez)
TaskHandle_t loopTaskH = nullptr, logTaskH = nullptr, netTaskH = nullptr;
uint32_t stackMinFree(const char **name) {   // a legkevesebb szabad verem a taskok közül (bájt)
  uint32_t best = UINT32_MAX;
  const char *nm = "-";
  TaskHandle_t h[3] = {loopTaskH, logTaskH, netTaskH};
  const char *n[3] = {"vezerles", "naplo", "halozat"};
  for (int i = 0; i < 3; i++) {
    if (!h[i]) continue;
    uint32_t f = (uint32_t)uxTaskGetStackHighWaterMark(h[i]);
    if (f < best) { best = f; nm = n[i]; }
  }
  if (name) *name = nm;
  return best;
}

// Áramszünet utáni várakozás hátralévő ideje: reset után megmarad, tápvesztéskor elvész
const uint32_t HOLD_MAGIC = 0x484F4C44UL;
RTC_NOINIT_ATTR uint32_t rtcHoldLeft;
RTC_NOINIT_ATTR uint32_t rtcHoldCheck;

// ======================================================================= Segédek
unsigned long timeLeft(unsigned long start, unsigned long dur) {   // túlcsordulás-biztos hátralévő idő
  unsigned long el = millis() - start;
  return (el >= dur) ? 0 : (dur - el);
}
unsigned long uptimeHours() { return (unsigned long)(esp_timer_get_time() / 3600000000LL); }   // 64 bites óra: nem csordul túl

unsigned long minOffLeft() { return (on || !minOffActive) ? 0 : timeLeft(offTime, MIN_OFF_MS); }
unsigned long lockoutLeft() { return lockoutPending ? timeLeft(lockoutStart, lockoutDur) : 0; }
bool lossPaused() { return lossStreak >= LOSS_STREAK_PAUSE; }      // sorozatos hálózatkiesés
bool autoPaused() { return faultStreak >= FAULT_STREAK_PAUSE || lossPaused(); }   // ismétlődő hiba: csak gomb / távoli kapcsol be

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

// ======================================================================= Soros napló (külön task, nem blokkoló sor)
struct LogMsg { char text[160]; };
QueueHandle_t logQ = nullptr;
unsigned long logWinStart = 0, logSuppressed = 0, logDropped = 0, lastStatus = 0;
uint8_t logWinCount = 0;

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

void logTask(void *) {   // kiírás + soros parancsok fogadása
  LogMsg m;
  for (;;) {
    if (xQueueReceive(logQ, &m, pdMS_TO_TICKS(50)) == pdTRUE) Serial.print(m.text);
    while (Serial.available() > 0) {
      int c = Serial.read();
      if (c == 's' || c == 'S') statusReq = true;
      else if (c == 'h' || c == 'H') healthReq = true;
      else if (c == 'B') baseReq = true;
      else if (c == 'R') relayReq = true;
      else if (c == 'W') netResetReq = true;
      else if (c > ' ') helpReq = true;
    }
  }
}

// ======================================================================= LED
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
  } else if (!on && autoPaused()) {   // ismétlődő hiba / sorozatos kiesés: dupla felvillanás 3 s-onként
    unsigned long ph = now % HOLD_FLASH_PERIOD_MS;
    level = ph < HOLD_FLASH_MS || (ph >= 4 * HOLD_FLASH_MS && ph < 5 * HOLD_FLASH_MS);
  }
  digitalWrite(PIN_LED, level ? HIGH : LOW);
}

// ======================================================================= Bemenetek: szűrés, beragadás- és zajfigyelés
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

// ======================================================================= Hálózatfigyelés: az opto a nullátmeneteknél rövid ideig HIGH, egyébként LOW
void mainsCmd() {   // relé-parancs után új, tiszta mérés
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

// ======================================================================= Állapotfigyelés (NVS)
void statsSave(bool force = false) {   // force nélkül legfeljebb STATS_MIN_GAP_MS-onként; a többit a statsTimer menti
  if (!stDirty) return;
  if (!force && stLastSave && millis() - stLastSave < STATS_MIN_GAP_MS) return;
  stLastSave = millis();   // a próbálkozás ideje: hibás NVS-nél se próbálja minden ciklusban
  if (!prefs.begin("relay", false)) return;
  if (prefs.putBytes("stats", &st, sizeof st) == sizeof st) stDirty = false;
  prefs.end();
}

void statsAccount() {   // bekapcsolt idő hozzáadása
  if (!stCounting) return;
  unsigned long now = millis();
  stAcctMs += now - stAcctT;
  stAcctT = now;
  if (stAcctMs >= 1000) { st.onSeconds += stAcctMs / 1000; stAcctMs %= 1000; stDirty = true; }
}

void statsFold() {   // a mérési szakasz opto átlagának beszámítása
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

int statsWarnings(char *buf, size_t len) {   // figyelmeztetések szövege (üres = rendben)
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
  if (esp_get_minimum_free_heap_size() < HEAP_WARN_BYTES) {   // hónapokig tartó futásnál: memóriafogyás jele
    n++;
    snprintf(buf + strlen(buf), len - strlen(buf), " keves szabad memoria (min. %lu bajt);",
             (unsigned long)esp_get_minimum_free_heap_size());
  }
  size_t blk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (blk < HEAP_BLOCK_WARN) {   // sok szabad memória, de csak apró darabokban
    n++;
    snprintf(buf + strlen(buf), len - strlen(buf), " toredezett memoria (legnagyobb blokk %lu);", (unsigned long)blk);
  }
  const char *tn;
  uint32_t sf = stackMinFree(&tn);
  if (sf < STACK_WARN_BYTES) {
    n++;
    snprintf(buf + strlen(buf), len - strlen(buf), " keves verem (%s: %lu);", tn, (unsigned long)sf);
  }
  return n;
}

void checkWarnings() {
  char w[120];
  int n = statsWarnings(w, sizeof w);
  if (lastWarnCount >= 0 && n > lastWarnCount) netEvent("warning", "Öregedés:%s", w);
  lastWarnCount = n;
}

// ======================================================================= Relék
void setState(bool s, const char *src) {
  if (s == on) return;
  on = s;
  offPending = false;
  offByBtn = false;
  onReqBtn = false;
  onReqTrig = false;
  if (on) {
    digitalWrite(PIN_RL1, HIGH);
    noMainsWait = false;
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

void updateRl2(unsigned long now) {   // RL2 késleltetve (csak igazolt hálózat után), függő kikapcsolás az RL2 minimum után
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
}

// ======================================================================= Hibák

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
  if (f != F_MAINSLOST && f != F_PRESSES) {   // a szándékos 3 nyomás és a kiesés nem hardverhiba-sorozat
    if (faultStreak && millis() - faultQuietT >= FAULT_STREAK_RESET_MS) faultStreak = 0;
    if (faultStreak < 255) faultStreak++;
    pauseLogged = false;
  }
  if (f == F_MAINSLOST) {   // FAULT_STREAK_RESET_MS-on belüli kiesések száma
    if (lossStreak && millis() - lossLastT >= FAULT_STREAK_RESET_MS) lossStreak = 0;
    if (lossStreak < 255) lossStreak++;
    lossLastT = millis();
    pauseLogged = false;
  }
  if (idx == IN_FB1) {   // hálózati hiba: relék azonnal KI
    bool retry = on && !offPending;
    setState(false, (f == F_MAINSLOST) ? "halozatkieses" : "halozati hiba");
    if (f == F_MAINSLOST && retry && lossPaused())
      slogc("!!! %u halozatkieses %lu percen belul: nincs automatikus visszakapcsolas (gomb / tavoli: igen)\n",
            (unsigned)lossStreak, FAULT_STREAK_RESET_MS / 60000);
    else if (f == F_MAINSLOST && retry) {
      onReqTrig = true;
      onReqSrc = "trigger halozatkieses utan";
    }
  }
  if (f == F_MAINSLOST && millis() - stLastSave >= 30000) statsSave(true);   // valódi áramszünet: a számláló ne vesszen el (legfeljebb 30 s-onként)
  lastFault = f;
  saveFault(f);
  if (f == F_MAINSLOST) netEvent("mains_loss", "Hálózatkiesés bekapcsolt állapotban%s", onReqTrig ? ", újrakapcsolás aktív triggerre"
                                 : lossPaused() ? ", sorozatos kiesés: nincs automatikus visszakapcsolás" : "");
  else netEvent("fault", "%s: %s", idx == IN_FB1 ? "RL1 kimenet" : inputs[idx].name, faultName(f));
  faultActive = true;
  faultStart = millis();
  ledFaultBlink = true;
  ledFlashes = 0;
  ledT = faultStart;
  if (idx == IN_FB1) {
    if (f == F_MAINSLOST)
      slogc("!!! HALOZATKIESES: relek AZONNAL KI%s\n",
            onReqTrig ? ", MIN_OFF_MS utan aktiv triggerre ujra be"
            : lossPaused() ? " (sorozatos kieses: nincs ujrakapcsolas)" : " (kikapcsolas volt fuggoben: nincs ujrakapcsolas)");
    else
      slogc("!!! RL1 kimenet HIBA: %s -> relek KI\n",
            (f == F_NOMAINS) ? "nincs halozat (az RL1 nem huzott be, vagy nincs halozat)"
                             : "halozat van KIKAPCSOLT relenel (osszehegedt erintkezo?)");
    return;
  }
  slogc("!!! %s HIBA: %s -> LED villog, majd relek KI\n", inputs[idx].name,
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
  if (lastFault != F_PRESSES)   // ismétlődő hiba: 5, 10, 20 ... s, legfeljebb FAULT_LOCKOUT_MAX_MS
    for (uint8_t i = 1; i < faultStreak && lockoutDur < FAULT_LOCKOUT_MAX_MS; i++) lockoutDur *= 2;
  if (lockoutDur > FAULT_LOCKOUT_MAX_MS) lockoutDur = FAULT_LOCKOUT_MAX_MS;
  faultQuietT = millis();
  slogc("Hiba vege: relek KI, %lu ms-ig nincs bekapcsolas%s\n", lockoutDur,
        autoPaused() ? " (ismetlodo hiba: a trigger automatika szunetel)" : "");
}

void endLockout() {   // tiltás vége: ok ellenőrzése, mentett hiba törlése, automata bekapcsolás
  lockoutPending = false;
  netStateDirty = true;
  if (btn.stable == LOW || btn.raw == LOW) {
    slog("Tiltas vege: a gomb meg nyomva\n");
    startFault(IN_BTN, F_STUCK);
  } else if (MAINS_CHECK && fbValid && fbPresent) {
    slog("Tiltas vege: RL1 kimeneten halozat van KI relenel\n");
    startFault(IN_FB1, F_MAINSOFF);
  } else {
    int was = lastFault;
    lastFault = F_NONE;
    saveFault(F_NONE);
    noMainsWait = (was == F_NOMAINS);
    if (was == F_NOMAINS) slog("Nincs automatikus ujrakapcsolas: gomb vagy uj trigger kell\n");
    else if (AUTO_ON_AFTER_FAULT && !trigOff && trig.stable == LOW && !autoPaused()) switchOn("aktiv trigger hiba utan", false);
  }
}

void checkInputFaults() {   // gomb, majd (ha nincs tiltva) trigger
  int f = checkFault(IN_BTN);
  if (f != F_NONE) startFault(IN_BTN, f);
  else if (!trigOff && (f = checkFault(IN_TRIG)) != F_NONE) startFault(IN_TRIG, f);
}

// ======================================================================= Be / ki kérések
void requestOff(const char *src, bool byBtn) {   // az RL2 legalább RL2_MIN_ON_MS-ig behúzva marad
  if (!on) return;
  if (rl2Settled) {
    setState(false, src);
    return;
  }
  if (byBtn) offByBtn = true;
  if (offPending) return;
  offPending = true;
  if (rl2On) slog("Kikapcsolas (%s) %lu ms mulva\n", src, timeLeft(rl2Time, RL2_MIN_ON_MS));
  else slog("Kikapcsolas (%s) az RL2 utan %lu ms-mal\n", src, RL2_MIN_ON_MS);
}

bool btnBusy() { return onReqBtn || (offPending && offByBtn); }   // függő kézi kérés alatt a gomb hatástalan

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

void processPendingOn(bool inHold) {   // függő bekapcsolás a minimális kikapcsolt idő végén
  if (faultActive || on || minOffLeft() > 0) return;
  if (inHold) {   // várakozás alatt csak a kézi kérés
    if (onReqBtn) { onReqBtn = false; switchOn("gomb aramszunet utan", true); }
    return;
  }
  if (!onReqBtn && !onReqTrig) return;
  bool byBtn = onReqBtn;
  const char *src = onReqSrc;
  onReqBtn = false;
  onReqTrig = false;
  if (byBtn) switchOn("gomb", true);
  else if (!trigOff && trig.stable == LOW) switchOn(src, false);
  else slog("Fuggo bekapcsolas elmarad: a trigger nem aktiv\n");
}

void triggerEdge() {   // trigger: aktív élre be, inaktív élre ki
  if (trig.stable == LOW) {
    if (offPending && !offByBtn) {
      offPending = false;
      slog("Trigger ujra aktiv: kikapcsolas elmarad\n");
    }
    if (!on && autoPaused()) {
      if (!pauseLogged) slog("Trigger: ismetlodo hiba miatt nincs automatikus bekapcsolas (gomb / tavoli: igen)\n");
      pauseLogged = true;
    }
    else switchOn("trigger", false);
  } else {
    requestOff("trigger", false);
  }
}

void handleRemote(bool inHold) {   // Home Assistant: ON mint a gomb (várakozás alatt egyszer), OFF mindig
  if (remoteOnReq) {
    remoteOnReq = false;
    if (faultActive || on) {
      // nincs teendő
    } else if (inHold) {
      if (!holdBtnUsed) { holdBtnUsed = true; switchOn("tavoli aramszunet utan", true); }
    } else if (!btnBusy()) {
      switchOn("tavoli", true);
    }
  }
  if (remoteOffReq) {
    remoteOffReq = false;
    if (on) requestOff("tavoli", true);
    else if (onReqBtn || onReqTrig) { onReqBtn = onReqTrig = false; slog("Fuggo bekapcsolas torolve (tavoli)\n"); }
  }
}

// ======================================================================= Gomb
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

void buttonLogic(bool changed) {   // hosszú = be / ki; 2 rövid = trigger tiltás / engedés; 3+ rövid = hiba
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

void holdButton(unsigned long now, bool changed) {   // áramszünet utáni várakozás: egy hosszú nyomás bekapcsolhat
  if (holdBtnUsed) return;
  if (changed && btn.stable == LOW) {
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

// ======================================================================= Hálózat: WiFi (STA / AP), weboldal, MQTT – külön taskban; a vezérlés csak jelzőket és nem blokkoló sort használ
#if NET_ENABLED
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef MQTT_URI
#define MQTT_URI ""
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASS
#define MQTT_PASS ""
#endif
#ifndef AP_PASS
#define AP_PASS "erosito123"   // AP jelszó és a weboldal alapjelszava (legalább 8 karakter)
#endif
static_assert(sizeof(AP_PASS) - 1 >= 8 && sizeof(AP_PASS) - 1 <= 32, "AP_PASS: 8-32 karakter kell (WPA2)");
#ifndef STATIC_IP
#define STATIC_IP ""   // üres: DHCP
#endif
#ifndef STATIC_GW
#define STATIC_GW ""
#endif
#ifndef STATIC_MASK
#define STATIC_MASK ""   // üres: 255.255.255.0
#endif
#ifndef STATIC_DNS
#define STATIC_DNS ""    // üres: az átjáró
#endif

struct NetCfg { char ssid[33], pass[65], mqttUri[96], mqttUser[33], mqttPass[65], webPass[33], ip[16], gw[16], mask[16], dns[16]; };
NetCfg ncfg;
Preferences netPrefs;           // külön példány: a hálózati task használja
struct NetEvt { char type[12]; char text[100]; int64_t t; };   // t: 64 bites óra (µs)
QueueHandle_t netQ = nullptr;
WebServer web(80);
DNSServer dns;
esp_mqtt_client_handle_t mqtt = nullptr;
volatile bool mqttUp = false;
volatile bool netNeedDisc = false;
bool netStarted = false, apOn = false;
bool reconfWifi = false, reconfMqtt = false;   // függő újrakonfigurálás (a HTTP-válasz kiküldése után)
unsigned long reconfAt = 0;
const unsigned long RECONF_DELAY_MS = 1500;   // ennyi idő marad a válasz kiküldésére a WiFi bontása előtt

void requestReconfig(bool wifiChanged, bool mqttChanged) {
  reconfWifi |= wifiChanged;
  reconfMqtt |= mqttChanged;
  reconfAt = millis();
}

bool ipCfgDiffers(const NetCfg &x, const NetCfg &y) {
  return strcmp(x.ip, y.ip) || strcmp(x.gw, y.gw) || strcmp(x.mask, y.mask) || strcmp(x.dns, y.dns);
}
unsigned long netLastState = 0, staLastOk = 0, staLastBegin = 0, apIdleSince = 0, mqttLastTry = 0, apLastTry = 0;
bool mqttTried = false, apTried = false;
int mdnsState = 0;               // 0 = még nem indult, 1 = működik, 2 = hiba (újrapróbálja)
unsigned long mdnsLastTry = 0;
const char *mdnsName() {
  return mdnsState == 1 ? "OK (" NET_ID ".local)" : mdnsState == 2 ? "hiba" : "nem indult";
}

void wifiMode(wifi_mode_t m) {   // energiatakarékos mód nélkül: megbízható mDNS (multicast) és gyors weboldal
  WiFi.mode(m);
  WiFi.setSleep(false);
}
char apSsid[24];
char webToken[33];             // bejelentkezési süti (NVS-ben: áramszünet után is érvényes; jelszócserekor új)
const char *T_STATE   = NET_ID "/state";
const char *T_EVENT   = NET_ID "/event";
const char *T_CMD     = NET_ID "/power/set";
const char *T_TRIGCMD = NET_ID "/trigger/set";
const char *T_STATUS  = NET_ID "/status";

// ---- Fix IP: ellenőrzés (hibás vagy üres beállításnál DHCP) ----
// IPCFG_PURE_BEGIN
bool ipParse(const char *s, uint32_t *out) {   // "a.b.c.d" -> 32 bites szám; üres vagy hibás: false
  uint32_t v = 0;
  for (int part = 0; part < 4; part++) {
    if (*s < '0' || *s > '9') return false;
    uint32_t x = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
      x = x * 10 + (uint32_t)(*s - '0');
      if (++digits > 3 || x > 255) return false;
      s++;
    }
    v = (v << 8) | x;
    if (part < 3) {
      if (*s != '.') return false;
      s++;
    }
  }
  if (*s) return false;
  *out = v;
  return true;
}

// nullptr = rendben (o[0..3]: IP, átjáró, maszk, DNS), egyébként a hiba oka
const char *ipCheck(const char *ip, const char *gw, const char *mask, const char *dnsIp, uint32_t o[4]) {
  uint32_t a, g, m, d;
  if (!ipParse(ip, &a)) return "hibás IP-cím";
  if (!ipParse(mask[0] ? mask : "255.255.255.0", &m)) return "hibás maszk";
  uint32_t host = ~m;
  if (m == 0 || (host & (host + 1)) != 0) return "a maszk nem összefüggő";
  if (host < 3) return "túl kicsi alhálózat";
  if ((a >> 24) == 0 || (a >> 24) == 127 || (a >> 24) >= 224) return "nem használható IP-cím";
  if ((a & host) == 0 || (a & host) == host) return "az IP-cím hálózati vagy szórási cím";
  if (!gw[0]) return "hiányzik az átjáró";
  if (!ipParse(gw, &g)) return "hibás átjáró";
  if ((g & m) != (a & m)) return "az átjáró nincs az IP-cím alhálózatában";
  if (g == a) return "az átjáró nem lehet maga az eszköz";
  if (dnsIp[0]) {
    if (!ipParse(dnsIp, &d)) return "hibás DNS";
  } else {
    d = g;
  }
  o[0] = a; o[1] = g; o[2] = m; o[3] = d;
  return nullptr;
}
// IPCFG_PURE_END

int ipMode = 0;                  // 0 = DHCP, 1 = fix IP, 2 = hibás fix IP, ezért DHCP
const char *ipErr = nullptr;

IPAddress ipAddr(uint32_t v) { return IPAddress((v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

void ipApply() {   // WiFi.begin előtt: fix IP, vagy DHCP (üres / hibás beállításnál)
  uint32_t o[4];
  ipErr = ncfg.ip[0] ? ipCheck(ncfg.ip, ncfg.gw, ncfg.mask, ncfg.dns, o) : nullptr;
  ipMode = !ncfg.ip[0] ? 0 : (ipErr ? 2 : 1);
  if (ipMode == 1) {
    WiFi.config(ipAddr(o[0]), ipAddr(o[1]), ipAddr(o[2]), ipAddr(o[3]));
  } else {
    WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));   // DHCP
    if (ipMode == 2) slogc("Fix IP hibas (%s): DHCP\n", ipErr);
  }
}

const char *ipModeName() {
  return ipMode == 1 ? "fix" : ipMode == 2 ? "hibás fix IP, DHCP" : "DHCP";
}

// ---- Beállítások: NVS ("net"), ennek hiányában a secrets.h ----
void netCfgLoad() {
  strlcpy(ncfg.ssid, WIFI_SSID, sizeof ncfg.ssid);
  strlcpy(ncfg.pass, WIFI_PASS, sizeof ncfg.pass);
  strlcpy(ncfg.mqttUri, MQTT_URI, sizeof ncfg.mqttUri);
  strlcpy(ncfg.mqttUser, MQTT_USER, sizeof ncfg.mqttUser);
  strlcpy(ncfg.mqttPass, MQTT_PASS, sizeof ncfg.mqttPass);
  strlcpy(ncfg.webPass, AP_PASS, sizeof ncfg.webPass);
  strlcpy(ncfg.ip, STATIC_IP, sizeof ncfg.ip);
  strlcpy(ncfg.gw, STATIC_GW, sizeof ncfg.gw);
  strlcpy(ncfg.mask, STATIC_MASK, sizeof ncfg.mask);
  strlcpy(ncfg.dns, STATIC_DNS, sizeof ncfg.dns);
  webToken[0] = 0;
  if (!netPrefs.begin("net", true)) return;   // nincs mentett beállítás
  if (netPrefs.isKey("token")) netPrefs.getString("token", webToken, sizeof webToken);
  if (netPrefs.isKey("ssid")) {
    netPrefs.getString("ssid", ncfg.ssid, sizeof ncfg.ssid);
    netPrefs.getString("pass", ncfg.pass, sizeof ncfg.pass);
    netPrefs.getString("mqttUri", ncfg.mqttUri, sizeof ncfg.mqttUri);
    netPrefs.getString("mqttUser", ncfg.mqttUser, sizeof ncfg.mqttUser);
    netPrefs.getString("mqttPass", ncfg.mqttPass, sizeof ncfg.mqttPass);
    netPrefs.getString("webPass", ncfg.webPass, sizeof ncfg.webPass);
  }
  if (netPrefs.isKey("ip")) {   // a fix IP külön is menthető (iOS-app)
    netPrefs.getString("ip", ncfg.ip, sizeof ncfg.ip);
    netPrefs.getString("gw", ncfg.gw, sizeof ncfg.gw);
    netPrefs.getString("mask", ncfg.mask, sizeof ncfg.mask);
    netPrefs.getString("dns", ncfg.dns, sizeof ncfg.dns);
  }
  netPrefs.end();
}

bool netCfgSave() {
  if (!netPrefs.begin("net", false)) return false;
  bool ok = netPrefs.putString("ssid", ncfg.ssid) == strlen(ncfg.ssid);
  ok &= netPrefs.putString("pass", ncfg.pass) == strlen(ncfg.pass);
  ok &= netPrefs.putString("mqttUri", ncfg.mqttUri) == strlen(ncfg.mqttUri);
  ok &= netPrefs.putString("mqttUser", ncfg.mqttUser) == strlen(ncfg.mqttUser);
  ok &= netPrefs.putString("mqttPass", ncfg.mqttPass) == strlen(ncfg.mqttPass);
  ok &= netPrefs.putString("webPass", ncfg.webPass) == strlen(ncfg.webPass);
  ok &= netPrefs.putString("ip", ncfg.ip) == strlen(ncfg.ip);
  ok &= netPrefs.putString("gw", ncfg.gw) == strlen(ncfg.gw);
  ok &= netPrefs.putString("mask", ncfg.mask) == strlen(ncfg.mask);
  ok &= netPrefs.putString("dns", ncfg.dns) == strlen(ncfg.dns);
  netPrefs.end();
  return ok;
}

void tokenNew() {   // új bejelentkezési süti: a régiek érvénytelenek (a WiFi indulása után: valódi véletlenszám)
  snprintf(webToken, sizeof webToken, "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random());
  if (netPrefs.begin("net", false)) {
    netPrefs.putString("token", webToken);
    netPrefs.end();
  }
}

void netCfgClear() {
  if (netPrefs.begin("net", false)) { netPrefs.clear(); netPrefs.end(); }
}

// ---- Események a vezérlésből (nem blokkoló) ----
void netEvent(const char *type, const char *fmt, ...) {
  netStateDirty = true;
  if (!netQ) return;
  NetEvt e;
  strlcpy(e.type, type, sizeof e.type);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.text, sizeof e.text, fmt, ap);
  va_end(ap);
  e.t = esp_timer_get_time();
  xQueueSend(netQ, &e, 0);   // tele sornál elvész, nem vár
}

// ---- Állapot JSON (MQTT és weboldal közös) ----
void jsonEsc(char *dst, size_t len, const char *src) {   // idézőjel, backslash, vezérlőkarakter kiszűrése
  size_t j = 0;
  for (; *src && j + 2 < len; src++) {
    if (*src == '"' || *src == '\\') { dst[j++] = '\\'; dst[j++] = *src; }
    else if ((unsigned char)*src >= 0x20) dst[j++] = *src;
  }
  dst[j] = 0;
}

void buildState(char *js, size_t len, bool forWeb) {
  char w[120];
  statsWarnings(w, sizeof w);
  unsigned long h10 = st.onSeconds / 360;
  unsigned a = (unsigned)(st.optoAvg * 10 + 0.5f), b = (unsigned)(st.optoBase * 10 + 0.5f);
  bool flt = faultActive || lockoutPending;
  int n = snprintf(js, len,
           "{\"power\":\"%s\",\"rl2\":\"%s\",\"trigger\":\"%s\",\"trig_enabled\":%s,\"trig_en\":\"%s\",\"hold\":%s,"
           "\"fault\":\"%s\",\"warning\":\"%s\",\"problem\":\"%s\",\"on_hours\":%lu.%lu,\"on_count\":%lu,"
           "\"relay_count\":%lu,\"mains_loss\":%lu,\"faults\":%lu,\"power_ups\":%lu,\"opto_now\":%u,"
           "\"opto_avg\":%u.%u,\"opto_base\":%u.%u,\"rssi\":%d,\"heap_free\":%lu,\"heap_min\":%lu,\"heap_block\":%lu",
           on ? "ON" : "OFF", rl2On ? "ON" : "OFF", trig.stable == LOW ? "ON" : "OFF", trigOff ? "false" : "true",
           trigOff ? "OFF" : "ON", powerHold ? "true" : "false", flt ? faultName(lastFault) : "nincs", w[0] ? w + 1 : "nincs",
           (flt || w[0]) ? "ON" : "OFF", h10 / 10, h10 % 10, (unsigned long)st.onCount, (unsigned long)st.relayCount,
           (unsigned long)st.mainsLoss, (unsigned long)st.faults, (unsigned long)st.powerUps, (unsigned)fbCount,
           a / 10, a % 10, b / 10, b % 10, (int)WiFi.RSSI(), (unsigned long)esp_get_free_heap_size(),
           (unsigned long)esp_get_minimum_free_heap_size(), (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  if (n < 0 || (size_t)n >= len) { strlcpy(js, "{}", len); return; }
  if (forWeb) {
    char ss[70], mu[200], mus[70], ci[34], cg[34], cm[34], cd[34];
    jsonEsc(ci, sizeof ci, ncfg.ip);
    jsonEsc(cg, sizeof cg, ncfg.gw);
    jsonEsc(cm, sizeof cm, ncfg.mask);
    jsonEsc(cd, sizeof cd, ncfg.dns);
    jsonEsc(ss, sizeof ss, ncfg.ssid);
    jsonEsc(mu, sizeof mu, ncfg.mqttUri);
    jsonEsc(mus, sizeof mus, ncfg.mqttUser);
    bool sta = WiFi.status() == WL_CONNECTED;
    n += snprintf(js + n, len - n,
                  ",\"wifi\":\"%s\",\"ip\":\"%s\",\"mdns\":\"%s\",\"mqtt\":\"%s\",\"uptime_h\":%lu,\"board\":\"" BOARD_NAME "\","
                  "\"cfg_ssid\":\"%s\",\"cfg_mqtt_uri\":\"%s\",\"cfg_mqtt_user\":\"%s\",\"ip_mode\":\"%s\",\"ip_error\":\"%s\","
                  "\"cfg_ip\":\"%s\",\"cfg_gw\":\"%s\",\"cfg_mask\":\"%s\",\"cfg_dns\":\"%s\"",
                  sta ? (apOn ? "csatlakozva + AP" : "csatlakozva") : (apOn ? "AP mód" : "nincs"),
                  sta ? WiFi.localIP().toString().c_str() : (apOn ? WiFi.softAPIP().toString().c_str() : "-"), mdnsName(),
                  !ncfg.mqttUri[0] ? "nincs beállítva" : (mqttUp ? "OK" : "nincs kapcsolat"),
                  uptimeHours(), ss, mu, mus, ipModeName(), ipErr ? ipErr : "", ci, cg, cm, cd);
    if (n < 0 || (size_t)n >= len - 1) { strlcpy(js, "{}", len); return; }
  }
  strlcat(js, "}", len);
}

// ---- MQTT ----
void netPub(const char *topic, const char *payload, bool retain) {
  if (mqtt && mqttUp) esp_mqtt_client_enqueue(mqtt, topic, payload, 0, 1, retain ? 1 : 0, true);
}

bool topicIs(esp_mqtt_event_handle_t e, const char *t) {
  return e->topic_len == (int)strlen(t) && strncmp(e->topic, t, e->topic_len) == 0;
}

bool dataIs(esp_mqtt_event_handle_t e, const char *d) {
  return e->data_len == (int)strlen(d) && strncmp(e->data, d, e->data_len) == 0;
}

void mqttHandler(void *, esp_event_base_t, int32_t id, void *data) {
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
  if (id == MQTT_EVENT_CONNECTED) {
    mqttUp = true;
    netNeedDisc = true;
    esp_mqtt_client_subscribe_single(mqtt, T_CMD, 1);
    esp_mqtt_client_subscribe_single(mqtt, T_TRIGCMD, 1);
  } else if (id == MQTT_EVENT_DISCONNECTED) {
    mqttUp = false;
  } else if (id == MQTT_EVENT_DATA && !e->retain) {
    if (topicIs(e, T_CMD)) {
      if (dataIs(e, "ON")) remoteOnReq = true;
      else if (dataIs(e, "OFF")) remoteOffReq = true;
    } else if (topicIs(e, T_TRIGCMD)) {
      if (dataIs(e, "ON")) remoteTrigReq = 1;
      else if (dataIs(e, "OFF")) remoteTrigReq = 0;
    }
  }
}

void netDisc(const char *comp, const char *obj, const char *name, const char *extra) {   // MQTT discovery: egy entitás
  char topic[80], js[512];
  snprintf(topic, sizeof topic, "homeassistant/%s/" NET_ID "/%s/config", comp, obj);
  snprintf(js, sizeof js,
           "{\"name\":%s,\"uniq_id\":\"" NET_ID "_%s\",\"stat_t\":\"" NET_ID "/state\","
           "\"val_tpl\":\"{{value_json.%s}}\",\"avty_t\":\"" NET_ID "/status\"%s,"
           "\"dev\":{\"ids\":[\"" NET_ID "\"],\"name\":\"Erősítő\",\"mf\":\"DIY\",\"mdl\":\"" BOARD_NAME " bekapcsoló\"}}",
           name, obj, obj, extra);
  netPub(topic, js, true);
}

void netDiscovery() {
  netDisc("switch", "power", "null", ",\"cmd_t\":\"" NET_ID "/power/set\",\"ic\":\"mdi:amplifier\"");
  netDisc("switch", "trig_en", "\"Trigger figyelés\"", ",\"cmd_t\":\"" NET_ID "/trigger/set\",\"ic\":\"mdi:import\"");
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
  netDisc("sensor", "heap_min", "\"Legkisebb szabad memória\"", ",\"unit_of_meas\":\"B\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\"");
  netDisc("sensor", "heap_block", "\"Legnagyobb szabad memóriablokk\"", ",\"unit_of_meas\":\"B\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\"");
}

void mqttStart() {
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = ncfg.mqttUri;
  cfg.credentials.username = ncfg.mqttUser;
  cfg.credentials.authentication.password = ncfg.mqttPass;
  cfg.credentials.client_id = NET_ID;
  cfg.session.last_will.topic = T_STATUS;
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.session.keepalive = 30;
  cfg.network.reconnect_timeout_ms = 10000;
  cfg.buffer.size = 2048;   // az állapot JSON kényelmesen elfér
  mqtt = esp_mqtt_client_init(&cfg);
  if (!mqtt) return;
  esp_mqtt_client_register_event(mqtt, MQTT_EVENT_ANY, mqttHandler, nullptr);
  esp_mqtt_client_start(mqtt);
  netStarted = true;
}

void mqttStop() {
  if (mqtt) {
    esp_mqtt_client_stop(mqtt);
    esp_mqtt_client_destroy(mqtt);
    mqtt = nullptr;
  }
  mqttUp = false;
  netStarted = false;
}

void mqttService() {   // discovery, események, állapot
  if (!ncfg.mqttUri[0]) { xQueueReset(netQ); return; }   // nincs MQTT: az események nem gyűlnek
  if (!mqttUp) return;
  if (netNeedDisc) {
    netNeedDisc = false;
    netPub(T_STATUS, "online", true);
    netDiscovery();
    netStateDirty = true;
  }
  NetEvt e;
  while (xQueueReceive(netQ, &e, 0) == pdTRUE) {
    char tx[200], js[260];
    jsonEsc(tx, sizeof tx, e.text);
    snprintf(js, sizeof js, "{\"type\":\"%s\",\"text\":\"%s\",\"ago_s\":%lu}", e.type, tx, (unsigned long)((esp_timer_get_time() - e.t) / 1000000LL));
    netPub(T_EVENT, js, false);
  }
  if (netStateDirty || millis() - netLastState >= NET_STATE_MS) {
    char js[900];
    netStateDirty = false;
    netLastState = millis();
    buildState(js, sizeof js, false);
    netPub(T_STATE, js, true);
  }
}

// ---- WiFi: STA a beállítások szerint; sikertelenség esetén AP mód (közben újrapróbál) ----
void staBegin() {
  if (!ncfg.ssid[0]) return;
  ipApply();
  WiFi.begin(ncfg.ssid, ncfg.pass);
  WiFi.setTxPower(WIFI_TX_POWER);
  staLastBegin = millis();
}

void apStart() {
  apTried = true;
  apLastTry = millis();
  wifiMode(ncfg.ssid[0] ? WIFI_AP_STA : WIFI_AP);
  if (!WiFi.softAP(apSsid, AP_PASS)) {   // később újrapróbálja
    slogc("AP inditas sikertelen, ujraprobalas %lu s mulva\n", STA_RETRY_MS / 1000);
    return;
  }
  WiFi.setTxPower(WIFI_TX_POWER);
  dns.start(53, "*", WiFi.softAPIP());   // captive portal: minden név a weboldalra mutat
  apOn = true;
  apIdleSince = millis();
  slogc("AP mod: WiFi \"%s\", jelszo \"%s\", weboldal: http://%s (admin)\n", apSsid, AP_PASS,
        WiFi.softAPIP().toString().c_str());
}

void apStop() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  wifiMode(WIFI_STA);
  apOn = false;
  slogc("AP mod vege: csatlakozva, weboldal: http://%s\n", WiFi.localIP().toString().c_str());
}

void wifiService() {
  unsigned long now = millis();
  bool sta = WiFi.status() == WL_CONNECTED;
  if (sta) staLastOk = now;
  if (!apOn && !sta && (!ncfg.ssid[0] || now - staLastOk >= STA_TIMEOUT_MS) && (!apTried || now - apLastTry >= STA_RETRY_MS))
    apStart();
  if (apOn) {
    bool clients = WiFi.softAPgetStationNum() > 0;
    if (clients || !sta) apIdleSince = now;
    if (sta && now - apIdleSince >= AP_LINGER_MS) apStop();
    else if (!sta && ncfg.ssid[0] && now - staLastBegin >= (clients ? STA_RETRY_BUSY_MS : STA_RETRY_MS)) {   // a routert újrapróbálja
      WiFi.disconnect();
      staBegin();
    }
    if (apOn) dns.processNextRequest();
  }
}

// ---- Weboldal (jelszóval: admin / webPass) ----
#define PAGE_HEAD R"HTML(<!DOCTYPE html><html lang="hu"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="apple-mobile-web-app-capable" content="yes"><meta name="mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="Erősítő"><meta name="theme-color" content="#263238">
<link rel="apple-touch-icon" href="/apple-touch-icon.png"><link rel="icon" href="/apple-touch-icon.png"><link rel="manifest" href="/manifest.json">
<title>Erősítő</title><style>
body{font-family:-apple-system,system-ui,sans-serif;max-width:560px;margin:auto;background:#f2f2f2;color:#222;
padding:max(12px,env(safe-area-inset-top)) max(12px,env(safe-area-inset-right)) max(12px,env(safe-area-inset-bottom)) max(12px,env(safe-area-inset-left))}
button{-webkit-tap-highlight-color:transparent;touch-action:manipulation}
@media (prefers-color-scheme:dark){body{background:#000;color:#eee}section{background:#1c1c1e!important}td{border-color:#333!important}input{background:#2c2c2e;color:#eee;border:1px solid #444}}
)HTML"

const char PAGE[] = PAGE_HEAD R"HTML(
section{background:#fff;border-radius:8px;padding:12px;margin:10px 0}h1{font-size:1.3em}h2{font-size:1.05em;margin:0 0 8px}
table{width:100%;border-collapse:collapse}td{padding:4px 0;border-bottom:1px solid #eee}td+td{text-align:right;font-weight:bold}
button{padding:14px 16px;margin:4px 4px 4px 0;border:0;border-radius:10px;font-size:1.05em;color:#fff;background:#455a64}
.big{width:calc(50% - 6px)}.wide{width:100%}a{color:#888;font-size:.85em}
.on{background:#2e7d32}.off{background:#c62828}.warn{color:#c62828}
label{display:block;margin-top:8px;font-size:.9em}input{width:100%;padding:7px;box-sizing:border-box;margin-top:2px}
.st{font-size:1.6em;font-weight:bold;margin:4px 0 10px}.st.on1{color:#2e7d32}summary{font-weight:bold;cursor:pointer}
</style></head><body><h1>Erősítő bekapcsoló</h1>
<section><div id="st" class="st">Betöltés…</div>
<button class="on big" onclick="post('/api/power','state=on')">Bekapcsolás</button><button class="off big" onclick="post('/api/power','state=off')">Kikapcsolás</button>
<button id="tb" class="wide" onclick="post('/api/trigger','enabled='+(tr?0:1))">Trigger figyelés</button></section>
<section><h2>Állapot</h2><table id="t"></table></section>
<section><details><summary>Beállítások</summary><form method="post" action="/api/config">
<label>WiFi hálózat neve<input name="ssid" id="ssid" maxlength="32"></label>
<label>WiFi jelszó (üresen hagyva nem változik)<input name="pass" type="password" maxlength="64"></label>
<label>MQTT cím (pl. mqtt://192.168.1.10:1883; üresen: nincs MQTT)<input name="mqtt_uri" id="mu" maxlength="95"></label>
<label>MQTT felhasználó<input name="mqtt_user" id="mus" maxlength="32"></label>
<label>MQTT jelszó (üresen hagyva nem változik)<input name="mqtt_pass" type="password" maxlength="64"></label>
<label>Weboldal jelszó (legalább 8 karakter; üresen hagyva nem változik)<input name="web_pass" type="password" maxlength="32"></label>
<label>Fix IP-cím (üresen: DHCP, a router adja)<input name="ip" id="ip" maxlength="15" inputmode="decimal" placeholder="pl. 192.168.1.50"></label>
<label>Átjáró (router címe)<input name="gw" id="gw" maxlength="15" inputmode="decimal" placeholder="pl. 192.168.1.1"></label>
<label>Maszk (üresen: 255.255.255.0)<input name="mask" id="mask" maxlength="15" inputmode="decimal"></label>
<label>DNS (üresen: az átjáró)<input name="dns" id="dns" maxlength="15" inputmode="decimal"></label>
<button type="submit">Mentés</button></form></details></section><p><a href="/logout">Kijelentkezés</a></p>
<script>
let tr=false,cf=false;const $=i=>document.getElementById(i);
const L={rl2:'NTC áthidalva (RL2)',trigger:'Trigger jel',trig_enabled:'Trigger figyelés',hold:'Áramszünet utáni várakozás',
fault:'Hiba',warning:'Figyelmeztetés',on_hours:'Üzemidő (óra)',on_count:'Bekapcsolások',relay_count:'Relé kapcsolások',
mains_loss:'Hálózatkiesések',faults:'Hibák',opto_now:'Optó minta',opto_avg:'Optó átlag',wifi:'WiFi',ip:'IP-cím',ip_mode:'IP mód',ip_error:'IP-hiba',
rssi:'WiFi jel (dBm)',mdns:'mDNS',mqtt:'MQTT',uptime_h:'Futásidő (óra)',heap_free:'Szabad memória (B)',heap_min:'Legkisebb szabad memória (B)',heap_block:'Legnagyobb szabad blokk (B)',board:'Alaplap'};
const V=x=>x===true?'igen':x===false?'nem':x==='ON'?'BE':x==='OFF'?'KI':x;
async function upd(){try{const r=await fetch('/api/status');if(r.status==401){location='/login';return;}const s=await r.json();tr=s.trig_enabled;let h='';
for(const k in L)if(k in s&&s[k]!=='')h+='<tr><td>'+L[k]+'</td><td'+((k=='fault'||k=='warning')&&s[k]!='nincs'?' class="warn"':'')+'>'+V(s[k])+'</td></tr>';
$('t').innerHTML=h;const st=$('st');st.textContent=s.power=='ON'?'Bekapcsolva':'Kikapcsolva';st.className='st'+(s.power=='ON'?' on1':'');$('tb').textContent='Trigger figyelés: '+(tr?'BE – kikapcsolás':'KI – bekapcsolás');
if(!cf){cf=true;$('ssid').value=s.cfg_ssid;$('mu').value=s.cfg_mqtt_uri;$('mus').value=s.cfg_mqtt_user;$('ip').value=s.cfg_ip;$('gw').value=s.cfg_gw;$('mask').value=s.cfg_mask;$('dns').value=s.cfg_dns;}}catch(e){}}
async function post(u,b){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});if(r.status==401){location='/login';return;}setTimeout(upd,300);}
upd();setInterval(upd,2000);
</script></body></html>)HTML";

const char LOGIN_PAGE[] = PAGE_HEAD R"HTML(section{background:#fff;border-radius:8px;padding:16px;margin:10px 0}
input{width:100%;padding:12px;box-sizing:border-box;margin:8px 0;font-size:1.05em}
button{width:100%;padding:14px;border:0;border-radius:10px;font-size:1.05em;color:#fff;background:#455a64}.err{color:#c62828}
</style></head><body><h1>Erősítő bekapcsoló</h1><section><form method="post" action="/login">
<p class="err">%ERR%</p><label>Jelszó<input name="pw" type="password" autocomplete="current-password" autofocus></label>
<button type="submit">Belépés</button></form></section></body></html>)HTML";

const char MANIFEST[] = R"JSON({"name":"Erősítő","short_name":"Erősítő","start_url":"/","display":"standalone",
"background_color":"#f2f2f2","theme_color":"#263238","icons":[{"src":"/apple-touch-icon.png","sizes":"180x180","type":"image/png"}]})JSON";

bool cookieOk() {   // weboldal: tartós süti (a kezdőképernyőre tett app is megjegyzi)
  if (!webToken[0]) return false;
  char want[48];
  snprintf(want, sizeof want, "erosito_auth=%s", webToken);
  return strstr(web.header("Cookie").c_str(), want) != nullptr;
}

bool apiAuth() {   // API: süti (weboldal) vagy Basic jelszó (iOS-app, más kliensek)
  if (cookieOk() || web.authenticate("admin", ncfg.webPass)) return true;
  if (web.hasHeader("Authorization")) vTaskDelay(pdMS_TO_TICKS(1000));   // rossz jelszó: próbálgatás lassítása
  web.send(401, "text/plain", "Bejelentkezes szukseges");
  return false;
}

bool pageAuth() {   // oldal: bejelentkezés hiányában a belépő oldalra irányít
  if (cookieOk() || web.authenticate("admin", ncfg.webPass)) return true;
  web.sendHeader("Location", "/login");
  web.send(303, "text/plain", "");
  return false;
}

void setAuthCookie() {
  char c[96];
  snprintf(c, sizeof c, "erosito_auth=%s; Max-Age=31536000; Path=/; HttpOnly; SameSite=Strict", webToken);
  web.sendHeader("Set-Cookie", c);
}

void loginPage(const char *err) {
  String html(LOGIN_PAGE);
  html.replace("%ERR%", err);
  web.send(200, "text/html; charset=utf-8", html);
}

void webLogin() {
  if (web.method() == HTTP_GET) { loginPage(""); return; }
  if (strcmp(web.arg("pw").c_str(), ncfg.webPass) != 0) {
    vTaskDelay(pdMS_TO_TICKS(1000));   // próbálgatás lassítása
    loginPage("Hibás jelszó.");
    return;
  }
  setAuthCookie();
  web.sendHeader("Location", "/");
  web.send(303, "text/plain", "");
}

void webLogout() {
  web.sendHeader("Set-Cookie", "erosito_auth=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict");
  web.sendHeader("Location", "/login");
  web.send(303, "text/plain", "");
}

void webIcon() {   // bejelentkezés nélkül: a kezdőképernyő ikonja
  web.sendHeader("Cache-Control", "max-age=31536000");
  web.send_P(200, "image/png", (const char *)ICON_PNG, sizeof ICON_PNG);
}

void webId() {   // jelszó nélkül: csak azonosít (az iOS-app IP-szkennelése ez alapján találja meg)
  web.send(200, "application/json", "{\"device\":\"erosito\",\"board\":\"" BOARD_NAME "\"}");
}

void webManifest() {
  web.send(200, "application/manifest+json; charset=utf-8", MANIFEST);
}

void webRoot() {
  if (pageAuth()) web.send(200, "text/html; charset=utf-8", PAGE);
}

void webStatus() {
  if (!apiAuth()) return;
  char js[1600];   // leghosszabb beállításokkal is elfér
  buildState(js, sizeof js, true);
  web.send(200, "application/json; charset=utf-8", js);
}

void webPower() {   // mint a gomb / Home Assistant: ugyanazok a szabályok
  if (!apiAuth()) return;
  if (web.arg("state") == "on") remoteOnReq = true;
  else if (web.arg("state") == "off") remoteOffReq = true;
  else { web.send(400, "text/plain", "state=on vagy state=off kell"); return; }
  web.send(200, "text/plain", "OK");
}

void webTrigger() {   // csak egyértelmű kérésre változtat
  if (!apiAuth()) return;
  if (web.arg("enabled") == "1") remoteTrigReq = 1;
  else if (web.arg("enabled") == "0") remoteTrigReq = 0;
  else { web.send(400, "text/plain", "enabled=1 vagy enabled=0 kell"); return; }
  web.send(200, "text/plain", "OK");
}

// fix IP a kérés mezőiből (ip, gw, mask, dns): üres IP vagy hiba -> DHCP; nullptr = rendben, egyébként a hiba oka
const char *ipFromArgs(NetCfg &n) {
  uint32_t o[4];
  String ip = web.arg("ip"), gw = web.arg("gw"), mask = web.arg("mask"), dnsIp = web.arg("dns");
  ip.trim(); gw.trim(); mask.trim(); dnsIp.trim();
  const char *err = ip.length() ? ipCheck(ip.c_str(), gw.c_str(), mask.c_str(), dnsIp.c_str(), o) : nullptr;
  if (!ip.length() || err) {
    n.ip[0] = n.gw[0] = n.mask[0] = n.dns[0] = 0;
    return err;
  }
  char *dst[4] = {n.ip, n.gw, n.mask, n.dns};   // egységes alak (pl. 192.168.01.5 -> 192.168.1.5)
  for (int i = 0; i < 4; i++)
    snprintf(dst[i], sizeof n.ip, "%lu.%lu.%lu.%lu", (unsigned long)(o[i] >> 24), (unsigned long)((o[i] >> 16) & 0xFF),
             (unsigned long)((o[i] >> 8) & 0xFF), (unsigned long)(o[i] & 0xFF));
  if (!dnsIp.length()) n.dns[0] = 0;   // üres DNS: marad az átjáró (később is követi)
  return nullptr;
}

void webNetIp() {   // iOS-app: fix IP vagy DHCP (JSON válasz); hibás beállításnál DHCP
  if (!apiAuth()) return;
  NetCfg n = ncfg;
  const char *err = ipFromArgs(n);
  bool wifiChanged = ipCfgDiffers(n, ncfg);
  ncfg = n;
  bool ok = netCfgSave();
  requestReconfig(wifiChanged, false);
  slogc("IP beallitas (app): %s%s%s\n", ncfg.ip[0] ? ncfg.ip : "DHCP", err ? ", hiba: " : "", err ? err : "");
  char js[200];
  snprintf(js, sizeof js, "{\"ok\":%s,\"mode\":\"%s\",\"ip\":\"%s\",\"error\":\"%s\"}", (ok && !err) ? "true" : "false",
           ncfg.ip[0] ? "fix" : "dhcp", ncfg.ip, err ? err : (ok ? "" : "a mentés nem sikerült"));
  web.send(200, "application/json; charset=utf-8", js);
}

void webConfig() {   // mentés NVS-be, majd élő újracsatlakozás (nincs újraindítás: a relék nem engednek el)
  if (!pageAuth()) return;
  NetCfg n = ncfg;
  strlcpy(n.ssid, web.arg("ssid").c_str(), sizeof n.ssid);
  strlcpy(n.mqttUri, web.arg("mqtt_uri").c_str(), sizeof n.mqttUri);
  strlcpy(n.mqttUser, web.arg("mqtt_user").c_str(), sizeof n.mqttUser);
  if (web.arg("pass").length()) strlcpy(n.pass, web.arg("pass").c_str(), sizeof n.pass);
  if (web.arg("mqtt_pass").length()) strlcpy(n.mqttPass, web.arg("mqtt_pass").c_str(), sizeof n.mqttPass);
  bool pwOk = web.arg("web_pass").length() == 0 || web.arg("web_pass").length() >= 8;
  bool pwChanged = pwOk && web.arg("web_pass").length() && strcmp(web.arg("web_pass").c_str(), ncfg.webPass) != 0;
  if (pwChanged) strlcpy(n.webPass, web.arg("web_pass").c_str(), sizeof n.webPass);
  const char *ipError = ipFromArgs(n);
  bool wifiChanged = strcmp(n.ssid, ncfg.ssid) || strcmp(n.pass, ncfg.pass) || ipCfgDiffers(n, ncfg);
  bool mqttChanged = strcmp(n.mqttUri, ncfg.mqttUri) || strcmp(n.mqttUser, ncfg.mqttUser) || strcmp(n.mqttPass, ncfg.mqttPass);
  ncfg = n;
  bool ok = netCfgSave();
  if (pwChanged) {   // jelszócsere: a többi bejelentkezés érvénytelen, ez a böngésző új sütit kap
    tokenNew();
    setAuthCookie();
  }
  requestReconfig(wifiChanged, mqttChanged);
  slogc("Halozati beallitasok %s (weboldal): WiFi \"%s\", MQTT \"%s\", IP %s\n", ok ? "mentve" : "NEM mentve", ncfg.ssid,
        ncfg.mqttUri, ncfg.ip[0] ? ncfg.ip : "DHCP");
  char back[48], ipMsg[160];
  if (ncfg.ip[0]) snprintf(back, sizeof back, "http://%s/", ncfg.ip);   // fix IP: az új címre irányít
  else strlcpy(back, "/", sizeof back);
  if (ipError) snprintf(ipMsg, sizeof ipMsg, "<p>Hibás fix IP-beállítás (%s): DHCP-t használ.</p>", ipError);
  else if (ncfg.ip[0]) snprintf(ipMsg, sizeof ipMsg, "<p>Fix IP: <a href=\"%s\">%s</a></p>", back, ncfg.ip);
  else strlcpy(ipMsg, "<p>IP-cím: DHCP (a router adja; elérhető: http://" NET_ID ".local)</p>", sizeof ipMsg);
  char html[900];
  snprintf(html, sizeof html,
           "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<meta http-equiv=\"refresh\" content=\"10;url=%s\"></head>"
           "<body style=\"font-family:sans-serif;padding:16px\"><p>%s</p>%s%s<p><a href=\"%s\">Vissza</a></p></body></html>",
           back, !ok ? "HIBA: a mentés nem sikerült." : (wifiChanged ? "Mentve. Újracsatlakozás…" : "Mentve."), ipMsg,
           pwOk ? "" : "<p>A weboldal jelszó túl rövid (legalább 8 karakter), nem változott.</p>", back);
  web.send(200, "text/html; charset=utf-8", html);
}

void webNotFound() {   // AP módban minden ismeretlen cím a weboldalra irányít (captive portal)
  if (apOn) {
    web.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
    web.send(302, "text/plain", "");
  } else {
    web.send(404, "text/plain", "Nincs ilyen oldal");
  }
}

// ---- mDNS / Bonjour: erosito.local, "_erosito._tcp" és "_http._tcp" (ESPmDNS, vagy ha a core-ban nincs: saját válaszoló) ----
#if MDNS_ENABLED
void mdnsStart() {
  mdnsLastTry = millis();
  if (!MDNS.begin(NET_ID)) {
    mdnsState = 2;
    slogc("mDNS inditas sikertelen, ujraprobalas 60 s mulva\n");
    return;
  }
  MDNS.setInstanceName("Erosito");
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("erosito", "tcp", 80);
  MDNS.addServiceTxt("erosito", "tcp", "board", BOARD_NAME);
  mdnsState = 1;
  slogc("mDNS: OK, http://" NET_ID ".local\n");
}

void mdnsService() {
  if (WiFi.status() == WL_CONNECTED && (mdnsState == 0 || (mdnsState == 2 && millis() - mdnsLastTry >= 60000))) mdnsStart();
}
#else
// ---- Saját mDNS válaszoló (RFC 6762): minden rá vonatkozó kérdésre a teljes rekordkészlettel válaszol ----
// MDNS_PURE_BEGIN
const char *MD_HOST = NET_ID ".local";
const char *MD_SVC[2]  = {"_erosito._tcp.local", "_http._tcp.local"};
const char *MD_INST[2] = {"Erosito._erosito._tcp.local", "Erosito._http._tcp.local"};
const char *MD_TXT[2]  = {"board=" BOARD_NAME, "path=/"};
const char *MD_ENUM = "_services._dns-sd._udp.local";

int mdPutName(uint8_t *p, int pos, int cap, const char *dotted) {   // "a.b.c" -> DNS címkék
  for (const char *s = dotted; *s;) {
    const char *dot = strchr(s, '.');
    int n = dot ? (int)(dot - s) : (int)strlen(s);
    if (n == 0 || n > 63 || pos + n + 1 >= cap) return -1;
    p[pos++] = (uint8_t)n;
    memcpy(p + pos, s, n);
    pos += n;
    s += n;
    if (*s == '.') s++;
  }
  if (pos >= cap) return -1;
  p[pos++] = 0;
  return pos;
}

int mdPutRec(uint8_t *p, int pos, int cap, const char *name, uint16_t type, bool unique, uint32_t ttl,
             const uint8_t *rd, int rdLen) {
  pos = mdPutName(p, pos, cap, name);
  if (pos < 0 || pos + 10 + rdLen > cap) return -1;
  uint16_t cls = unique ? 0x8001 : 0x0001;   // egyedi rekordnál cache-flush bit
  p[pos++] = type >> 8; p[pos++] = type & 0xFF;
  p[pos++] = cls >> 8;  p[pos++] = cls & 0xFF;
  p[pos++] = ttl >> 24; p[pos++] = (ttl >> 16) & 0xFF; p[pos++] = (ttl >> 8) & 0xFF; p[pos++] = ttl & 0xFF;
  p[pos++] = rdLen >> 8; p[pos++] = rdLen & 0xFF;
  memcpy(p + pos, rd, rdLen);
  return pos + rdLen;
}

int mdPutNameRec(uint8_t *p, int pos, int cap, const char *name, uint16_t type, bool unique, uint32_t ttl, const char *target) {
  uint8_t rd[96];
  int n = mdPutName(rd, 0, sizeof rd, target);
  return n < 0 ? -1 : mdPutRec(p, pos, cap, name, type, unique, ttl, rd, n);
}

// teljes válasz: A, 2 x (PTR, SRV, TXT), 2 x szolgáltatás-felsorolás; legacy (nem 5353-as port): azonosító + kérdések vissza, rövid TTL
int mdBuild(uint8_t *out, int cap, const uint8_t ip[4], const uint8_t *q, int qLen, int qCount, uint16_t id, bool legacy) {
  if (cap < 12 + qLen) return -1;
  memset(out, 0, 12);
  out[0] = id >> 8; out[1] = id & 0xFF;
  out[2] = 0x84;                       // válasz, hiteles
  out[5] = legacy ? qCount : 0;
  out[7] = 9;                          // válaszrekordok száma
  int pos = 12;
  if (legacy) { memcpy(out + pos, q, qLen); pos += qLen; }
  uint32_t tHost = legacy ? 10 : 120, tSvc = legacy ? 10 : 4500;
  bool uq = !legacy;                   // legacy válaszban nincs cache-flush bit (RFC 6762 6.7)
  pos = mdPutRec(out, pos, cap, MD_HOST, 1, uq, tHost, ip, 4);
  for (int i = 0; i < 2 && pos > 0; i++) {
    pos = mdPutNameRec(out, pos, cap, MD_SVC[i], 12, false, tSvc, MD_INST[i]);
    if (pos < 0) break;
    uint8_t srv[96] = {0, 0, 0, 0, 0, 80};   // prioritás, súly, port 80
    int n = mdPutName(srv, 6, sizeof srv, MD_HOST);
    if (n < 0) return -1;
    pos = mdPutRec(out, pos, cap, MD_INST[i], 33, uq, tHost, srv, n);
    if (pos < 0) break;
    uint8_t txt[64];
    int tl = (int)strlen(MD_TXT[i]);
    if (tl > 62) return -1;
    txt[0] = (uint8_t)tl;
    memcpy(txt + 1, MD_TXT[i], tl);
    pos = mdPutRec(out, pos, cap, MD_INST[i], 16, uq, tSvc, txt, tl + 1);
  }
  for (int i = 0; i < 2 && pos > 0; i++) pos = mdPutNameRec(out, pos, cap, MD_ENUM, 12, false, tSvc, MD_SVC[i]);
  return pos;
}

int mdReadName(const uint8_t *p, int len, int pos, char *out, int outCap) {   // tömörítést is kezel; a név utáni pozíció, -1 = hiba
  int after = -1, o = 0, jumps = 0;
  for (;;) {
    if (pos >= len) return -1;
    uint8_t n = p[pos];
    if (n == 0) { pos++; break; }
    if ((n & 0xC0) == 0xC0) {
      if (pos + 1 >= len || ++jumps > 8) return -1;
      if (after < 0) after = pos + 2;
      pos = ((n & 0x3F) << 8) | p[pos + 1];
      continue;
    }
    if (n > 63 || pos + 1 + n > len || o + n + 2 > outCap) return -1;
    if (o) out[o++] = '.';
    memcpy(out + o, p + pos + 1, n);
    o += n;
    pos += 1 + n;
  }
  out[o] = 0;
  return after >= 0 ? after : pos;
}

bool mdIsOurs(const char *name) {
  if (strcasecmp(name, MD_HOST) == 0 || strcasecmp(name, MD_ENUM) == 0) return true;
  for (int i = 0; i < 2; i++)
    if (strcasecmp(name, MD_SVC[i]) == 0 || strcasecmp(name, MD_INST[i]) == 0) return true;
  return false;
}

// kérdés -> válasz hossza (0 = nem nekünk szól)
int mdHandle(const uint8_t *q, int qLen, uint8_t *out, int cap, const uint8_t ip[4], bool legacy) {
  if (qLen < 12 || (q[2] & 0x80)) return 0;   // csak kérdésre
  int qd = (q[4] << 8) | q[5], pos = 12;
  bool match = false;
  char name[128];
  for (int i = 0; i < qd && i < 16; i++) {
    int np = mdReadName(q, qLen, pos, name, sizeof name);
    if (np < 0 || np + 4 > qLen) return 0;
    uint16_t type = (q[np] << 8) | q[np + 1];
    pos = np + 4;
    if (mdIsOurs(name) && (type == 1 || type == 12 || type == 16 || type == 33 || type == 255)) match = true;
  }
  if (!match) return 0;
  int n = mdBuild(out, cap, ip, q + 12, pos - 12, qd, (q[0] << 8) | q[1], legacy);
  return n > 0 ? n : 0;
}
// MDNS_PURE_END

int mdSock = -1;
uint8_t mdIp[4];
uint8_t mdAnnounceLeft = 0;
unsigned long mdAnnounceLast = 0;

void mdClose() {
  if (mdSock >= 0) { close(mdSock); mdSock = -1; }
}

bool mdOpen(const uint8_t ip[4]) {   // UDP 5353, csatlakozás a 224.0.0.251 csoporthoz a WiFi címén
  mdClose();
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return false;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(5353);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  struct ip_mreq m = {};
  m.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
  memcpy(&m.imr_interface.s_addr, ip, 4);
  struct in_addr ifa;
  memcpy(&ifa.s_addr, ip, 4);
  uint8_t ttl = 255;
  if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 ||
      setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) < 0) {
    close(s);
    return false;
  }
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa);
  setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
  mdSock = s;
  memcpy(mdIp, ip, 4);
  mdAnnounceLeft = 3;   // bejelentés 3-szor, 1 s-onként
  mdAnnounceLast = millis() - 1000;
  return true;
}

void mdSendMulticast(const uint8_t *b, int n) {
  struct sockaddr_in d = {};
  d.sin_family = AF_INET;
  d.sin_port = htons(5353);
  d.sin_addr.s_addr = inet_addr("224.0.0.251");
  sendto(mdSock, b, n, 0, (struct sockaddr *)&d, sizeof d);
}

void mdnsService() {   // a hálózati taskból ~10 ms-onként: socket a WiFi címéhez, bejelentés, válaszok
  if (WiFi.status() != WL_CONNECTED) {
    mdClose();
    if (mdnsState == 1) mdnsState = 0;
    return;
  }
  IPAddress l = WiFi.localIP();
  uint8_t ip[4] = {l[0], l[1], l[2], l[3]};
  if (mdSock < 0 || memcmp(ip, mdIp, 4) != 0) {   // új csatlakozás vagy új IP: újra be kell lépni a csoportba
    if (mdSock < 0 && mdnsState == 2 && millis() - mdnsLastTry < 60000) return;
    mdnsLastTry = millis();
    bool ok = mdOpen(ip);
    mdnsState = ok ? 1 : 2;
    if (ok) slogc("mDNS (beepitett): OK, http://" NET_ID ".local -> %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
    else slogc("mDNS (beepitett) inditas sikertelen, ujraprobalas 60 s mulva\n");
    if (!ok) return;
  }
  static uint8_t in[512], out[600];
  if (mdAnnounceLeft && millis() - mdAnnounceLast >= 1000) {
    mdAnnounceLast = millis();
    mdAnnounceLeft--;
    int n = mdBuild(out, sizeof out, ip, nullptr, 0, 0, 0, false);
    if (n > 0) mdSendMulticast(out, n);
  }
  for (int k = 0; k < 4; k++) {
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int n = recvfrom(mdSock, in, sizeof in, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
    if (n <= 0) break;
    bool legacy = ntohs(from.sin_port) != 5353;   // egyszerű (nem mDNS) kérdező: közvetlen válasz
    int r = mdHandle(in, n, out, sizeof out, ip, legacy);
    if (r <= 0) continue;
    if (legacy) sendto(mdSock, out, r, 0, (struct sockaddr *)&from, fl);
    else mdSendMulticast(out, r);
  }
}
#endif

// ---- Hálózati task ----
void netService() {
  wifiService();
  web.handleClient();
  if (netResetReq) {   // 'W': mentett beállítások törlése, vissza a secrets.h / AP módhoz
    netResetReq = false;
    netCfgClear();
    netCfgLoad();
    tokenNew();
    requestReconfig(true, true);
    reconfAt = millis() - RECONF_DELAY_MS;   // nincs HTTP-válasz: azonnal
    slogc("Halozati beallitasok torolve\n");
  }
  if ((reconfWifi || reconfMqtt) && millis() - reconfAt >= RECONF_DELAY_MS) {   // csak ami változott; a relék nem érintettek
    mqttStop();   // WiFi-váltásnál is: új kapcsolat kell
    mqttTried = false;
    if (reconfWifi) {
      WiFi.disconnect();
      staLastOk = millis();   // új időkeret a csatlakozásra
      if (apOn) wifiMode(ncfg.ssid[0] ? WIFI_AP_STA : WIFI_AP);
      staBegin();
    }
    reconfWifi = reconfMqtt = false;
  }
  mdnsService();
  if (!netStarted && ncfg.mqttUri[0] && WiFi.status() == WL_CONNECTED && (!mqttTried || millis() - mqttLastTry >= MQTT_RETRY_MS)) {
    mqttTried = true;
    mqttLastTry = millis();
    mqttStart();
  }
  mqttService();
}

void netInit() {   // beállítások betöltése, WiFi indítása, weboldal útvonalai
  netCfgLoad();
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apSsid, sizeof apSsid, "Erosito-%02X%02X", mac[4], mac[5]);
  WiFi.persistent(false);   // a WiFi könyvtár ne írja a flash-t
  WiFi.setHostname(NET_ID);
  WiFi.setAutoReconnect(true);
  wifiMode(WIFI_STA);
  if (!webToken[0]) tokenNew();   // a rádió már fut: valódi véletlenszám
  staLastOk = millis();
  staBegin();
  const char *hdrs[] = {"Cookie", "Authorization"};
  web.collectHeaders(hdrs, 2);
  web.on("/", HTTP_GET, webRoot);
  web.on("/login", HTTP_ANY, webLogin);
  web.on("/logout", HTTP_GET, webLogout);
  web.on("/apple-touch-icon.png", HTTP_GET, webIcon);
  web.on("/apple-touch-icon-precomposed.png", HTTP_GET, webIcon);
  web.on("/favicon.ico", HTTP_GET, webIcon);
  web.on("/manifest.json", HTTP_GET, webManifest);
  web.on("/api/id", HTTP_GET, webId);
  web.on("/api/status", HTTP_GET, webStatus);
  web.on("/api/power", HTTP_POST, webPower);
  web.on("/api/trigger", HTTP_POST, webTrigger);
  web.on("/api/config", HTTP_POST, webConfig);
  web.on("/api/netip", HTTP_POST, webNetIp);
  web.onNotFound(webNotFound);
  web.begin();
  slogc("Halozat: %s\n", ncfg.ssid[0] ? "csatlakozas a mentett / secrets.h WiFi-hez" : "nincs WiFi beallitas, AP mod");
}

void netTask(void *) {
  netInit();
  for (;;) {
    netService();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void netStart() {
  netQ = xQueueCreate(16, sizeof(NetEvt));
  if (netQ) xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, AUX_PRIO, &netTaskH, AUX_CORE);
}

void netStatusLine() {
  bool sta = WiFi.status() == WL_CONNECTED;
  slogc("  Halozat: WiFi %s%s (%d dBm), IP %s (%s%s%s), mDNS %s, MQTT %s\n", sta ? "OK" : "nincs", apOn ? " + AP" : "",
        (int)WiFi.RSSI(), sta ? WiFi.localIP().toString().c_str() : (apOn ? WiFi.softAPIP().toString().c_str() : "-"),
        ipMode == 1 ? "fix" : "DHCP", ipErr ? ", hibas fix IP: " : "", ipErr ? ipErr : "", mdnsName(),
        !ncfg.mqttUri[0] ? "nincs beallitva" : (mqttUp ? "OK" : "nincs"));
}
#else
void netEvent(const char *, const char *, ...) {}
void netStart() {}
void netStatusLine() {}
#endif

// ======================================================================= Soros parancsok, kiírások
void printHelp() {
  slogc("Parancsok: s = allapot, h = allapotfigyeles, B = opto alapertek torles (optocsere), R = rele szamlalo nullazas (relecsere)%s\n",
        NET_ENABLED ? ", W = halozati beallitasok torlese" : "");
}

void printStatus() {
  char lk[48] = "";
  if (powerHold) snprintf(lk, sizeof lk, " [aramszunet utan: %lu s]", timeLeft(holdStart, holdDur) / 1000);
  else if (lockoutLeft() > 0) snprintf(lk, sizeof lk, " [tiltas: %lu ms%s]", lockoutLeft(), autoPaused() ? ", ismetlodo" : "");
  else if (autoPaused()) snprintf(lk, sizeof lk, lossPaused() ? " [sorozatos kieses: auto szunetel]" : " [ismetlodo hiba: trigger szunetel]");
  char rq[48] = "";
  if (onReqBtn || onReqTrig) snprintf(rq, sizeof rq, " [bekapcs. %s, %lu ms]", onReqBtn ? "gomb" : "trigger", minOffLeft());
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
  slogc("  Futasido: %lu ora, szabad memoria: %lu bajt (legkisebb: %lu, legnagyobb blokk: %lu)\n", uptimeHours(),
        (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  {
    const char *tn;
    uint32_t sf = stackMinFree(&tn);
    slogc("  Szabad verem (legkevesebb): %lu bajt (%s)\n", (unsigned long)sf, tn);
  }
  if (MAINS_CHECK) {
    unsigned a = (unsigned)(st.optoAvg * 10 + 0.5f), b = (unsigned)(st.optoBase * 10 + 0.5f),
             l = (unsigned)(lastSessMean * 10 + 0.5f);
    if (st.optoBaseN == 0)
      slogc("  Opto LOW minta: meg nincs adat (legalabb ~10 s bekapcsolas kell)\n");
    else if (st.optoBaseN >= OPTO_BASE_SESSIONS)
      slogc("  Opto LOW minta: atlag %u,%u, alapertek %u,%u, utolso %u,%u\n", a / 10, a % 10, b / 10, b % 10, l / 10, l % 10);
    else
      slogc("  Opto LOW minta: atlag %u,%u, alapertek: gyujtes (%lu / %u szakasz)\n", a / 10, a % 10,
            (unsigned long)st.optoBaseN, (unsigned)OPTO_BASE_SESSIONS);
  }
  char w[120];
  if (statsWarnings(w, sizeof w)) slogc("  FIGYELMEZTETES:%s\n", w);
  else slogc("  Allapot: rendben\n");
}

bool statusAllowed() {   // a soros kiírások legfeljebb STATUS_MIN_MS időnként
  if (millis() - lastStatus < STATUS_MIN_MS) return false;
  lastStatus = millis();
  return true;
}

void handleRemoteTrigger() {   // weboldal / Home Assistant: trigger figyelés be / ki (mentve, mint a két rövid nyomás)
  int8_t r = remoteTrigReq;
  if (r < 0) return;
  remoteTrigReq = -1;
  if ((r == 0) != trigOff) toggleTrigOff();
}

void handleSerial() {
  if (statusReq) { statusReq = false; if (statusAllowed()) printStatus(); }
  if (helpReq)   { helpReq = false;   if (statusAllowed()) printHelp(); }
  if (healthReq) { healthReq = false; if (statusAllowed()) printHealth(); }
  if (baseReq) {   // optocsere után: új alapérték a következő mérési szakaszokból
    baseReq = false;
    st.optoAvg = 0;
    st.optoBase = 0;
    st.optoBaseN = 0;
    lastSessMean = 0;
    stDirty = true;
    statsSave(true);
    slogc("Opto alapertek torolve: a kovetkezo %u meresi szakaszbol ujra kepzodik\n", (unsigned)OPTO_BASE_SESSIONS);
  }
  if (relayReq) {   // relécsere után
    relayReq = false;
    st.relayCount = 0;
    stDirty = true;
    statsSave(true);
    slogc("Rele szamlalo nullazva\n");
  }
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

// ======================================================================= Indulás
void initOutputs() {   // relék és LED KI (a pinMode előtti írás hatástalan, ezért utána is)
  pinMode(PIN_RL1, OUTPUT);
  pinMode(PIN_RL2, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_RL1, LOW);
  digitalWrite(PIN_RL2, LOW);
  digitalWrite(PIN_LED, LOW);
}

bool loadSettings() {   // NVS: trigger tiltás, mentett hiba, állapotfigyelés
  bool ok = prefs.begin("relay", false);
  if (ok) {
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
  return ok;
}

void initInputs() {
  pinMode(PIN_BTN,  INPUT_PULLUP);
  pinMode(PIN_TRIG, INPUT_PULLUP);
  if (MAINS_CHECK) {
    pinMode(PIN_FB1, INPUT);   // külső 43k felhúzó
    mainsCmd();
    fbGraceOver = true;   // induláskor nincs türelmi idő
  }
  for (int i = 0; i < 50; i++) {   // a bemenetek beállnak, közben az első mérési ablak
    mainsUpdate();
    delay(1);
  }
  resyncInputs();
}

void initWatchdog() {   // csak a vezérlő ciklust figyeli
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms     = WDT_MS;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic  = true;
  if (esp_task_wdt_init(&cfg) == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&cfg);
  esp_task_wdt_add(nullptr);
}

void setup() {
  initOutputs();
  Serial.begin(115200);
  logQ = xQueueCreate(24, sizeof(LogMsg));   // tele sornál az üzenet elvész, a vezérlés nem áll meg
  if (logQ) xTaskCreatePinnedToCore(logTask, "slog", 4096, nullptr, AUX_PRIO, &logTaskH, AUX_CORE);
  loopTaskH = xTaskGetCurrentTaskHandle();   // a setup és a loop ugyanabban a taskban fut
  bool nvsOk = loadSettings();
  initInputs();
  initWatchdog();

  esp_reset_reason_t rr = esp_reset_reason();
  bool crashReset = (rr == ESP_RST_TASK_WDT || rr == ESP_RST_INT_WDT || rr == ESP_RST_WDT || rr == ESP_RST_PANIC ||
                     (rr == ESP_RST_BROWNOUT && !BROWNOUT_IS_POWERLOSS));
  bool powerLoss = (rr == ESP_RST_POWERON) || (rr == ESP_RST_BROWNOUT && BROWNOUT_IS_POWERLOSS);
  unsigned long holdMs = powerLoss ? POWERUP_HOLD_MS : holdLoad();   // reset a várakozás alatt: folytatódik
  if (holdMs > 0) {
    powerHold = true;
    holdStart = lastHoldSave = millis();
    holdDur = holdMs;
    holdSave(holdMs);
  } else {
    holdClear();
  }

  slogc("\n=== Erosito bekapcsolo indul (" BOARD_NAME ", core %d.%d.%d, %s) ===\n", ESP_ARDUINO_VERSION_MAJOR,
        ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH, resetName(rr));
  slogc("TRIGGER: %s, %s; halozatfigyeles: %s\n", trig.stable == LOW ? "aktiv" : "nincs",
        trigOff ? "TILTVA" : "engedelyezve", MAINS_CHECK ? "BE" : "KI");
  if (!nvsOk) slogc("FIGYELEM: NVS hiba, alapertelmezett beallitasok\n");
  if (powerHold) slogc("Aramszunet utan: %lu s varakozas\n", holdMs / 1000);
  if (powerLoss) { st.powerUps++; stDirty = true; statsSave(true); }
  printHelp();
  netStart();
  netEvent("boot", "Indulás: %s%s", resetName(rr), powerHold ? ", áramszünet utáni várakozás" : "");
  checkWarnings();
  if (lastWarnCount > 0) {
    char w[120];
    statsWarnings(w, sizeof w);
    slogc("FIGYELMEZTETES:%s ('h' = reszletek)\n", w);
    netEvent("warning", "Öregedési figyelmeztetés induláskor");
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

// ======================================================================= Fő ciklus
void handleFaultBlink(unsigned long now, bool btnChanged) {   // hibavillogás: a bemenetek nem számítanak
  if (mainsLost()) {   // bemenet-hiba alatt (relék még BE) is azonnal KI
    setState(false, "halozatkieses hiba alatt");
    slogc("!!! HALOZATKIESES hiba alatt: relek AZONNAL KI\n");
  }
  if (btnChanged) lastShortRel = now;
  remoteOnReq = remoteOffReq = false;   // hiba alatt a távoli parancs elvész
  if (now - faultStart >= FAULT_BLINK_MS) endFault();
}

void endPowerHold() {
  powerHold = false;
  netStateDirty = true;
  holdClear();
  resyncInputs();
  slogc("Aramszunet utani varakozas vege\n");
  if (!lockoutPending && !autoPaused() && !noMainsWait && TRIGGER_ON_BOOT && !trigOff && trig.stable == LOW)
    switchOn("aktiv trigger aramszunet utan", false);
}

void handlePowerHold(unsigned long now, bool btnChanged) {   // áramszünet utáni várakozás: trigger hatástalan, kézi bekapcsolás egyszer
  if (timeLeft(holdStart, holdDur) == 0) { endPowerHold(); return; }
  if (now - lastHoldSave >= 1000) {
    lastHoldSave = now;
    holdSave(timeLeft(holdStart, holdDur));
  }
  if (on) {
    int mf = mainsFault();
    if (mf != F_NONE) startFault(IN_FB1, mf);
  }
  if (!faultActive) holdButton(now, btnChanged);
  handleRemote(true);
  processPendingOn(true);
}

void handleNormal(bool btnChanged, bool trigChanged) {
  int mf = mainsFault();
  if (mf != F_NONE && (on || !faultHold)) startFault(IN_FB1, mf);
  if (!faultActive && !faultHold) checkInputFaults();
  if (!faultActive) buttonLogic(btnChanged);
  handleRemote(false);
  if (!faultActive && trigChanged && !trigOff) triggerEdge();
  processPendingOn(false);
  if (!faultActive && lockoutPending && lockoutLeft() == 0) endLockout();
  if (faultStreak && !faultActive) {   // a sorozat csak akkor nullázódik, ha a bemenetek közben is nyugodtak (KI relénél is figyelve)
    if (checkFault(IN_BTN) != F_NONE || (!trigOff && checkFault(IN_TRIG) != F_NONE)) faultQuietT = millis();
  }
  if (faultStreak && !faultActive && !lockoutPending && millis() - faultQuietT >= FAULT_STREAK_RESET_MS) {
    bool wasPaused = autoPaused();   // FAULT_STREAK_RESET_MS hibamentes idő: a sorozat nullázódik
    faultStreak = 0;
    slog("Hibasorozat nullazva (%lu s hibamentes)\n", FAULT_STREAK_RESET_MS / 1000);
    if (wasPaused && !autoPaused() && AUTO_ON_AFTER_FAULT && !on && !trigOff && trig.stable == LOW && !noMainsWait)
      switchOn("aktiv trigger, ismetlodo hiba utan stabil", false);
  }
  if (lossStreak && !faultActive && millis() - lossLastT >= FAULT_STREAK_RESET_MS) {
    bool wasPaused = autoPaused();
    lossStreak = 0;
    slog("Halozatkieses-sorozat nullazva (%lu s kieses nelkul)\n", FAULT_STREAK_RESET_MS / 1000);
    if (wasPaused && !autoPaused() && !lockoutPending && !noMainsWait && !on && !trigOff && trig.stable == LOW)
      switchOn("aktiv trigger, sorozatos kieses utan", false);
  }
}

void handleStatsTimer(unsigned long now) {   // halasztott mentés (STATS_MIN_GAP_MS), bekapcsolva óránként mentés
  if (stDirty && !on && now - stLastSave >= STATS_MIN_GAP_MS) statsSave();   // bekapcsolva a kikapcsolás / az óránkénti mentés viszi
  if (!on || now - stLastSave < STATS_SAVE_MS) return;
  statsAccount();
  statsFold();
  statsSave(true);
  stLastSave = now;
  checkWarnings();
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();

  bool btnChanged  = updateInput(IN_BTN);
  bool trigChanged = updateInput(IN_TRIG);
  mainsUpdate();
  if (rl2On && !rl2Settled && now - rl2Time >= RL2_MIN_ON_MS) rl2Settled = true;   // letelt állapotok rögzítése:
  if (minOffActive && !on && now - offTime >= MIN_OFF_MS) minOffActive = false;     // régi időbélyeg nem zavarhat be
  if (trigChanged) netStateDirty = true;

  if (faultActive)    handleFaultBlink(now, btnChanged);
  else if (powerHold) handlePowerHold(now, btnChanged);
  else                handleNormal(btnChanged, trigChanged);

  updateRl2(now);
  ledUpdate();
  handleRemoteTrigger();
  handleSerial();
  handleStatsTimer(now);
  logWindow();
  delay(1);
}
