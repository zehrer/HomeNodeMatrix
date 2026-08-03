#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Speicherstruktur im Flash
struct ConfigData {
  uint32_t magic;
  char wifi_ssid[32];
  char wifi_pass[64];
  char shelly_ip[32];
  char shelly_path[64];
  char inverter_ip[32];
  char inverter_path[64];
  int utc_offset_sec;
  uint8_t brightness;
};

// Magische Zahl für EEPROM Validierung
const uint32_t CONFIG_MAGIC = 0x484E4D32; // "HNM2"

// Standard-Konfiguration
const ConfigData defaultConfig = {
  CONFIG_MAGIC,
  "",                             // wifi_ssid (leer -> löst AP Setup aus)
  "",                             // wifi_pass
  "192.168.178.154",             // shelly_ip (Shelly 3Pro)
  "/rpc/Shelly.GetStatus",        // shelly_path (Gen2 HTTP API)
  "192.168.178.24",              // inverter_ip (Fronius Wechselrichter)
  "/solar_api/v1/GetPowerFlowRealtimeData.fcgi", // inverter_path (Fronius Solar API v1)
  7200,                           // utc_offset_sec (MESZ = UTC + 2 Stunden = 7200 Sek)
  200                             // brightness (0 - 255)
};

// Access Point Einstellungen
#define AP_SSID "HomeNodeMatrix-Setup"

// NTP Server
#define NTP_SERVER "pool.ntp.org"

// Update-Intervall für Leistungswerte (in ms)
#define POWER_UPDATE_MS 3000

#endif // CONFIG_H
