#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <functional>
#include "Arduino.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <esp_system.h>
unsigned long T=0; int hiMs=2; bool quiet=false; void (*hook)()=nullptr; long nvsBool=0, nvsUChar=0; int pins[64]; bool mains=true; int rr=ESP_RST_EXT; int ledOnCount=0, prevLed=0;
bool rl1(){return pins[D10];} bool rl2(){return pins[D5];} bool spk(){return pins[D7];}
struct PinInit{ PinInit(){ pins[D8]=1; } } pinInit_;
int prevRL1=0, prevRL2=0;
unsigned long millis(){return (uint32_t)T;}
void step(); void delay(unsigned long ms){ for(unsigned long i=0;i<ms;i++){T++; step();} }
void digitalWrite(int p,int v){pins[p]=v;}
bool railFail[2]={false,false}; bool railPow=false, railChg=false; unsigned long railUpSince=0, railDownSince=0;
int digitalRead(int p){ if(p==D2||p==D6){ return (railChg && !railFail[p==D6]) ? LOW : HIGH; } if(p==D4){ if(mains && rl1()){ return ((T%10)<(unsigned)hiMs)?HIGH:LOW; } return HIGH;} return pins[p]; }
void pinMode(int,int){}
S_ Serial;
long nvsBegins=0; bool Preferences::begin(const char*,bool){ nvsBegins++; static int f=getenv("NVSFAIL")?1:0; return !f;} void Preferences::end(){}
#include <map>
#include <string>
std::map<std::string,uint32_t> nvsNum;
bool Preferences::getBool(const char*k,bool d){ auto it=nvsNum.find(k); return it==nvsNum.end()? d : (bool)it->second;} uint8_t Preferences::getUChar(const char*k,uint8_t d){ auto it=nvsNum.find(k); return it==nvsNum.end()? d : (uint8_t)it->second;}
uint32_t Preferences::getUInt(const char*k,uint32_t d){ auto it=nvsNum.find(k); return it==nvsNum.end()? d : it->second;} size_t Preferences::putUInt(const char*k,uint32_t v){ nvsNum[k]=v; return 4;}
size_t Preferences::putBool(const char*k,bool v){ nvsBool++; nvsNum[k]=v; if(!quiet) printf("  [%7.3f] NVS %s=%d\n",T/1000.0,k,v); return 1;} size_t Preferences::putUChar(const char*k,uint8_t v){ nvsUChar++; nvsNum[k]=v; return 1;}
static unsigned char blob[256]; static size_t blobLen=0; int nvsWrites=0;
size_t Preferences::putBytes(const char*,const void*p,size_t n){memcpy(blob,p,n);blobLen=n;nvsWrites++;return n;}
size_t Preferences::getBytes(const char*,void*p,size_t n){ if(blobLen!=n) return 0; memcpy(p,blob,n); return n;}
#include <deque>
#include <vector>
struct Q{int sz; std::deque<std::vector<char>> d; int cap;}; static Q logq{0,{},0};
QueueHandle_t xQueueCreate(int n,int sz){ static int k=0; if(k++==0){logq.sz=sz;logq.cap=n;return &logq;} Q*q=new Q{sz,{},n}; return q;}
bool qFull=false; int xQueueSend(QueueHandle_t h,const void*m,int){ if(h!=&logq){Q*q=(Q*)h; if((int)q->d.size()>=q->cap) return 0; q->d.emplace_back((const char*)m,(const char*)m+q->sz); return 1;} if(qFull) return 0; if(quiet) return 1; printf("  [%7.3f] %s", T/1000.0, (const char*)m); return 1;}
int xQueueReset(QueueHandle_t h){ if(h!=&logq) ((Q*)h)->d.clear(); return 1;}
int xQueueReceive(QueueHandle_t h,void*m,int){ if(h==&logq) return 0; Q*q=(Q*)h; if(q->d.empty()) return 0; memcpy(m,q->d.front().data(),q->sz); q->d.pop_front(); return 1;}
void vTaskDelay(unsigned){}
int xTaskCreatePinnedToCore(void(*)(void*),const char*,int,void*,UBaseType_t,void*,BaseType_t){return 1;}
int xTaskCreate(void(*)(void*),const char*,int,void*,int,void*){return 1;}
#include <esp_task_wdt.h>
esp_err_t esp_task_wdt_init(const esp_task_wdt_config_t*){return 0;} esp_err_t esp_task_wdt_reconfigure(const esp_task_wdt_config_t*){return 0;}
esp_err_t esp_task_wdt_add(void*){return 0;} esp_err_t esp_task_wdt_reset(){return 0;}
esp_reset_reason_t esp_reset_reason(){return (esp_reset_reason_t)rr;}
#include <esp_timer.h>
uint64_t T64=0; int64_t esp_timer_get_time(){return (int64_t)T64*1000;}
#ifndef HB
#define HB 90000
#endif
#ifndef SF
#define SF 2000
#endif
size_t heapBlock=HB; size_t heap_caps_get_free_size(uint32_t); size_t heap_caps_get_minimum_free_size(uint32_t); struct SimRestart{}; int restarts=0; void esp_restart(){ restarts++; throw SimRestart(); } size_t heap_caps_get_largest_free_block(uint32_t){return heapBlock;} void* xTaskGetCurrentTaskHandle(){return (void*)1;} UBaseType_t stackFree=SF; UBaseType_t uxTaskGetStackHighWaterMark(void*){return stackFree;}
uint32_t heapFree=150000, heapMin=120000; uint32_t esp_get_free_heap_size(){return heapFree;} uint32_t esp_get_minimum_free_heap_size(){return heapMin;} size_t heap_caps_get_free_size(uint32_t){return heapFree;} size_t heap_caps_get_minimum_free_size(uint32_t){return heapMin;}
#include <WiFi.h>
#include <mqtt_client.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <map>
// --- szimulált WiFi: a "router" ki-be kapcsolható, a kapcsolat begin() után 3 s-mal jön létre ---
bool routerUp=true; std::string routerSsid="otthon", routerPass="jelszo123"; int beginCount=0; unsigned long beginT=0; std::string curSsid, curPass;
wifi_mode_t curMode=WIFI_OFF; bool apUp=false; int apClients=0; int8_t rssiV=-61;
WiFiClass WiFi; void WiFiClass::persistent(bool){} bool WiFiClass::setHostname(const char*){return true;}
bool WiFiClass::mode(wifi_mode_t m){ if(m!=curMode) printf("  [%7.3f] WIFI mod: %s\n",T/1000.0, m==WIFI_STA?"STA":m==WIFI_AP?"AP":m==WIFI_AP_STA?"AP+STA":"OFF"); curMode=m; if(m==WIFI_STA) apUp=false; return true;}
bool WiFiClass::setAutoReconnect(bool){return true;}
wl_status_t WiFiClass::begin(const char*s_,const char*p_){ beginCount++; beginT=T; curSsid=s_; curPass=p_; return WL_DISCONNECTED;}
bool WiFiClass::setTxPower(wifi_power_t){return true;}
wl_status_t WiFiClass::status(){ bool ok = routerUp && curSsid==routerSsid && curPass==routerPass && T-beginT>=3000 && (curMode==WIFI_STA||curMode==WIFI_AP_STA); return ok?WL_CONNECTED:WL_DISCONNECTED;}
int8_t WiFiClass::RSSI(){return status()==WL_CONNECTED?rssiV:0;}
int discCount=0; unsigned long lastDisc=0; bool WiFiClass::disconnect(bool,bool){ curSsid=""; discCount++; lastDisc=T; return true;}
bool WiFiClass::softAP(const char*n,const char*){ apUp=true; printf("  [%7.3f] AP indul: %s\n",T/1000.0,n); return true;}
IPAddress WiFiClass::softAPIP(){IPAddress a; a.b[0]=192;a.b[1]=168;a.b[2]=4;a.b[3]=1; return a;}
uint8_t WiFiClass::softAPgetStationNum(){return apUp?apClients:0;}
bool WiFiClass::softAPdisconnect(bool){ apUp=false; printf("  [%7.3f] AP leall\n",T/1000.0); return true;}
uint8_t* WiFiClass::macAddress(uint8_t*m){ for(int i=0;i<6;i++) m[i]=0x10+i; return m;}
bool WiFiClass::setSleep(bool){return true;}
bool cfgStatic=false; IPAddress cfgIp; int configCalls=0;
bool WiFiClass::config(IPAddress a, IPAddress, IPAddress, IPAddress, IPAddress){ configCalls++; cfgStatic = a.b[0]!=0; cfgIp=a; return true; }
IPAddress WiFiClass::localIP(){ if(cfgStatic) return cfgIp; IPAddress a; a.b[0]=192;a.b[1]=168;a.b[2]=1;a.b[3]=50; return a;}
// --- szimulált webszerver: a kezelők közvetlenül hívhatók ---
std::map<std::string,std::function<void(void)>> routes; std::function<void(void)> nf; std::map<std::string,std::string> reqArgs; std::string lastBody; int lastCode=0; std::string authPass="";
WebServer::WebServer(int){} void WebServer::on(const char*u,HTTPMethod,std::function<void(void)> f){routes[u]=f;} void WebServer::onNotFound(std::function<void(void)> f){nf=f;}
void WebServer::begin(){} void WebServer::handleClient(){}
bool WebServer::authenticate(const char*u,const char*p){ return std::string(u)=="admin" && authPass==p;}
void WebServer::requestAuthentication(HTTPAuthMethod,const char*,const String&){ lastCode=401; lastBody="AUTH";}
String WebServer::arg(const char*n){ auto it=reqArgs.find(n); return it==reqArgs.end()?String(""):String(it->second);}
void WebServer::send(int c,const char*,const char*b){lastCode=c; lastBody=b;} void WebServer::send(int c,const char*,const String&b){lastCode=c; lastBody=b.s;}
std::string reqCookie; HTTPMethod reqMethod=HTTP_GET; std::map<std::string,std::string> respHdr;
void WebServer::sendHeader(const char*k,const String&v,bool){ respHdr[k]=v.s; }
void WebServer::collectHeaders(const char*[],const size_t){} String WebServer::header(const char*){return String(reqCookie);} bool WebServer::hasHeader(const char*k){ return std::string(k)=="Authorization" && !authPass.empty(); } HTTPMethod WebServer::method(){return reqMethod;}
void WebServer::send_P(int c,const char*,const char*b,size_t n){lastCode=c; lastBody.assign(b,n);}
uint32_t esp_random(){ static uint32_t x=0x12345678; x=x*1664525u+1013904223u; return x; }
bool DNSServer::start(uint16_t,const char*,const IPAddress&){return true;} void DNSServer::stop(){} void DNSServer::processNextRequest(){}
void httpReq(const char*u, std::map<std::string,std::string> a, const char*pw, const char*cookie="", HTTPMethod m=HTTP_POST){ reqArgs=a; authPass=pw; reqCookie=cookie; reqMethod=m; respHdr.clear(); lastCode=0; lastBody=""; auto it=routes.find(u); if(it!=routes.end()) it->second(); else if(nf) nf(); }
// --- NVS a "net" névtérhez ---
std::map<std::string,std::string> nvsStr; bool nvsNetExists=false;
bool Preferences::isKey(const char*k){return nvsStr.count(k)>0;}
size_t Preferences::getString(const char*k,char*v,size_t n){ auto it=nvsStr.find(k); if(it==nvsStr.end()) return 0; strlcpy(v,it->second.c_str(),n); return it->second.size();}
size_t Preferences::putString(const char*k,const char*v){ nvsStr[k]=v; return strlen(v);}
bool Preferences::clear(){ nvsStr.clear(); return true;}
FILE *pubLog=nullptr; bool showPub=false;
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t*){return (esp_mqtt_client_handle_t)1;}
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t, esp_mqtt_event_id_t, esp_event_handler_t, void*){return 0;}
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t){return 0;}
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t){return 0;} esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t){return 0;}
int esp_mqtt_client_subscribe_single(esp_mqtt_client_handle_t, const char*, int){return 1;}
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t, const char*t, const char*d, int, int, int r, bool){
  if(pubLog) fprintf(pubLog,"%s\t%d\t%s\n",t,r,d); if(showPub && strstr(t,"/event")) printf("  [%7.3f] MQTT %s %s\n",T/1000.0,t,d); return 1;}
#include <ESPmDNS.h>
MDNSResponder MDNS; bool MDNSResponder::begin(const char*){return true;} void MDNSResponder::setInstanceName(String){} bool MDNSResponder::addService(const char*,const char*,uint16_t){return true;} bool MDNSResponder::addServiceTxt(const char*,const char*,const char*,const char*){return true;}
#include "prog.inc"
#if NET_ENABLED
void netPoll(){ netService(); }
void mqttCmd(const char*d){ esp_mqtt_event_t ev={}; ev.topic=(char*)T_CMD; ev.topic_len=strlen(T_CMD); ev.data=(char*)d; ev.data_len=strlen(d); mqttHandler(nullptr,nullptr,MQTT_EVENT_DATA,&ev);}
void mqttConnect(){ esp_mqtt_event_t ev={}; mqttHandler(nullptr,nullptr,MQTT_EVENT_CONNECTED,&ev);}
#endif
std::function<void()> scen;

// ================= terheleses teszt: veletlen bemenetek + invariansok minden ms-ban =================
#include <random>
unsigned long traceFrom=~0UL, traceTo=0;
struct Stress {
  std::mt19937 rng; int bootReason=0; bool armed=false;
  bool btnDown=false; unsigned long btnUntil=0, btnNext=3000, bounceUntil=0;
  unsigned long trgNext=8000, trgBounceUntil=0, chatterUntil=0, chatterNext=0; bool trgWant=false;
  unsigned long mainsUntil=0, mainsNext=30000; bool mainsWant=true;
  unsigned long rl1On=0, rl1Off=0, rl2On=0, lastMainsDown=0, mainsDownSince=0; bool p1=false, p2=false, haveRl1Off=true;
  bool holdManual=false; int faultToggles=0; bool pf=false; int pled=0; unsigned long faultOnSince=0;
  unsigned long dcUntil=0, dcNext=20000, dcLowSince=0, spkOnT=0; bool dcWant=false, ps=false, pfDc=false; long spkOns=0, dcEvents=0, dcGlitches=0, dcFaults=0;
  unsigned long railUntil=0, railNext=30000, railHighSince[2]={0,0}; int railSide=0; bool pfRail=false; long railGlitch=0, railEvents=0, railFaults=0;
  unsigned long lastBtnDownT=0; bool sevPrev=false; long sevBlocks=0;
  long viol=0, ons=0, rl2s=0, fastOffs=0, faultBlinks=0, presses=0, longPresses=0, trgToggles=0, drops=0;
  double u(){ return std::uniform_real_distribution<double>(0,1)(rng); }
  unsigned long r(unsigned long a, unsigned long b){ return std::uniform_int_distribution<unsigned long>(a,b)(rng); }
  void bad(const char* m){ viol++; if(viol<=15) printf("  SERTES @%.3f s: %s (on=%d rl2On=%d fault=%d lockout=%d hold=%d mains=%d)\n",T/1000.0,m,(int)on,(int)rl2On,(int)faultActive,(int)lockoutPending,(int)powerHold,(int)mains); }
} X;

void checkInv(Stress &x){
  // ---- invariansok ----
  bool r1=rl1(), r2=rl2();
  if(!mains){ if(!x.mainsDownSince) x.mainsDownSince=T; x.lastMainsDown=T; } else x.mainsDownSince=0;
  if(r2 && !r1) x.bad("RL2 bekapcsolva RL1 nelkul");
  if(r1 && !x.p1 && x.sevPrev && T - x.lastBtnDownT > 7000) x.bad("bekapcsolas mentett DC / tap hiba mellett, gomb nelkul");
  if(r1 && !x.p1){ x.ons++;
    if(x.haveRl1Off && T - x.rl1Off < MIN_OFF_MS) x.bad("minimalis kikapcsolt ido megsertve");
    if(lockoutPending && lockoutLeft()>0) x.bad("bekapcsolas hiba utani tiltas alatt");
    if(x.bootReason==ESP_RST_POWERON && T < POWERUP_HOLD_MS && !x.holdManual) x.bad("bekapcsolas aramszunet utani varakozas alatt kezi keres nelkul");
    x.rl1On=T; }
  if(r2 && !x.p2){ x.rl2s++;
    if(T - x.rl1On < RL2_DELAY_MS) x.bad("RL2 tul korai (lagyinditas)");
    if(!mains && T - x.mainsDownSince > MAINS_GAP_MS) x.bad("RL2 behuzas halozat nelkul"); x.rl2On=T; }
  if(!r1 && x.p1){
    bool shortRl2 = !x.p2 ? (x.rl2On < x.rl1On) : (T - x.rl2On < RL2_MIN_ON_MS);
    bool fastOk = faultActive || lockoutPending || (T - x.lastMainsDown < 500);
    if(shortRl2){ if(fastOk) x.fastOffs++; else x.bad("kikapcsolas az RL2 2 s-os minimuma elott, hiba nelkul"); }
    x.rl1Off=T; }
  if(r2 && !mains && T - std::max(x.mainsDownSince, x.rl2On) > 40) x.bad("halozatkieses: RL2 40 ms utan is be");
  if(r1 && !mains && T - std::max(x.mainsDownSince, x.rl1On) > 300) x.bad("halozat nelkul: RL1 300 ms utan is be");
  { bool sp=spk(); bool dcl = pins[D8]==LOW;
    if(dcl){ if(!x.dcLowSince) x.dcLowSince=T; } else x.dcLowSince=0;
    if(sp && !(r1 && r2)) x.bad("hangszorok RL1 / RL2 nelkul");
    if(sp && !x.ps){ x.spkOns++; if(T - x.rl2On < SPK_DELAY_MS) x.bad("hangszorok tul koran (RL2 + 1 s elott)"); }
    if(sp && dcl && T - x.dcLowSince > DC_FILTER_MS + 2) x.bad("DC mellett a hangszorok bekapcsolva");
    if(sp && faultActive) x.bad("hiba alatt a hangszorok bekapcsolva");
    bool fdc = faultActive && lastFault==F_DC;
    if(fdc && !x.pfDc){ x.dcFaults++; if(!dcl || T - x.dcLowSince < DC_FILTER_MS) x.bad("DC-hiba rovid tuskere"); }
    x.pfDc=fdc; x.ps=sp; }
  int led=pins[D1];
  if(!faultActive && ledFlashes==0 && !((powerHold || autoPaused() || sevFault) && !on) && led != (int)on) x.bad("LED nem koveti a releket");
  if(faultActive){ if(!x.pf){ x.faultToggles=0; x.faultOnSince=T; } if(led!=x.pled) x.faultToggles++;
    if(r1 && T - x.faultOnSince > FAULT_BLINK_MS + 5) x.bad("hiba-villogas alatt a relek tul sokaig be"); }
  if(!faultActive && x.pf){ x.faultBlinks++; if(x.faultToggles < 6) x.bad("hibanal a LED nem villogott"); }
  { for(int i=0;i<2;i++){ bool hi = digitalRead(i?D6:D2)==HIGH; if(hi){ if(!x.railHighSince[i]) x.railHighSince[i]=T; } else x.railHighSince[i]=0; }
    auto hiFor=[&](int i){ return x.railHighSince[i] ? T - x.railHighSince[i] : 0UL; };
    if(spk() && (hiFor(0) > RAIL_FILTER_MS + 2 || hiFor(1) > RAIL_FILTER_MS + 2)) x.bad("hangszorok hianyzo erosito-tap mellett");
    bool fr = faultActive && lastFault==F_RAIL;
    if(fr && !x.pfRail){ x.railFaults++; if(hiFor(0) < RAIL_FILTER_MS && hiFor(1) < RAIL_FILTER_MS) x.bad("tap-hiba rovid tuskere"); }
    x.pfRail=fr; }
  if(sevFault && !x.sevPrev) x.sevBlocks++;
  x.sevPrev = sevFault!=0;
  x.pf=faultActive; x.pled=led; x.p1=r1; x.p2=r2;
}

void stressHook(){
  Stress &x = X;
  // ---- gomb: rovid / hosszu / nagyon hosszu (beragadas), sorozatok, pergessel ----
  if(!x.btnDown && T>=x.btnNext){
    double k=x.u(); unsigned long dur = k<0.55? x.r(60,300) : k<0.85? x.r(450,1500) : k<0.97? x.r(300,460) : x.r(9000,14000);
    x.btnDown=true; x.lastBtnDownT=T; x.btnUntil=T+dur; x.bounceUntil=T+x.r(0,15); x.presses++;
    if(dur>=430){ x.longPresses++; if(powerHold) x.holdManual=true; }
  }
  if(x.btnDown && T>=x.btnUntil){ x.btnDown=false; x.bounceUntil=T+x.r(0,15); x.btnNext=T+(x.u()<0.35? x.r(120,700) : x.r(2000,40000)); }
  pins[PIN_BTN] = (T<x.bounceUntil) ? (x.rng()&1) : (x.btnDown?LOW:HIGH);
  // ---- trigger: veletlen valtasok pergessel, idonkent zajos sorozat ----
  if(T>=x.trgNext){ x.trgWant=!x.trgWant; x.trgToggles++; x.trgBounceUntil=T+x.r(0,20); x.trgNext=T+(x.u()<0.06? x.r(50,600) : x.r(3000,60000)); }
  if(T>=x.chatterNext){ if(x.u()<0.08) x.chatterUntil=T+x.r(500,2500); x.chatterNext=T+x.r(20000,120000); }
  bool tv = x.trgWant;
  if(T<x.chatterUntil) tv = ((T/ (40+ (x.chatterUntil%90)))&1);
  pins[D3] = (T<x.trgBounceUntil) ? (x.rng()&1) : (tv?LOW:HIGH);
  // ---- halozat: rovid tuske / kozepes / hosszu kieses ----
  if(x.mainsWant && T>=x.mainsNext){ double k=x.u(); unsigned long d = k<0.5? x.r(3,15) : k<0.8? x.r(30,400) : x.r(1000,8000);
    x.mainsWant=false; x.mainsUntil=T+d; x.drops++; }
  if(!x.mainsWant && T>=x.mainsUntil){ x.mainsWant=true; x.mainsNext=T+x.r(20000,200000); }
  mains = x.mainsWant;
  // ---- tavoli es soros parancsok ----
  if(x.r(0,40000)==0){ remoteOnReq=true; if(powerHold) x.holdManual=true; }
  if(x.r(0,40000)==0) remoteOffReq=true;
  if(x.r(0,90000)==0) remoteTrigReq = x.rng()&1;
  if(x.r(0,20000)==0) statusReq=true;
  if(x.r(0,30000)==0) healthReq=true;
  if(T>=traceFrom && T<=traceTo){ static int pr1=-1,pr2=-1,pm=-1; if(rl1()!=pr1||rl2()!=pr2||(int)mains!=pm){ printf("    t=%.3f RL1=%d RL2=%d halozat=%d fault=%d lastFault=%d on=%d fbConfirmed=%d\n",T/1000.0,rl1(),rl2(),(int)mains,(int)faultActive,lastFault,(int)on,(int)fbConfirmed); pr1=rl1(); pr2=rl2(); pm=mains; } }
  if(!x.armed) return;
  checkInv(x);
  // ---- erosito-tap: kieses bekapcsolt allapotban (tuske < 50 ms vagy valodi) ----
  if(!railFail[0] && !railFail[1] && T>=x.railNext){ if(rl1() && railChg){ double k=x.u(); unsigned long d = k<0.5? x.r(1,40) : x.r(80,4000); x.railSide=x.rng()%3; x.railUntil=T+d; if(d<=40) x.railGlitch++; else x.railEvents++;
      railFail[0] = x.railSide!=1; railFail[1] = x.railSide!=0; } x.railNext=T+x.r(20000,200000); }
  if((railFail[0]||railFail[1]) && T>=x.railUntil){ railFail[0]=railFail[1]=false; }
  // ---- hangszorovedo: DC csak bekapcsolt erositonel; tuske (<15 ms) vagy valodi DC ----
  if(!x.dcWant && T>=x.dcNext){ if(rl1()){ double k=x.u(); unsigned long d = k<0.6? x.r(1,15) : x.r(40,3000); x.dcWant=true; x.dcUntil=T+d; if(d<=15) x.dcGlitches++; else x.dcEvents++; } x.dcNext=T+x.r(15000,150000); }
  if(x.dcWant && (T>=x.dcUntil || !rl1())){ x.dcWant=false; }
  pins[D8] = x.dcWant ? LOW : HIGH;
}

void step(){
  T64++;
  { bool pw = rl1() && mains; if(pw && !railPow){ railPow=true; railUpSince=T; } if(!pw && railPow){ railPow=false; railDownSince=T; }
    if(railPow && T-railUpSince>=800) railChg=true; if(!railPow && T-railDownSince>=1500) railChg=false; }
  if(scen) scen();
  if(hook) hook();
  if(!quiet && (rl1()!=prevRL1||rl2()!=prevRL2)){ printf("  [%7.3f] >>> HW: RL1=%d RL2=%d\n",T/1000.0,rl1(),rl2()); prevRL1=rl1(); prevRL2=rl2(); }
  if(pins[D1] && !prevLed) ledOnCount++; prevLed=pins[D1]; prevRL1=rl1(); prevRL2=rl2();
}
void run(unsigned long until){ while(T<until){ loop(); } }
void setBtn(bool down){ pins[PIN_BTN]=down?LOW:HIGH; } void setTrig(bool a){ pins[D3]=a?LOW:HIGH; }
#include "week.inc"
int main(int argc,char**argv){
  int sc=atoi(argv[1]);
  if(sc==68) return weekMain(argc,argv);
  pins[PIN_BTN]=HIGH; pins[D3]=HIGH;
  if(sc==1){ puts("== S1: POWERON, trigger aktiv -> 120 s varakozas, LED felvillan, gomb hatastalan");
    rr=ESP_RST_POWERON; setTrig(true);
    scen=[]{ if(T==30000) setBtn(true); if(T==31000) setBtn(false); };
    setup(); run(125000); printf("  LED felvillanasok: %d\n", ledOnCount); }
  if(sc==2){ puts("== S2: EXT reset, trigger aktiv -> be 5 s-nal; trigger elmegy 5.5 s-nal -> RL2 6 s, KI 8 s");
    rr=ESP_RST_EXT; setTrig(true);
    scen=[]{ if(T==5500) setTrig(false); };
    setup(); run(10000); }
  if(sc==3){ puts("== S3: halozatkieses bekapcsolva 20.000-20.300 s -> azonnal KI, 5 s utan aktiv triggerre ujra be");
    rr=ESP_RST_EXT; setTrig(true);
    scen=[]{ if(T==20000) mains=false; if(T==20300) mains=true; };
    setup(); run(28000); }
  if(sc==4){ puts("== S4: gomb KI 7.0 s-nal (RL2 6.0 s-tol) -> KI 8.0 s; kozben nyomasok hatastalanok; 9 s-nal hosszu nyomas -> BE 13.0 s");
    rr=ESP_RST_EXT; setTrig(false);
    scen=[]{ if(T==500) setBtn(true); if(T==1200) setBtn(false);
             if(T==7000) setBtn(true); if(T==7600) setBtn(false);
             if(T==7700) setBtn(true); if(T==7800) setBtn(false);
             if(T==9000) setBtn(true); if(T==9600) setBtn(false);
             if(T==10000) setBtn(true); if(T==10700) setBtn(false); };
    setup(); run(15000); }
  if(sc==5){ puts("== S5: watchdog reset a varakozas alatt (RTC: 60 s maradt) -> nem kapcsol be azonnal, 60 s-nal be");
    rr=ESP_RST_TASK_WDT; setTrig(true); holdSave(60000);
    setup(); run(63000); }
  if(sc==6){ puts("== S6: kieses ujrakapcsolas, de a halozat nem jon vissza -> F_NOMAINS, nincs tobb probalkozas");
    rr=ESP_RST_EXT; setTrig(true);
    scen=[]{ if(T==20000) mains=false; };
    setup(); run(40000); }
  if(sc==8){ puts("== S8: gombos KI fuggoben (RL2 minimum), kozben halozatkieses -> azonnal KI, NINCS ujrakapcsolas (trigger aktiv)");
    rr=ESP_RST_EXT; setTrig(true);
    scen=[]{ if(T==6500) setBtn(true); if(T==7100) setBtn(false); if(T==7500) mains=false; if(T==7700) mains=true; };
    setup(); run(15000); }
  if(sc==9){ puts("== S9: 3 rovid nyomas (bemenet-hiba, relek BE villogas alatt), kozben kieses -> azonnal KI");
    rr=ESP_RST_EXT; setTrig(true);
    scen=[]{ if(T==8000) setBtn(true); if(T==8100) setBtn(false); if(T==8250) setBtn(true); if(T==8350) setBtn(false);
             if(T==8500) setBtn(true); if(T==8600) setBtn(false); if(T==8900) mains=false; if(T==9200) mains=true; };
    setup(); run(16000); }
  if(sc==10){ puts("== S10: kieses ujrakapcsolas kozben is nincs halozat -> F_NOMAINS NEM kerul NVS-be; ujabb kieses utan sem marad tiltva");
    rr=ESP_RST_EXT; setTrig(true);
    scen=[]{ if(T==20000) mains=false; if(T==30000) mains=true; };
    setup(); run(32000); printf("  storedFault=%d (0 = nincs mentve)\n", storedFault); }
  if(sc==11){ puts("== S11: kieses utan a bemenet-hibafigyeles nem szunetel (3 rovid nyomas 22 s-nal -> hiba)");
    rr=ESP_RST_EXT; setTrig(false);
    scen=[]{ if(T==500) setBtn(true); if(T==1200) setBtn(false); if(T==20000) mains=false; if(T==20300) mains=true;
             if(T==22000) setBtn(true); if(T==22100) setBtn(false); if(T==22250) setBtn(true); if(T==22350) setBtn(false);
             if(T==22500) setBtn(true); if(T==22600) setBtn(false); };
    setup(); run(25000); }
  if(sc==12){ puts("== S12: aramszunet, 30 s-nal hosszu nyomas -> BE; 40 s / 50 s nyomas hatastalan; 60 s kieses -> KI; trigger nem aktiv");
    rr=ESP_RST_POWERON; setTrig(false);
    scen=[]{ if(T==30000) setBtn(true); if(T==30600) setBtn(false);
             if(T==40000) setBtn(true); if(T==40600) setBtn(false);
             if(T==50000) setBtn(true); if(T==50600) setBtn(false);
             if(T==60000) mains=false; if(T==60300) mains=true; };
    setup(); run(125000); }
  if(sc==13){ puts("== S13: aramszunet, 2 s-nal hosszu nyomas (MIN_OFF a boottol) -> BE 5 s-nal; varakozas vegen nyomas -> KI normalisan");
    rr=ESP_RST_POWERON; setTrig(true);
    scen=[]{ if(T==2000) setBtn(true); if(T==2600) setBtn(false);
             if(T==121000) setBtn(true); if(T==121600) setBtn(false); };
    setup(); run(124000); }
  if(sc==14){ puts("== S14: 6 ep ciklus (alapertek), majd az opto gyengul (LOW ~16/40), 5 ciklus -> figyelmeztetes");
    rr=ESP_RST_EXT; setTrig(false);
    scen=[]{ unsigned long c=T/70000, t=T%70000;
             if(t==6000) setTrig(true); if(t==66000) setTrig(false);
             if(T==6*70000) hiMs=6; };
    setup(); run(6*70000); healthReq=true; run(6*70000+100);
    run(12*70000); healthReq=true; run(12*70000+100); statusReq=true; run(12*70000+1200);
    printf("  NVS stats irasok: %d\n", nvsWrites); }
  if(sc==15){ puts("== S15: rele 80%% (79999 -> 80000) figyelmeztetes, majd 'R' nullazas");
    rr=ESP_RST_EXT; setTrig(true);
    setup(); st.relayCount=79999; run(8000); healthReq=true; run(8100); relayReq=true; run(8200); run(9300); healthReq=true; run(9400); }
  if(sc==16){ puts("== S16: a soros kimenet beragadt (a sor mindig tele) -> a vezerles ugyanugy fut");
    qFull=true; rr=ESP_RST_EXT; setTrig(true);
    scen=[]{ if(T==5500) setTrig(false); if(T==20000) setTrig(true); if(T==30000) mains=false; if(T==30300) mains=true; };
    setup(); run(40000); printf("  elveszett uzenetek (szamlalo): %lu + mar jelentett\n", logDropped); }
  if(sc==17){ puts("== S17: 25 napig bekapcsolva, utana a trigger elmegy -> azonnal KI kell");
    rr=ESP_RST_EXT; setTrig(true);
    setup(); run(10000); T += 25UL*86400000UL; run(T+2000); printf("  most: %.3f nap\n", T/86400000.0);
    unsigned long t0=T; setTrig(false); run(T+10000); printf("  RL1=%d (0 = kikapcsolt), %lu ms-mal a trigger utan ellenorizve\n", rl1(), T-t0); }
  if(sc==18){ puts("== S18: a millis() tulcsordulasa (49,7 nap) korul: be / ki / kieses / hossz. nyomas");
    rr=ESP_RST_EXT; setTrig(false);
    setup(); run(6000); T = 4294967296UL - 8000; run(T+100);
    scen=[]{ uint32_t m=(uint32_t)T;
             if(m==4294967296UL-5000) setTrig(true);
             if(m==3000) setTrig(false);
             if(m==12000) setTrig(true);
             if(m==20000) mains=false; if(m==20300) mains=true;
             if(m==40000) setBtn(true); if(m==40600) setBtn(false); };
    run(T+60000); }
  if(sc==19){ puts("== S19: 60 napig kikapcsolva (tulcsordulason at), utana trigger -> azonnal BE (nincs hamis 5 s varakozas)");
    rr=ESP_RST_EXT; setTrig(false);
    setup(); run(10000); setTrig(true); run(20000); setTrig(false); run(30000);
    T += 60UL*86400000UL; run(T+1000); unsigned long t0=T; setTrig(true); run(T+3000);
    printf("  RL1=%d, a trigger utan %s\n", rl1(), rl1() ? "bekapcsolt" : "NEM kapcsolt be"); }
#if NET_ENABLED
  if(sc==20){ puts("== S20: Home Assistant: discovery + allapot + esemenyek, tavoli BE/KI, varakozas alatt tavoli BE egyszer");
    pubLog=fopen("pub.txt","w"); showPub=true;
    rr=ESP_RST_POWERON; setTrig(false);
    nvsStr={{"ssid","otthon"},{"pass","jelszo123"},{"mqttUri","mqtt://192.168.1.10:1883"},{"mqttUser","u"},{"mqttPass","p"},{"webPass","admin1234"}}; setup(); netInit();
    scen=[]{ static unsigned long lp=0; if(T-lp>=100){lp=T; netPoll(); if(mqtt && !mqttUp) mqttConnect();}
             if(T==20000) mqttCmd("ON");      // varakozas alatt: egyszer
             if(T==30000) mqttCmd("OFF");     // KI (RL2 min. utan)
             if(T==40000) mqttCmd("ON");      // varakozas alatt mar felhasznalva -> hatastalan
             if(T==130000) mqttCmd("ON");     // varakozas utan: mint a gomb
             if(T==131000) mqttCmd("ON");     // mar be: hatastalan
             if(T==140000) mqttCmd("OFF");
             if(T==141000) mqttCmd("ON");     // min. kikapcsolt ido alatt: fuggo
             if(T==142000) mqttCmd("OFF");    // fuggo bekapcsolas torlese
             if(T==160000) mains=false; if(T==160300) mains=true; };
    run(150000); mqttCmd("ON"); run(170000); fclose(pubLog); }
#endif
#if NET_ENABLED
  if(sc==21){ puts("== S21: nincs WiFi beallitas -> azonnal AP; weboldalon mentes -> csatlakozik, AP 60 s mulva leall; a relek nem erintettek");
    rr=ESP_RST_EXT; setTrig(true); setup(); netInit();
    scen=[]{ static unsigned long lp=0; if(T-lp>=10){lp=T; netPoll();}
             if(T==5500){ apClients=1; puts("  (telefon csatlakozik az AP-ra)"); }
             if(T==20000){ httpReq("/api/config",{{"ssid","x"}},"rossz"); printf("  rossz jelszo: HTTP %d\n",lastCode);
                           httpReq("/api/config",{{"ssid","otthon"},{"pass","jelszo123"},{"mqtt_uri",""},{"mqtt_user",""}},"erosito123");
                           printf("  mentes: HTTP %d, NVS ssid=%s\n",lastCode,nvsStr["ssid"].c_str()); }
             if(T==40000){ apClients=0; puts("  (telefon lecsatlakozik)"); } };
    run(110000); printf("  WiFi: %s, AP: %s, begin hivasok: %d\n", WiFi.status()==WL_CONNECTED?"csatlakozva":"nincs", apUp?"fut":"all", beginCount); }
  if(sc==22){ puts("== S22: aramszunet utan a router lassabban indul (90 s) -> 30 s utan AP, 30 s-onkent ujraprobal, csatlakozas utan az AP leall");
    nvsStr={{"ssid","otthon"},{"pass","jelszo123"},{"mqttUri",""},{"mqttUser",""},{"mqttPass",""},{"webPass","admin1234"}};
    routerUp=false; rr=ESP_RST_POWERON; setTrig(true); setup(); netInit();
    scen=[]{ static unsigned long lp=0; if(T-lp>=10){lp=T; netPoll();} if(T==90000){routerUp=true; puts("  (router elindult)");} };
    run(200000); printf("  WiFi: %s, AP: %s, begin hivasok: %d\n", WiFi.status()==WL_CONNECTED?"csatlakozva":"nincs", apUp?"fut":"all", beginCount); }
  if(sc==23){ puts("== S23: weboldal: BE / KI / trigger figyeles, statusz JSON, idezojeles SSID");
    nvsStr={{"ssid","o\"tt\\hon"},{"pass","x"},{"mqttUri",""},{"mqttUser",""},{"mqttPass",""},{"webPass","admin1234"}};
    rr=ESP_RST_EXT; setTrig(false); setup(); netInit();
    scen=[]{ static unsigned long lp=0; if(T-lp>=10){lp=T; netPoll();}
             if(T==6000) httpReq("/api/power",{{"state","on"}},"admin1234");
             if(T==10000) httpReq("/api/power",{{"state","off"}},"admin1234");
             if(T==12000) httpReq("/api/trigger",{{"enabled","0"}},"admin1234");
             if(T==13000) httpReq("/api/trigger",{{"enabled","0"}},"admin1234");
             if(T==14000) httpReq("/api/trigger",{{"enabled","1"}},"admin1234");
             if(T==15000){ httpReq("/api/status",{},"admin1234"); FILE*f=fopen("status.json","w"); fputs(lastBody.c_str(),f); fclose(f); }
             if(T==15001){ httpReq("/api/status",{},""); printf("  jelszo nelkul: HTTP %d\n",lastCode);} };
    run(16000); }
  if(sc==24){ puts("== S24: a telefon megjegyezte az AP-t es rajta marad; a router 40-200 s kozott nem elerheto -> kliens mellett is ujraprobal (5 percenkent)");
    nvsStr={{"ssid","otthon"},{"pass","jelszo123"},{"mqttUri",""},{"mqttUser",""},{"mqttPass",""},{"webPass","admin1234"}};
    rr=ESP_RST_EXT; setTrig(false); setup(); netInit();
    scen=[]{ static unsigned long lp=0; if(T-lp>=10){lp=T; netPoll();}
             if(T==40000){routerUp=false; curSsid=""; puts("  (router leall)");} if(T==75000){apClients=1; puts("  (telefon raall az AP-ra, es ott marad)");}
             if(T==200000){routerUp=true; puts("  (router ujra el)");} };
    run(800000); printf("  WiFi: %s, AP: %s (kliens: %d), begin hivasok: %d\n", WiFi.status()==WL_CONNECTED?"csatlakozva":"nincs", apUp?"fut":"all", apClients, beginCount); }
  if(sc==25){ puts("== S25: 60 napos futas: futasido es esemeny-eletkor; heap figyelmeztetes");
    nvsStr={{"ssid","otthon"},{"pass","jelszo123"},{"mqttUri","mqtt://x"},{"mqttUser",""},{"mqttPass",""},{"webPass","admin1234"}};
    pubLog=fopen("pub25.txt","w"); rr=ESP_RST_EXT; setTrig(false); setup(); netInit();
    scen=[]{ static unsigned long lp=0; if(T-lp>=100){lp=T; netPoll();} };
    run(10000); T += 60UL*86400000UL; T64 += 60ULL*86400000ULL; heapMin=15000;
    unsigned long t0=T; setBtn(true); run(t0+600); setBtn(false); run(t0+2000);   // esemeny MQTT nelkul (meg nem csatlakozott)
    mqttConnect(); run(t0+3000);
    httpReq("/api/status",{},"admin1234"); FILE*f=fopen("status25.json","w"); fputs(lastBody.c_str(),f); fclose(f); fclose(pubLog);
    healthReq=true; run(t0+4500); }
  if(sc==26){ puts("== S26: bejelentkezes sutivel (kezdokepernyos webapp), Basic (iOS-app), jelszocsere, ikon, manifest");
    nvsStr={{"ssid","otthon"},{"pass","jelszo123"},{"mqttUri",""},{"mqttUser",""},{"mqttPass",""},{"webPass","admin1234"}};
    rr=ESP_RST_EXT; setTrig(false); setup(); netInit();
    printf("  token NVS-ben: %s\n", nvsStr.count("token")?"van":"NINCS");
    httpReq("/",{},"","",HTTP_GET); printf("  / belepes nelkul: HTTP %d -> %s\n",lastCode,respHdr["Location"].c_str());
    httpReq("/api/status",{},"","",HTTP_GET); printf("  /api/status belepes nelkul: HTTP %d\n",lastCode);
    httpReq("/login",{},"","",HTTP_GET); printf("  /login GET: HTTP %d, urlap: %s\n",lastCode, lastBody.find("name=\"pw\"")!=std::string::npos?"van":"NINCS");
    httpReq("/login",{{"pw","rossz"}},"","",HTTP_POST); printf("  rossz jelszo: HTTP %d, uzenet: %s\n",lastCode, lastBody.find("Hibás jelszó")!=std::string::npos?"van":"NINCS");
    httpReq("/login",{{"pw","admin1234"}},"","",HTTP_POST); std::string sc=respHdr["Set-Cookie"]; printf("  jo jelszo: HTTP %d -> %s, suti: %.45s...\n",lastCode,respHdr["Location"].c_str(),sc.c_str());
    std::string ck=sc.substr(0,sc.find(';'));
    httpReq("/",{},"",ck.c_str(),HTTP_GET); printf("  / sutivel: HTTP %d\n",lastCode);
    httpReq("/api/status",{},"",ck.c_str(),HTTP_GET); printf("  /api/status sutivel: HTTP %d\n",lastCode);
    httpReq("/api/status",{},"admin1234","",HTTP_GET); printf("  /api/status Basic jelszoval (iOS-app): HTTP %d\n",lastCode);
    httpReq("/api/config",{{"ssid","otthon"},{"mqtt_uri",""},{"mqtt_user",""},{"web_pass","ujjelszo99"}},"",ck.c_str(),HTTP_POST);
    std::string sc2=respHdr["Set-Cookie"]; std::string ck2=sc2.substr(0,sc2.find(';')); printf("  jelszocsere: HTTP %d, uj suti: %s\n",lastCode, (ck2!=ck && !ck2.empty())?"igen":"NEM");
    httpReq("/api/status",{},"",ck.c_str(),HTTP_GET); printf("  regi sutivel: HTTP %d\n",lastCode);
    httpReq("/api/status",{},"",ck2.c_str(),HTTP_GET); printf("  uj sutivel: HTTP %d\n",lastCode);
    httpReq("/logout",{},"",ck2.c_str(),HTTP_GET); printf("  kijelentkezes: HTTP %d, suti: %s\n",lastCode,respHdr["Set-Cookie"].c_str());
    httpReq("/apple-touch-icon.png",{},"","",HTTP_GET); printf("  ikon belepes nelkul: HTTP %d, %zu bajt, PNG: %s\n",lastCode,lastBody.size(), lastBody.compare(1,3,"PNG")==0?"igen":"NEM");
    httpReq("/manifest.json",{},"","",HTTP_GET); FILE*f=fopen("manifest.json","w"); fputs(lastBody.c_str(),f); fclose(f);
    httpReq("/",{},"","",HTTP_GET); httpReq("/",{},"ujjelszo99","",HTTP_GET); printf("  / Basic jelszoval: HTTP %d\n",lastCode);
    FILE*g=fopen("page.html","w"); fputs(lastBody.c_str(),g); fclose(g); }
  if(sc==27){ puts("== S27: fix IP weboldalrol es appbol; hibas beallitas -> DHCP; a relek nem erintettek");
    nvsStr={{"ssid","otthon"},{"pass","jelszo123"},{"mqttUri",""},{"mqttUser",""},{"mqttPass",""},{"webPass","admin1234"}};
    rr=ESP_RST_EXT; setTrig(true); setup(); netInit();
    scen=[]{ static unsigned long lp=0; if(T-lp>=10){lp=T; netPoll();}
      auto st=[](const char*t){ httpReq("/api/status",{},"admin1234","",HTTP_GET); size_t a=lastBody.find("\"ip\":"); size_t m=lastBody.find("\"ip_mode\":"); size_t e=lastBody.find("\"ip_error\":");
        printf("  [%7.3f] %s: %s | %s | %s\n",T/1000.0,t,lastBody.substr(a,22).c_str(),lastBody.substr(m,25).c_str(),lastBody.substr(e,50).c_str()); };
      if(T==8000) st("indulas (DHCP)");
      if(T==10000){ httpReq("/api/config",{{"ssid","otthon"},{"mqtt_uri",""},{"mqtt_user",""},{"ip","192.168.01.77"},{"gw","192.168.1.1"},{"mask",""},{"dns",""}},"admin1234");
                    printf("  weboldal mentes: HTTP %d, NVS ip=%s gw=%s mask=%s, atiranyitas: %s\n",lastCode,nvsStr["ip"].c_str(),nvsStr["gw"].c_str(),nvsStr["mask"].c_str(), lastBody.find("url=http://192.168.1.77/")!=std::string::npos?"az uj cimre":"NEM"); }
      if(T==16000) st("fix IP utan");
      if(T==18000){ httpReq("/api/netip",{{"ip","192.168.1.80"},{"gw","192.168.2.1"},{"mask",""},{"dns",""}},"admin1234"); printf("  app, hibas atjaro: %s\n",lastBody.c_str()); }
      if(T==24000) st("hibas beallitas utan");
      if(T==26000){ httpReq("/api/netip",{{"ip","192.168.1.90"},{"gw","192.168.1.1"},{"mask","255.255.255.0"},{"dns","1.1.1.1"}},"admin1234"); printf("  app, jo: %s\n",lastBody.c_str()); }
      if(T==32000) st("app utan");
      if(T==34000){ httpReq("/api/netip",{{"ip",""}},"admin1234"); printf("  app, ures IP: %s\n",lastBody.c_str()); }
      if(T==40000) st("vissza DHCP-re"); };
    run(41000); printf("  WiFi.config hivasok: %d\n", configCalls); }
  if(sc==28){ puts("== S28: csak MQTT-modositas nem bontja a WiFit; WiFi-valtas 1,5 s-mal a valasz utan; hibas kerdesre 400");
    nvsStr={{"ssid","otthon"},{"pass","jelszo123"},{"mqttUri",""},{"mqttUser",""},{"mqttPass",""},{"webPass","admin1234"}};
    rr=ESP_RST_EXT; setTrig(false); setup(); netInit();
    scen=[]{ static unsigned long lp=0; if(T-lp>=10){lp=T; netPoll();}
      if(T==8000){ int d0=discCount; httpReq("/api/config",{{"ssid","otthon"},{"mqtt_uri","mqtt://192.168.1.10:1883"},{"mqtt_user","u"},{"ip",""}},"admin1234");
                   printf("  csak MQTT mentve: HTTP %d, valasz: %s\n", lastCode, lastBody.find("Újracsatlakozás")!=std::string::npos?"ujracsatlakozas":"mentve (WiFi marad)"); (void)d0; }
      if(T==12000) printf("  WiFi bontasok az MQTT-mentes utan: %d (vart: 0)\n", discCount);
      if(T==14000){ httpReq("/api/netip",{{"ip","192.168.1.90"},{"gw","192.168.1.1"}},"admin1234"); printf("  [%7.3f] IP mentve, valasz kiment: HTTP %d\n",T/1000.0,lastCode); }
      if(T==18000) printf("  WiFi bontas ideje: %.3f s (a valasz utan %.1f s)\n", lastDisc/1000.0, (lastDisc-14000)/1000.0);
      if(T==19000){ httpReq("/api/trigger",{},"admin1234"); printf("  /api/trigger parameter nelkul: HTTP %d, trigger figyeles: %s\n",lastCode, trigOff?"KI (HIBA)":"marad BE"); }
      if(T==19001){ httpReq("/api/power",{{"state","xyz"}},"admin1234"); printf("  /api/power state=xyz: HTTP %d\n",lastCode); } };
    run(20000); }
  if(sc==63){   // memoria: 30 nap app-lekerdezes, weboldal, mentesek, WiFi / MQTT kiesesek; heap naponta
    nvsStr={{"ssid","otthon"},{"pass","jelszo123"},{"mqttUri","mqtt://x"},{"mqttUser",""},{"mqttPass",""},{"webPass","admin1234"}};
    quiet=true; rr=ESP_RST_EXT; pins[PIN_BTN]=HIGH; setTrig(false); X.rng.seed(5); setup(); netInit();
    int days = argc>2 ? atoi(argv[2]) : 30; long reqs=0, saves=0, drops=0;
    size_t h1=0; size_t hmax=0;
    for(int d=0; d<days; d++){
      unsigned long dayEnd = T + 86400000UL;
      while(T<dayEnd){
        loop();
        if(T%100==0){ netPoll(); if(mqtt && !mqttUp && routerUp) mqttConnect(); }
        if(T%2000==0){ httpReq("/api/status",{},"admin1234","",HTTP_GET); reqs++; }
        if(T%60000==7){ httpReq("/",{},"","",HTTP_GET); httpReq("/login",{},"","",HTTP_GET); httpReq("/login",{{"pw","rossz"}},"","",HTTP_POST); httpReq("/manifest.json",{},"","",HTTP_GET); httpReq("/api/id",{},"","",HTTP_GET); httpReq("/nincs",{},"","",HTTP_GET); }
        if(T%300000==11){ httpReq("/api/power",{{"state", (T/300000)&1?"on":"off"}},"admin1234"); httpReq("/api/trigger",{{"enabled",(T/600000)&1?"0":"1"}},"admin1234"); }
        if(T%3600000==13){ httpReq("/api/config",{{"ssid","otthon"},{"pass","jelszo123"},{"mqtt_uri",(T/3600000)&1?"mqtt://x":"mqtt://y"},{"mqtt_user",""}},"admin1234"); saves++; }
        if(T%7200000==17){ httpReq("/api/netip",{{"ip",(T/7200000)&1?"192.168.1.77":""},{"gw","192.168.1.1"},{"mask",""},{"dns",""}},"admin1234"); saves++; }
        if(T%5400000==19){ routerUp=false; drops++; }
        if(T%5400000==19+120000){ routerUp=true; }
        if(T%900000==23){ esp_mqtt_event_t ev={}; mqttHandler(nullptr,nullptr,MQTT_EVENT_DISCONNECTED,&ev); }
        if(T%10000==29){ setTrig(!pins[D3]==0); }
      }
      struct mallinfo2 mi = mallinfo2(); size_t h = mi.uordblks;
      if(d==0) h1=h; if(h>hmax) hmax=h;
      if(d==0||d==1||d==days-1||d%10==9) printf("  nap %2d: heap foglalt %zu bajt\n", d+1, h);
    }
    printf("63: %d nap, %ld lekerdezes, %ld mentes, %ld WiFi-kieses; heap 1. nap %zu, max %zu, vege %zu -> %s\n", days, reqs, saves, drops, h1, hmax,
      mallinfo2().uordblks, (mallinfo2().uordblks <= h1 + 256) ? "NEM NO" : "NO!");
    return 0; }
#endif
  if(sc==60){   // argv: 60 <seed> <ora> <reset ok: 1=tapfeltoltes 2=kulso 7=watchdog>
    X.rng.seed(atoi(argv[2])); if(argc>6){ traceFrom=atol(argv[5]); traceTo=atol(argv[6]); } double hours=atof(argv[3]); rr=argc>4?atoi(argv[4]):ESP_RST_EXT; X.bootReason=rr;
    quiet=true; pins[PIN_BTN]=HIGH; pins[D3]=HIGH; hook=stressHook;
#if NET_ENABLED
    nvsStr={{"ssid","otthon"},{"pass","jelszo123"},{"mqttUri",""},{"mqttUser",""},{"mqttPass",""},{"webPass","admin1234"}};
#endif
    setup();
#if NET_ENABLED
    netInit(); scen=[]{ static unsigned long lp=0; if(T-lp>=10){lp=T; netPoll();} };
#endif
    X.armed=true; X.p1=rl1(); X.p2=rl2();
    unsigned long end=(unsigned long)(hours*3600000.0); while(T<end) loop();
    long nvs = nvsWrites + nvsBool + nvsUChar;
    printf("seed %s, %.1f ora, reset %d: sertes %ld | bekapcs %ld, RL2 %ld, gombnyomas %ld (hosszu %ld), trigger valtas %ld, halozati zavar %ld | hibak %lu, kiesesek %lu, hibavillogas %ld, gyors (hiba miatti) kikapcs %ld | NVS iras %.1f/ora\n",
      argv[2], hours, rr, X.viol, X.ons, X.rl2s, X.presses, X.longPresses, X.trgToggles, X.drops, (unsigned long)st.faults, (unsigned long)st.mainsLoss, X.faultBlinks, X.fastOffs, nvs/hours);
    printf("  hangszorok BE %ld, DC tuske %ld, valodi DC %ld, DC-hiba %ld, tap tuske %ld, valodi tapkieses %ld, tap-hiba %ld, mentett hiba %ld\n", X.spkOns, X.dcGlitches, X.dcEvents, X.dcFaults, X.railGlitch, X.railEvents, X.railFaults, X.sevBlocks);
    return X.viol?1:0; }
  if(sc==67){   // DC / tap hiba: mentes, trigger KI, csak fizikai gomb; ujrainditas utan is
    quiet=!getenv("LOUD"); rr=ESP_RST_EXT; pins[PIN_BTN]=HIGH; setTrig(true); int fails=0;
    auto ok=[&](bool c,const char*t){ printf("  %s %s\n", c?"OK  ":"HIBA", t); if(!c) fails++; };
    auto runTo=[&](unsigned long t){ while(T<t) loop(); };
    auto press=[&](){ setBtn(true); runTo(T+600); setBtn(false); };
    if(argc>2 && argv[2][0]=='H'){   // aramszunet utan, mentett hibaval: a tavoli BE nem fogyasztja el a gomb lehetoseget; LED
      nvsNum["sev"]=F_RAIL; nvsNum["trigOff"]=1; rr=ESP_RST_POWERON; setup(); runTo(10000);
      int fl=0, pv=0; for(unsigned long e=T+3000; T<e;){ loop(); int l=pins[D1]; if(l&&!pv) fl++; pv=l; }
      ok(powerHold && fl==3, "varakozas + mentett hiba: a LED 3 s alatt harmat villan");
      remoteOnReq=true; runTo(T+7000); ok(!rl1() && !holdBtnUsed, "tavoli BE a varakozas alatt: tiltva, nem fogyasztja el a gomb lehetoseget");
      press(); runTo(T+3000); ok(rl1() && sevFault==F_NONE && powerHold, "fizikai gomb a varakozas alatt: BE, nyugtazva");
      printf("67H: %s (%d hiba)\n", fails?"BUKOTT":"RENDBEN", fails); return fails; }
    if(argc>2 && argv[2][0]=='R'){   // "ujraindulas": a mentett hiba es a trigger tiltas megmarad
      nvsNum["sev"]=F_DC; nvsNum["sevCnt"]=1; nvsNum["trigOff"]=1; setup(); runTo(15000);
      ok(sevFault==F_DC && trigOff && !rl1(), "ujraindulas utan: mentett DC hiba, trigger KI, nem kapcsol be");
      remoteOnReq=true; runTo(T+7000); ok(!rl1(), "tavoli BE tiltva");
      press(); runTo(T+3000); ok(rl1() && sevFault==F_NONE && nvsNum["sev"]==F_NONE, "fizikai gomb: BE, a mentett hiba torolve");
      printf("67R: %s (%d hiba)\n", fails?"BUKOTT":"RENDBEN", fails); return fails; }
    setup();
#if NET_ENABLED
    netInit();
#endif
    runTo(8000); ok(rl1() && spk(), "bekapcsolva aktiv triggerre");
    pins[D8]=LOW; runTo(T+100); pins[D8]=HIGH;
    ok(!rl1() && sevFault==F_DC && nvsNum["sev"]==F_DC && trigOff && nvsNum["trigOff"]==1, "DC hiba: KI, mentve, trigger figyeles KI (mentve)");
    runTo(T+20000); setTrig(false); runTo(T+500); setTrig(true); runTo(T+8000); ok(!rl1(), "aktiv trigger, uj trigger el: nem kapcsol be");
    remoteOnReq=true; runTo(T+7000); ok(!rl1(), "tavoli (app / HA) BE: tiltva");
#if NET_ENABLED
    httpReq("/api/power",{{"state","on"}},ncfg.webPass); ok(lastCode==409, "weboldal / app BE: HTTP 409 (csak gombbal)");
    { char js[1700]; buildState(js,sizeof js,true); ok(strstr(js,"\"latched\":\"DC hiba")!=nullptr && strstr(js,"\"problem\":\"ON\""), "allapot: latched + problem"); }
#endif
    remoteTrigReq=1; runTo(T+200); ok(!trigOff && sevFault==F_DC, "trigger figyeles kezzel visszakapcsolhato, a mentett hiba marad");
    setTrig(false); runTo(T+500); setTrig(true); runTo(T+8000); ok(!rl1(), "visszakapcsolt trigger mellett sem kapcsol be gomb nelkul");
    press(); runTo(T+3000); ok(rl1() && spk() && sevFault==F_NONE, "fizikai gomb: BE, nyugtazva");
    setTrig(false); runTo(T+3000); ok(!rl1(), "nyugtazas utan a trigger ujra mukodik (KI)");
    setTrig(true); runTo(T+8000); ok(rl1(), "nyugtazas utan a trigger ujra mukodik (BE)");
    railFail[1]=true; runTo(T+200); railFail[1]=false;
    ok(!rl1() && sevFault==F_RAIL && trigOff && sevCount==2, "tap hiba: KI, mentve, trigger figyeles ujra KI");
    runTo(T+20000); setTrig(false); runTo(T+500); setTrig(true); runTo(T+8000); ok(!rl1(), "tap hiba utan sem kapcsol be triggerre");
    printf("67: %s (%d hiba)\n", fails?"BUKOTT":"RENDBEN", fails); return fails; }
  if(sc==66){   // erosito-tapfigyeles
    quiet=!getenv("LOUD"); rr=ESP_RST_EXT; pins[PIN_BTN]=HIGH; setTrig(true); setup(); int fails=0;
    auto ok=[&](bool c,const char*t){ printf("  %s %s\n", c?"OK  ":"HIBA", t); if(!c) fails++; };
    auto runTo=[&](unsigned long t){ while(T<t) loop(); };
    auto press=[&](){ setBtn(true); runTo(T+600); setBtn(false); };
    runTo(8000); ok(rl1() && rl2() && spk() && rails[0].ok && rails[1].ok, "bekapcsolas: mindket tap OK, hangszorok BE");
    railFail[0]=true; runTo(T+40); railFail[0]=false; runTo(T+500); ok(spk() && !faultActive, "40 ms-os taptuske (bal): nincs hiba");
    unsigned long t0=T, tOff=0; railFail[0]=true; while(T<t0+200){ loop(); if(!spk()&&!tOff) tOff=T; }
    ok(tOff && tOff-t0>=50 && tOff-t0<=52, "bal tap kiesik: hangszorok 50-52 ms alatt KI");
    ok(!rl1() && !rl2() && lastFault==F_RAIL && railFaultMask==1, "relek KI, hiba: bal erosito");
    railFail[0]=false; runTo(T+20000); ok(!rl1(), "tiltas utan aktiv triggerre sem kapcsol be magatol");
    press(); runTo(T+3000); ok(rl1() && spk(), "hosszu gombnyomas: ujra BE");
    press(); runTo(T+8000);
    railFail[1]=true; press(); runTo(T+2500); ok(rl1() && rl2() && !spk(), "jobb tap nincs bekapcsolaskor: RL2 igen, hangszorok varnak");
    runTo(T+1500); ok(!rl1() && lastFault==F_RAIL && railFaultMask==2, "1 s varakozas utan hiba: jobb erosito, minden KI");
    railFail[1]=false; runTo(T+12000);
    railFail[1]=true; press(); runTo(T+2400); railFail[1]=false; runTo(T+200); ok(rl1() && spk(), "a tap a varakozas alatt megjon: hangszorok BE");
    printf("66: %s (%d hiba)\n", fails?"BUKOTT":"RENDBEN", fails); return fails; }
  if(sc==65){   // hangszorovedo
    quiet=!getenv("LOUD"); rr=ESP_RST_EXT; pins[PIN_BTN]=HIGH; setTrig(true); setup(); int fails=0;
    auto ok=[&](bool c,const char*t){ printf("  %s %s\n", c?"OK  ":"HIBA", t); if(!c) fails++; };
    auto runTo=[&](unsigned long t){ while(T<t) loop(); };
    unsigned long tRl2=0, tSpk=0; while(T<20000){ loop(); if(rl2()&&!tRl2) tRl2=T; if(spk()&&!tSpk) tSpk=T; }
    ok(tRl2 && tSpk && tSpk-tRl2>=1000 && tSpk-tRl2<=1002, "hangszorok az RL2 utan 1 s-mal");
    pins[D8]=LOW; runTo(T+15); pins[D8]=HIGH; runTo(T+500); ok(spk() && rl1() && !faultActive, "15 ms-os DC-tuske: nincs hiba");
    unsigned long t0=T; pins[D8]=LOW; unsigned long tOff=0; while(T<t0+100){ loop(); if(!spk()&&!tOff) tOff=T; }
    ok(tOff && tOff-t0>=20 && tOff-t0<=22, "valodi DC: hangszorok 20-22 ms alatt KI");
    ok(!rl1() && !rl2(), "valodi DC: RL1 / RL2 is KI");
    pins[D8]=HIGH; runTo(T+30000); ok(!rl1(), "tiltas utan aktiv triggerre sem kapcsol be (csak gomb / tavoli)");
    setBtn(true); runTo(T+600); setBtn(false); runTo(T+3000); ok(rl1() && spk(), "hosszu gombnyomas: ujra BE, hangszorok is");
    setBtn(true); runTo(T+600); setBtn(false); runTo(T+8000);
    pins[D8]=LOW; setBtn(true); runTo(T+600); setBtn(false); runTo(T+2500); ok(rl1() && rl2() && !spk(), "bekapcsolaskor DC: RL2 igen, hangszorok varnak");
    pins[D8]=HIGH; runTo(T+50); ok(spk(), "a DC megszunt a varakozasban: hangszorok BE");
    setBtn(true); runTo(T+600); setBtn(false); runTo(T+8000);
    pins[D8]=LOW; setBtn(true); runTo(T+600); setBtn(false); unsigned long ts=T; runTo(T+6000);
    ok(!rl1() && !spk() && st.faults>=2, "tartos DC bekapcsolaskor: 2 s varakozas utan hiba, minden KI");
    pins[D8]=HIGH; runTo(T+12000); (void)ts;   // 2. hiba: a tiltas 10 s (duplazodik)
    setBtn(true); runTo(T+600); setBtn(false); runTo(T+3000); ok(spk(), "ismet BE");
    remoteTrigReq=1; runTo(T+100); ok(!trigOff, "trigger figyeles kezzel visszakapcsolva");
    unsigned long t1=T; setTrig(false); unsigned long tS=0, tR=0; while(T<t1+5000){ loop(); if(!spk()&&!tS) tS=T; if(!rl1()&&!tR) tR=T; }
    ok(tS && tR && tS<=tR, "kikapcsolas: hangszorok elsokent (vagy egyutt) valnak le");
    setTrig(true); runTo(T+8000); ok(spk(), "trigger ujra: BE");
    mains=false; unsigned long t2=T; tS=0; while(T<t2+100){ loop(); if(!spk()&&!tS) tS=T; } mains=true;
    ok(tS && tS-t2<=26, "halozatkieses: hangszorok 26 ms-on belul KI");
    printf("65: %s (%d hiba)\n", fails?"BUKOTT":"RENDBEN", fails); return fails; }
  if(sc==64){   // memoria-or. argv[2]: A=kritikus, B=veszhelyzet, C=ujraindulas utan, D=hurok-vedelem, E=rovid kilenges
    char m=argv[2][0]; quiet=!getenv("LOUD"); pins[PIN_BTN]=HIGH; int fails=0;
    auto ok=[&](bool c,const char*t){ printf("  %s %s\n", c?"OK  ":"HIBA", t); if(!c) fails++; };
    auto runTo=[&](unsigned long t)->bool{ try{ while(T<t) loop(); }catch(SimRestart&){ return true; } return false; };
#if NET_ENABLED
    nvsStr={{"ssid","otthon"},{"pass","jelszo123"},{"mqttUri",""},{"mqttUser",""},{"mqttPass",""},{"webPass","admin1234"}};
    scen=[]{ static unsigned long lp=0; if(T-lp>=100){lp=T; netPoll();} };
#endif
    if(m=='F'){ rtcMemInfo = 1 | (1<<8) | 0x20000; rtcMemCheck = MEMR_MAGIC ^ rtcMemInfo; rr=ESP_RST_SW; setTrig(true); setup();
      runTo(20000); ok(!rl1(), "KI-kapcsolt relenel tortent ujrainditas, aktiv trigger: KI marad");
      setTrig(false); runTo(21000); setTrig(true); runTo(28000); ok(rl1(), "a trigger kovetkezo aktiv ele bekapcsol");
      printf("64%c: %s\n", m, fails?"BUKOTT":"RENDBEN"); return fails; }
    if(m=='C'){ rtcMemInfo = 1 | (2<<8) | 0x10000 | 0x20000; rtcMemCheck = MEMR_MAGIC ^ rtcMemInfo; rr=ESP_RST_SW; setTrig(false); setup();
      bool r=runTo(3000); ok(!r && !rl1(), "ujraindulas utan 3 s: meg KI (minimalis kikapcsolt ido)");
      runTo(7000); ok(rl1(), "5 s utan visszakapcsolt (relek BE voltak)"); ok((memInfo&0x30000)==0 && (memInfo&0xFF)==1, "RTC: jelzobitek torolve, szamlalo 1");
      runTo(7000+3600000UL+2000); ok((memInfo&0xFF)==0, "1 ora zavartalan futas utan a szamlalo 0");
      printf("64%c: %s\n", m, fails?"BUKOTT":"RENDBEN"); return fails; }
    rr=ESP_RST_EXT; setTrig(true); if(m=='D'){ rtcMemInfo=3; rtcMemCheck=MEMR_MAGIC^3; } setup();
    if(!runTo(8000)) ok(rl1()&&rl2(), "bekapcsolva aktiv triggerre");
    if(m=='A'){
      heapFree=11000; bool r=runTo(8000+29000); ok(!r && rl1(), "kritikus 29 s: meg nincs teendo, relek BE");
      long d0=discCount; r=runTo(8000+33000);
#if NET_ENABLED
      ok(!r && rl1() && discCount>d0, "30 s utan: halozat ujraindul, relek BE maradnak");
      r=runTo(8000+33000+59000); ok(!r && rl1(), "halozat-ujrainditas utan 60 s varakozas");
      r=runTo(8000+33000+65000); ok(!r && rl1(), "nem javult, de a relek BE: nincs ujrainditas, var");
#else
      ok(!r || true, "(HA nelkul nincs halozat-lepes)");
      r=runTo(8000+40000); ok(!r && rl1(), "relek BE: nincs ujrainditas, var");
#endif
      setTrig(false); unsigned long t0=T; r=runTo(T+20000);
      ok(r, "trigger KI -> relek KI -> szabalyos ujrainditas");
      ok(!rl1() && !rl2(), "ujrainditaskor a relek KI");
      ok(T-t0 >= 30 && T-t0 < 6000, "ujrainditas a kikapcsolas utan rogton");
      ok((rtcMemInfo&0xFF)==1 && ((rtcMemInfo>>8)&0xFF)==1 && !(rtcMemInfo&0x10000) && (rtcMemInfo&0x20000), "RTC: 1. ujrainditas, ok: keves memoria, relek KI voltak");
    }
    if(m=='B'){
      heapBlock=1500; unsigned long t0=T; int p1=1,p2=1; bool r=false; unsigned long tr=0;
      try{ while(T<t0+10000){ loop(); p1=rl1(); p2=rl2(); } }catch(SimRestart&){ r=true; tr=T; }
      ok(r && tr-t0 >= 3000 && tr-t0 <= 4100, "veszhelyzet (blokk 1,5 kB): ujrainditas 3-4 s utan");
      ok(!rl1() && !rl2(), "ujrainditas elott a relek KI");
      ok((rtcMemInfo&0x10000) && ((rtcMemInfo>>8)&0xFF)==2, "RTC: relek BE voltak, ok: toredezett");
    }
    if(m=='D'){
      heapFree=5000; bool r=runTo(8000+120000); ok(!r, "3 egymas utani ujrainditas utan: nincs tobb (csak jelzes)"); ok(rl1(), "relek BE maradnak");
    }
    if(m=='E'){
      heapBlock=1500; bool r=runTo(8000+2500); heapBlock=90000; r|=runTo(8000+40000); ok(!r && rl1(), "2,5 s-os kilenges: nincs ujrainditas");
      heapFree=11000; r=runTo(8000+40000+25000); heapFree=150000; r|=runTo(8000+200000); ok(!r && rl1(), "25 s-os kritikus kilenges: nincs teendo");
      stackFree=200; setTrig(false); r=runTo(8000+200000+40000); ok(r && !rl1(), "verem 200 B (kritikus): relek KI utan ujrainditas (halozat-lepes nelkul)");
    }
    printf("64%c: %s\n", m, fails?"BUKOTT":"RENDBEN"); return fails; }
  if(sc==62){   // sorozatos kieses: szunet, gomb, LED, visszaallas; F_NOMAINS utan nincs auto
    quiet=!getenv("LOUD"); rr=ESP_RST_EXT; pins[PIN_BTN]=HIGH; pins[D3]=HIGH; setTrig(true); setup();
    int fails=0; auto ok=[&](bool c,const char*m){ printf("  %s %s\n", c?"OK  ":"HIBA", m); if(!c) fails++; };
    auto runTo=[&](unsigned long t){ while(T<t) loop(); };
    auto drop=[&](unsigned long at){ runTo(at); mains=false; runTo(at+300); mains=true; };
    runTo(7000); ok(rl1()&&rl2(), "indulas aktiv triggerrel");
    drop(10000); runTo(17000); ok(rl1(), "1. kieses utan visszakapcsolt");
    drop(20000); runTo(27000); ok(rl1(), "2. kieses utan visszakapcsolt");
    drop(30000); runTo(45000); ok(!rl1(), "3. kieses utan NINCS visszakapcsolas");
    ok(autoPaused(), "szunet aktiv");
    int fl=0, prev=0; for(unsigned long e=T+3000; T<e;){ loop(); int l=pins[D1]; if(l&&!prev) fl++; prev=l; }
    ok(fl==2, "LED: dupla felvillanas 3 s alatt");
    setTrig(false); runTo(T+500); setTrig(true); runTo(T+500); ok(!rl1(), "uj trigger el szunetben: nincs bekapcsolas");
    pins[PIN_BTN]=LOW; runTo(T+600); pins[PIN_BTN]=HIGH; runTo(T+2000); ok(rl1(), "hosszu gombnyomas szunetben bekapcsol");
    pins[PIN_BTN]=LOW; runTo(T+600); pins[PIN_BTN]=HIGH; runTo(T+3000); ok(!rl1(), "gombbal ki");
    runTo(30300+600000+100); ok(rl1() && !autoPaused(), "10 perc kieses nelkul: visszaall, aktiv triggerre bekapcsol");
    runTo(T+3000); mains=false; runTo(T+20000); ok(!rl1(), "4. kieses (uj sorozat 1.) -> ki, halozat nincs");
    runTo(T+10000); ok(!rl1(), "visszakapcsolas halozat nelkul -> F_NOMAINS, ki"); mains=true;
    runTo(T+700000); ok(!rl1(), "F_NOMAINS utan 10 perc mulva sincs automatikus bekapcsolas");
    setTrig(false); runTo(T+500); setTrig(true); runTo(T+2000); ok(rl1(), "uj trigger el bekapcsol");
    printf("62: %s (%d hiba)\n", fails?"BUKOTT":"RENDBEN", fails); return fails; }
  if(sc==61){   // tartos hiba 24 oran at: 1 = beragadt gomb, 2 = zajos trigger, 3 = 10 s-onkent 200 ms halozatkieses aktiv triggerrel
    int mode=atoi(argv[2]); quiet=!getenv("LOUD"); rr=ESP_RST_EXT; pins[PIN_BTN]=HIGH; pins[D3]=HIGH;
    if(mode==3) setTrig(true);
    hook=[]{ };
    static int md; md=mode;
    scen=[]{ if(md==1 && T>=60000) pins[PIN_BTN]=LOW;
             if(md==2 && T>=60000) pins[D3] = ((T/70)&1) ? LOW : HIGH;
             if(md==3) mains = (T%10000) >= 200; };
    setup();
    long n0 = nvsWrites + nvsBool + nvsUChar;
    { unsigned long hEnd = getenv("HRS") ? atol(getenv("HRS"))*60000UL : 24UL*3600000UL; while(T<hEnd) loop(); }
    long n = nvsWrites + nvsBool + nvsUChar - n0;
    printf("begin %ld; mod %d, 24 ora: NVS iras %ld (%.1f/ora), hibak %lu, kiesesek %lu, bekapcsolasok %lu, RL1 most %d\n", nvsBegins, mode, n, n/24.0,
      (unsigned long)st.faults, (unsigned long)st.mainsLoss, (unsigned long)st.onCount, rl1());
    return 0; }
  if(sc==7){ puts("== S7: hosszu NVS-szeru blokk bekapcsolva (60 ms) nem okozhat hamis kiesest");
    rr=ESP_RST_EXT; setTrig(true);
    scen=[]{ static bool d=false; if(T==15000 && !d){ d=true; T+=60; } };
    setup(); run(17000); }
}
