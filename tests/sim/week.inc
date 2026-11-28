// ===== 68: tobb hetes, valosaghu hasznalat (szakaszokban: ESP-ujraindulaskor uj folyamat, NVS / RTC atadassal) =====
#include <fstream>
#include <sstream>
typedef unsigned long long u64;
const u64 MIN_=60000ULL, HOUR=3600000ULL, DAY=86400000ULL;
struct Week {
  std::mt19937_64 rng; u64 base=0, endW=0;
  int bootRR=ESP_RST_POWERON; uint32_t rHL=0, rHC=0, rMI=0, rMC=0;
  // felhasznaloi modell
  int sess=0, method=0; u64 nextSess=0, sessEnd=0, checkAt=0, offCheckAt=0, trigOffAt=0, ackAt=0, reenAt=0;
  bool trigWant=false, noLongOutage=false;
  // hibak, kornyezet
  bool dcBroken=false; int railBroken=0; u64 dcRepair=0, railRepair=0, nextDc=0, nextRail=0, nextGlitch=0, nextFlick=0, nextOut=0, nextCrash=0, nextChat=0;
  // szamlalok
  long refault=0; u64 lastAck=0;
  long sessions=0, btnPress=0, trigCycles=0, appCmds=0, flickers=0, outages=0, crashes=0, dcInj=0, railInj=0, latches=0, acks=0, ackFails=0, reenables=0, viol=0, ons=0, spkOns=0, glitches=0, chatters=0, fallback=0;
  double uu(){ return std::uniform_real_distribution<double>(0,1)(rng); }
  u64 rr_(u64 a, u64 b){ return std::uniform_int_distribution<u64>(a,b)(rng); }
} W;
struct Press { unsigned long at, dur; };
std::deque<Press> pressQ; unsigned long pressUntil=0, chatUntil=0, mainsUpAt=0, rl1OnT=0, glitchUntil=0; bool wasOn=false, prevSev=false;
int segEnd=0; u64 outLen=0;   // 1 = hosszu aramszunet, 2 = watchdog
const char* weekFile;

void wSave(){
  std::ofstream f(weekFile);
  std::ostringstream r; r<<W.rng; f<<"rng "<<r.str()<<"\n";
#define WS(k) f<<#k<<" "<<W.k<<"\n";
  WS(base) WS(endW) WS(bootRR) WS(rHL) WS(rHC) WS(rMI) WS(rMC) WS(sess) WS(method) WS(nextSess) WS(sessEnd) WS(checkAt) WS(offCheckAt) WS(trigOffAt) WS(ackAt) WS(reenAt)
  WS(trigWant) WS(noLongOutage) WS(dcBroken) WS(railBroken) WS(dcRepair) WS(railRepair) WS(nextDc) WS(nextRail) WS(nextGlitch) WS(nextFlick) WS(nextOut) WS(nextCrash) WS(nextChat)
  WS(sessions) WS(btnPress) WS(trigCycles) WS(appCmds) WS(flickers) WS(outages) WS(crashes) WS(dcInj) WS(railInj) WS(latches) WS(acks) WS(ackFails) WS(reenables) WS(viol) WS(ons) WS(spkOns) WS(glitches) WS(chatters) WS(fallback) WS(refault) WS(lastAck)
  for(auto&kv:nvsNum) f<<"N "<<kv.first<<" "<<kv.second<<"\n";
  for(auto&kv:nvsStr) f<<"S "<<kv.first<<" "<<kv.second<<"\n";
  f<<"B "<<blobLen; for(size_t i=0;i<blobLen;i++) f<<" "<<(int)blob[i]; f<<"\n";
}
bool wLoad(){
  std::ifstream f(weekFile); if(!f) return false; std::string line;
  while(std::getline(f,line)){ std::istringstream is(line); std::string k; is>>k;
    if(k=="rng"){ std::string rest; std::getline(is,rest); std::istringstream rs(rest); rs>>W.rng; continue; }
    if(k=="N"){ std::string n; uint32_t v; is>>n>>v; nvsNum[n]=v; continue; }
    if(k=="S"){ std::string n,v; is>>n; std::getline(is,v); if(!v.empty()&&v[0]==' ') v.erase(0,1); nvsStr[n]=v; continue; }
    if(k=="B"){ is>>blobLen; for(size_t i=0;i<blobLen;i++){ int b; is>>b; blob[i]=(unsigned char)b; } continue; }
#define WL(x) if(k==#x){ is>>W.x; continue; }
    WL(base) WL(endW) WL(bootRR) WL(rHL) WL(rHC) WL(rMI) WL(rMC) WL(sess) WL(method) WL(nextSess) WL(sessEnd) WL(checkAt) WL(offCheckAt) WL(trigOffAt) WL(ackAt) WL(reenAt)
    WL(trigWant) WL(noLongOutage) WL(dcBroken) WL(railBroken) WL(dcRepair) WL(railRepair) WL(nextDc) WL(nextRail) WL(nextGlitch) WL(nextFlick) WL(nextOut) WL(nextCrash) WL(nextChat)
    WL(sessions) WL(btnPress) WL(trigCycles) WL(appCmds) WL(flickers) WL(outages) WL(crashes) WL(dcInj) WL(railInj) WL(latches) WL(acks) WL(ackFails) WL(reenables) WL(viol) WL(ons) WL(spkOns) WL(glitches) WL(chatters) WL(fallback) WL(refault) WL(lastAck)
  }
  return true;
}
u64 Wt(){ return W.base + T; }
void press(unsigned long dur, unsigned long delayMs=0){ unsigned long at = std::max((unsigned long)T + delayMs, pressQ.empty() ? (unsigned long)T + delayMs : pressQ.back().at + pressQ.back().dur + 250); pressQ.push_back({at,dur}); }
void longPress(){ press(700); W.btnPress++; }
void weekHook(){
  Stress &x = X;
  u64 w = Wt();
  // ---- gomb (sorban allo nyomasok, 0-10 ms pergessel) ----
  if(!pressQ.empty() && T>=pressQ.front().at && pressUntil<=T){ pressUntil=T+pressQ.front().dur; x.lastBtnDownT=T; if(powerHold && pressQ.front().dur>=400) x.holdManual=true; pressQ.pop_front(); }
  bool bdown = T < pressUntil;
  pins[PIN_BTN] = bdown ? LOW : HIGH;
  // ---- trigger (idonkent zajos lokessel) ----
  bool tv = W.trigWant; if(T<chatUntil) tv = (T/60)&1;
  pins[D3] = tv ? LOW : HIGH;
  // ---- halozat ----
  mains = T >= mainsUpAt;
  // ---- hibas vegfok / tap ----
  railFail[0] = W.railBroken&1; railFail[1] = (W.railBroken&2)!=0;
  // ---- ellenorzes (a DC-jel frissitese elott: a hiba pillanataban meg a regi szint latszik) ----
  checkInv(x);
  if(rl1() && !wasOn) rl1OnT=T; wasOn=rl1();
  bool dc = (W.dcBroken && rl1() && T-rl1OnT>=300) || T<glitchUntil;
  pins[D8] = dc ? LOW : HIGH;
  if(sevFault && !prevSev){ W.latches++; if(W.lastAck && w - W.lastAck < 5*MIN_) W.refault++; W.ackAt = w + W.rr_(1*HOUR, 6*HOUR); }
  prevSev = sevFault;
  if(T % 100) return;
  // ---- felhasznaloi modell (100 ms-onkent) ----
  if(segEnd) return;
  // javitas
  if(W.dcBroken && w>=W.dcRepair) W.dcBroken=false;
  if(W.railBroken && w>=W.railRepair) W.railBroken=0;
  // hiba befecskendezes (bekapcsolt, hangszorok mukodnek)
  if(w>=W.nextDc){ if(spk()){ W.dcBroken=true; W.dcRepair=w+W.rr_(DAY,3*DAY); W.dcInj++; W.nextDc=w+W.rr_(7*DAY,14*DAY); } else W.nextDc=w+HOUR; }
  if(w>=W.nextRail){ if(spk()){ W.railBroken=1+(int)W.rr_(0,2); W.railRepair=w+W.rr_(DAY,3*DAY); W.railInj++; W.nextRail=w+W.rr_(7*DAY,14*DAY); } else W.nextRail=w+HOUR; }
  if(w>=W.nextGlitch){ if(rl1()){ glitchUntil=T+W.rr_(2,15); W.glitches++; } W.nextGlitch=w+W.rr_(2*HOUR,6*HOUR); }
  if(w>=W.nextChat){ chatUntil=T+W.rr_(1000,2000); W.chatters++; W.nextChat=w+W.rr_(5*DAY,10*DAY); }
  if(w>=W.nextFlick){ mainsUpAt=T+W.rr_(30,400); W.flickers++; W.nextFlick=w+W.rr_(DAY,4*DAY); }
  if(!W.noLongOutage && w>=W.nextOut){ mainsUpAt=T+100000; segEnd=1; outLen=W.rr_(10*MIN_,4*HOUR); W.outages++; W.nextOut=w+W.rr_(4*DAY,10*DAY); return; }
  if(!W.noLongOutage && w>=W.nextCrash){ segEnd=2; W.crashes++; W.nextCrash=w+W.rr_(10*DAY,20*DAY); return; }
  // mentett hiba: a felhasznalo nehany ora mulva gombbal nyugtaz (ha meg hibas, ujra lekapcsol)
  if(sevFault && W.ackAt && w>=W.ackAt && !rl1() && !lockoutPending && pressQ.empty()){
    longPress(); W.ackAt = w + W.rr_(3*HOUR, 8*HOUR);
    W.sess=1; W.method=1; W.sessEnd=w+W.rr_(10*MIN_, 60*MIN_); W.checkAt=0; W.sessions++;
  }
  if(!sevFault && W.ackAt && rl1()){ W.ackAt=0; W.acks++; W.lastAck=w; if(trigOff) W.reenAt = w + W.rr_(30*MIN_, 3*HOUR); }
  if(W.reenAt && w>=W.reenAt){ W.reenAt=0; if(trigOff && !sevFault){ W.reenables++; if(W.uu()<0.5){ remoteTrigReq=1; W.appCmds++; } else { press(150); press(150); W.btnPress+=2; } } }
  // munkamenet
  if(!W.sess && w>=W.nextSess){
    W.sess=1; W.sessions++; W.sessEnd=w+W.rr_(30*MIN_,240*MIN_); double k=W.uu();
    W.method = k<0.6 ? 0 : k<0.9 ? 1 : 2;
    if(W.method==0){ W.trigWant=true; W.trigCycles++; W.checkAt=w+W.rr_(60000,300000); }
    else if(W.method==1){ if(!rl1()) longPress(); W.checkAt=w+60000; }
    else { remoteOnReq=true; if(powerHold) x.holdManual=true; W.appCmds++; W.checkAt=w+30000; }
  }
  if(W.sess && W.checkAt && w>=W.checkAt){ W.checkAt=0; if(!rl1() && !faultActive && pressQ.empty()){ longPress(); W.fallback++; } }
  if(W.sess && w>=W.sessEnd){
    W.sess=0; W.nextSess=w+W.rr_(2*HOUR,20*HOUR);
    if(W.method==0){ if(W.uu()<0.8) W.trigWant=false; else { if(rl1()) longPress(); W.trigOffAt=w+W.rr_(MIN_,10*MIN_); } }
    else if(W.method==1){ if(rl1()) longPress(); }
    else { remoteOffReq=true; W.appCmds++; }
    W.offCheckAt=w+90000;
  }
  if(W.trigOffAt && w>=W.trigOffAt){ W.trigOffAt=0; W.trigWant=false; }
  if(W.offCheckAt && w>=W.offCheckAt){ W.offCheckAt=0; if(!W.sess && rl1() && pressQ.empty()){ longPress(); W.fallback++; } }
}
int weekMain(int argc, char**argv){
  quiet=!getenv("LOUD"); weekFile = argv[4];
  if(!wLoad()){ W.rng.seed(atoll(argv[2])); W.base=0; W.endW=(u64)(atof(argv[3])*DAY); W.noLongOutage = argc>5 && argv[5][0]=='N';
    W.nextSess=W.rr_(MIN_,2*HOUR); W.nextDc=W.rr_(3*DAY,8*DAY); W.nextRail=W.rr_(3*DAY,8*DAY); W.nextGlitch=W.rr_(HOUR,4*HOUR);
    W.nextFlick=W.rr_(HOUR,2*DAY); W.nextOut=W.rr_(DAY,5*DAY); W.nextCrash=W.rr_(5*DAY,12*DAY); W.nextChat=W.rr_(2*DAY,6*DAY); W.bootRR=ESP_RST_POWERON; }
  rr=W.bootRR;
  if(rr!=ESP_RST_POWERON){ rtcHoldLeft=W.rHL; rtcHoldCheck=W.rHC; rtcMemInfo=W.rMI; rtcMemCheck=W.rMC; }
  X.armed=true; X.bootReason=rr; X.haveRl1Off=false;
  pins[PIN_BTN]=HIGH; pins[D3]= W.trigWant?LOW:HIGH; mains=true;
  hook=weekHook;
  setup();
  long ons0=X.ons, spk0=X.spkOns;
  try{ while(Wt() < W.endW){ loop(); if(segEnd==1 && T>=mainsUpAt-100000+450) break; if(segEnd==2) break; } }catch(SimRestart&){ segEnd=3; }
  W.viol += X.viol; W.ons += X.ons-ons0; W.spkOns += X.spkOns-spk0;
  if(segEnd){
    W.rHL=rtcHoldLeft; W.rHC=rtcHoldCheck; W.rMI=rtcMemInfo; W.rMC=rtcMemCheck;
    if(segEnd==1){ W.base += T + outLen; W.bootRR=ESP_RST_POWERON; W.trigWant=false; W.sess=0; W.checkAt=W.offCheckAt=W.trigOffAt=0; W.nextSess=W.base+W.rr_(10*MIN_,3*HOUR); }
    else if(segEnd==2){ W.base += T + 1000; W.bootRR=ESP_RST_TASK_WDT; }
    else { W.base += T + 500; W.bootRR=ESP_RST_SW; }
    wSave(); return 42; }
  wSave();
  printf("%.0f nap: sertes %ld | munkamenet %ld, bekapcs %ld, hangszoro BE %ld, gombnyomas %ld, trigger ciklus %ld, app parancs %ld, fallback gomb %ld\n",
    W.endW/(double)DAY, W.viol, W.sessions, W.ons, W.spkOns, W.btnPress, W.trigCycles, W.appCmds, W.fallback);
  printf("  aramszunet: rovid %ld, hosszu (ESP ujraindul) %ld, watchdog-ujraindulas %ld | befecskendezett hiba: DC %ld, tap %ld (1-3 napig fennall)\n",
    W.flickers, W.outages, W.crashes, W.dcInj, W.railInj);
  printf("  mentett DC / tap lekapcsolas %ld, gombos nyugtazas %ld (ebbol meg hibas volt, ujra lekapcsolt: %ld), trigger kezzel vissza %ld | DC tuske (nem hiba) %ld, trigger zajlokes %ld\n",
    W.latches, W.acks, W.refault, W.reenables, W.glitches, W.chatters);
  printf("  utolso szakasz: ESP futasido %.1f nap%s\n", T/(double)DAY, T > 0xFFFFFFFFULL ? " (a millis() 49,7 napnal tulcsordult)" : "");
  printf("  vegallapot: on=%d, mentett hiba=%d, trigger figyeles=%s, NVS stat: bekapcs %lu, hibak %lu, kiesesek %lu, aramszunet utani indulas %lu\n",
    (int)on, (int)sevFault, trigOff?"KI":"BE", (unsigned long)st.onCount, (unsigned long)st.faults, (unsigned long)st.mainsLoss, (unsigned long)st.powerUps);
  return W.viol?1:0;
}
