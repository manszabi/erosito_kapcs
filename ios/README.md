# Erősítő – iOS vezérlő

Egyszerű SwiftUI-app: megkeresi az erősítő-bekapcsolót a helyi hálózaton (Bonjour), be- és kikapcsolja az erősítőt, ki- és bekapcsolja a trigger figyelést, és mutatja az állapotot. Ugyanazt a felületet használja, mint az ESP weboldala (`/api/status`, `/api/power`, `/api/trigger`), ugyanazzal a jelszóval.

- Minimum: iOS 17 (iOS 27-en és iPhone 17 Pro-n is fut), Xcode 16 vagy újabb, Swift 6.
- Firmware: a keresés az ESP mDNS-hirdetésén alapul (`_erosito._tcp`, `erosito.local`); ehhez a legfrissebb firmware kell. Kézzel IP-cím is megadható.

## Telepítés Windowsról (GitHub Actions + Sideloadly)

Mac nélkül: a GitHub ingyenes macOS-gépe lefordítja az appot, a Windows-os Sideloadly pedig a saját Apple ID-ddel aláírja és feltelepíti.

1. **Fordítás:** a repóban a `.github/workflows/ios.yml` minden `ios/` módosításkor lefut; kézzel: GitHub → Actions → „iOS app (alairatlan IPA)” → Run workflow.
2. **Letöltés:** a lefutott munkafolyamat oldalán, az Artifacts résznél: `Erosito-ipa` (zip, benne az `Erosito.ipa`). 30 napig tölthető le.
3. **Windows előkészítés:**
   - telepítsd az Apple **iTunes** és **iCloud** programot az apple.com-ról (nem a Microsoft Store-os változatot) – a Sideloadly ezeken keresztül éri el az iPhone-t;
   - telepítsd a **Sideloadly**-t (sideloadly.io).
4. **Telepítés:** iPhone kábellel a gépre („Megbízol ebben a számítógépben?” → Igen). Sideloadly: húzd be az `Erosito.ipa`-t, add meg az Apple ID-det, Start.
5. **iPhone-on:**
   - Beállítások → Adatvédelem és biztonság → **Fejlesztői mód** be (újraindítás után megerősítés);
   - Beállítások → Általános → VPN és eszközkezelés → a saját Apple ID-d → **Megbízás**.
6. **Első indítás:** helyi hálózat engedélyezése, majd a fogaskeréknél az eszköz kiválasztása és a jelszó.

Ingyenes Apple ID-vel a telepítés 7 napig érvényes (és egyszerre legfeljebb 3 ilyen app lehet); utána a Sideloadly-vel újra kell telepíteni – a Sideloadly ezt WiFi-n automatikusan is meg tudja csinálni, ha a gépe fut. A Sideloadly nem az Apple programja; az Apple ID-det az aláíráshoz kéri.

## Telepítés (Xcode, Mac kell)

1. **Új projekt:** Xcode → File → New → Project → iOS → **App**.
   - Product Name: `Erosito`, Interface: **SwiftUI**, Language: **Swift**, Storage: None.
2. **Fájlok:** töröld a generált `ContentView.swift`-et, `ErositoApp.swift`-et és `Assets.xcassets`-et, majd húzd be az `Erosito/` mappa öt `.swift` fájlját és az `Assets.xcassets` mappát (ikon), „Copy items if needed” bejelölve.
   - Vagy egyszerűbben: `brew install xcodegen`, majd az `ios/` mappában `xcodegen generate`, és nyisd meg az `Erosito.xcodeproj`-ot (a 3–4. lépés ekkor kimarad).
3. **Minimum verzió:** a target → General → Minimum Deployments: **iOS 17.0**.
4. **Engedélyek** (target → Info → Custom iOS Target Properties, `+`):
   | Kulcs | Típus | Érték |
   |---|---|---|
   | Privacy - Local Network Usage Description | String | Az erősítő-bekapcsoló megkereséséhez és vezérléséhez a helyi hálózaton. |
   | Bonjour services | Array | Item 0: `_erosito._tcp` |
   | App Transport Security Settings → Allow Local Networking | Boolean | YES |
5. **Aláírás:** target → Signing & Capabilities → Team: a saját Apple ID-d (ingyenes fiókkal is megy).
6. **Telefon:** csatlakoztasd kábellel, az iPhone-on kapcsold be a fejlesztői módot (Beállítások → Adatvédelem és biztonság → Fejlesztői mód), majd Xcode-ban Run (▶).
7. **Első indítás:** engedélyezd a helyi hálózat elérését, majd a fogaskerék ikonnál válaszd ki a talált eszközt, és add meg a weboldal jelszavát.

Ingyenes Apple ID-vel a telepített app 7 napig érvényes, utána Xcode-ból újra kell telepíteni (fizetős fejlesztői fiókkal 1 év).

Ha a nyers `Info.plist`-et szerkeszted, ugyanez XML-ben:

```xml
<key>NSLocalNetworkUsageDescription</key>
<string>Az erősítő-bekapcsoló megkereséséhez és vezérléséhez a helyi hálózaton.</string>
<key>NSBonjourServices</key>
<array>
    <string>_erosito._tcp</string>
</array>
<key>NSAppTransportSecurity</key>
<dict>
    <key>NSAllowsLocalNetworking</key>
    <true/>
</dict>
```

## Használat

- **Be / Ki:** ugyanazok a szabályok, mint a gombnál: 5 s minimális kikapcsolt idő, az RL2 2 s-os minimuma, hiba utáni tiltás; az áramszünet utáni 2 perces várakozás alatt egyszer kapcsolható be.
- **Trigger figyelés:** be- és kikapcsolás, az ESP elmenti.
- **Állapot:** 2 s-onként frissül, amíg az app előtérben van; lehúzással azonnal frissíthető.
- **Nem találja?** A telefon és az erősítő ugyanazon a WiFi-hálózaton legyen (vendéghálózat vagy kliens-elszigetelés esetén a keresés nem működik). Kézzel megadható az IP-cím (a soros `s` parancs kiírja) vagy az `erosito.local` név.
- A kapcsolat sima HTTP, Basic jelszóval: csak a helyi hálózaton használd.
