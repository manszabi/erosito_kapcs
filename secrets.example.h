// Nem kötelező: alapértékek a hálózathoz. Másold le secrets.h néven a sketch mappájába (a secrets.h nem kerül a gitbe).
// A weboldalon mentett beállítás ezeket felülírja; ha egyik sincs, a program AP módban indul (http://192.168.4.1).
#define WIFI_SSID "wifi_neve"
#define WIFI_PASS "wifi_jelszava"
#define MQTT_URI  "mqtt://192.168.1.10:1883"   // a Home Assistant / Mosquitto címe; üres: nincs MQTT
#define MQTT_USER "mqtt_felhasznalo"
#define MQTT_PASS "mqtt_jelszo"
#define AP_PASS   "erosito123"                 // AP jelszó és a weboldal alapjelszava (legalább 8 karakter)
// Fix IP (nem kötelező; ha nincs megadva vagy hibás: DHCP). A weboldalon / iOS-appban mentett érték felülírja.
// #define STATIC_IP   "192.168.1.50"
// #define STATIC_GW   "192.168.1.1"
// #define STATIC_MASK "255.255.255.0"   // üres: 255.255.255.0
// #define STATIC_DNS  ""                // üres: az átjáró
