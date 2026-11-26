// Nem kötelező: alapértékek a hálózathoz. Másold le secrets.h néven a sketch mappájába (a secrets.h nem kerül a gitbe).
// A weboldalon mentett beállítás ezeket felülírja; ha egyik sincs, a program AP módban indul (http://192.168.4.1).
#define WIFI_SSID "wifi_neve"
#define WIFI_PASS "wifi_jelszava"
#define MQTT_URI  "mqtt://192.168.1.10:1883"   // a Home Assistant / Mosquitto címe; üres: nincs MQTT
#define MQTT_USER "mqtt_felhasznalo"
#define MQTT_PASS "mqtt_jelszo"
#define AP_PASS   "erosito123"                 // AP jelszó és a weboldal alapjelszava (legalább 8 karakter)
