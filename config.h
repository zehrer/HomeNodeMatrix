#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Firmware Version & Build Metadata
#define FIRMWARE_VERSION "v0.1.0"
#define BUILD_NUMBER     108
#define BUILD_TIMESTAMP  __DATE__ " " __TIME__

// Flash Storage Structure for SAMD51 EEPROM
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
  uint8_t lang; // 0 = DE (German), 1 = EN (English)
};

// Magic number for EEPROM validation
const uint32_t CONFIG_MAGIC = 0x484E4D33; // "HNM3"

// Default Configuration Settings
const ConfigData defaultConfig = {
  CONFIG_MAGIC,
  "",                             // wifi_ssid (empty triggers AP setup)
  "",                             // wifi_pass
  "192.168.178.154",             // shelly_ip (Shelly 3Pro)
  "/rpc/Shelly.GetStatus",        // shelly_path (Gen2 HTTP API)
  "192.168.178.24",              // inverter_ip (Fronius Solar Inverter)
  "/solar_api/v1/GetPowerFlowRealtimeData.fcgi", // inverter_path (Fronius Solar API v1)
  7200,                           // utc_offset_sec (CEST = UTC + 2 hours = 7200 sec)
  200,                            // brightness (0 - 255)
  0                               // lang: 0 = DE (German), 1 = EN (English)
};

// Access Point Settings
#define AP_SSID "HomeNodeMatrix-Setup"

// NTP Server
#define NTP_SERVER "pool.ntp.org"

// Power Data Update Interval (in ms)
#define POWER_UPDATE_MS 3000

#endif // CONFIG_H
