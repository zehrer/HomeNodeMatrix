# HomeNodeMatrix ⚡ MatrixPortal M4

**HomeNodeMatrix** ist eine smarte Energie- & Uhrzeitanzeige für das **Adafruit MatrixPortal M4** mit einem **64x64 RGB LED Matrix Display**.

Es liest Live-Leistungswerte deiner Photovoltaikanlage (**Fronius Wechselrichter**) sowie deines Netzanschlusses (**Shelly 3Pro / Pro 3EM**) aus und stellt diese zusammen mit Uhrzeit, Datum und Statusanzeigen übersichtlich dar.

---

## 🌟 Features

- **Uhrzeit & Datum**: Exakte NTP-Uhrzeitsynchronisation & Wochentagsberechnung.
- **Solar-Erzeugung**: Auslesen der aktuellen Solarleistung über die **Fronius Solar API v1** (`/solar_api/v1/GetPowerFlowRealtimeData.fcgi`).
- **Netzbezug & Einspeisung**: Auslesen der aktuellen Netzleistung über die **Shelly Gen2 HTTP API** (`/rpc/Shelly.GetStatus`).
- **WLAN Access Point & Captive Portal**:
  - Automatisches Setup-WLAN (`HomeNodeMatrix-Setup`) bei fehlendem WLAN.
  - Dynamischer **8-stelliger PIN** direkt auf dem 64x64 Display.
  - Captive Portal Weiterleitung an `http://192.168.4.1/`.
- **Dunkle Web-Konfigurationsseite**: Komfortables Ändern von WLAN, IP-Adressen, API-Pfaden, UTC-Offset und Helligkeit im Browser.
- **Serial CLI (Kommandozeile)**:
  - Vollwertige serielle Konsole über USB Serial (Web Serial Terminal tauglich).
  - Befehle: `status`, `debug on/off`, `wifi`, `shelly`, `inverter`, `brightness`, `save`, `reboot`.
  - Stummschaltbarer Debug-Modus (`debug off`), um den CLI-Prompt sauber zu halten.
- **Tastensteuerung am Board**:
  - **Taste UP**: Öffnet den **Status-Bildschirm** (Matrix IP, Shelly IP & Fronius IP in ultra-kompaktem 3x5 Pixel Font).
  - **Taste DOWN**: Aktiviert die **PixelDust Sand-Physik-Demo** (nutzt den integrierten LIS3DH Beschleunigungssensor für schwerkraftbasierte Partikelsimulation).

---

## 🛠 Hardware & Anforderungen

- **Board**: Adafruit MatrixPortal M4 (ATSAMD51 + ESP32 AirLift Co-Prozessor)
- **Display**: 64x64 RGB LED Matrix (5 Adress-Pins)
- **Netz-Messgerät**: Shelly 3Pro / Pro 3EM
- **Wechselrichter**: Fronius Symo / Primo / Gen24 (Solar API v1)

---

## 🚀 Installation & Kompilierung

Mit `arduino-cli`:

```bash
# Kompilieren
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4 .

# Upload
arduino-cli upload -p /dev/cu.usbmodem1101 --fqbn adafruit:samd:adafruit_matrixportal_m4 .
```

---

## 📜 Lizenz

MIT License
