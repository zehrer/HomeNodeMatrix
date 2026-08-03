/* ----------------------------------------------------------------------
  HomeNodeMatrix - MatrixPortal M4 (64x64 LED Matrix)
  Smart Energy & Time Display mit Web-Konfiguration & Serial CLI
  - Korrigierter Wochentags-Array Index (0=So, 1=Mo, ..., 4=Do) für exakte NTP-Tage
  - Überschriften: SOLAR & NETZ (kein Bezug / Einspeisung Text mehr)
  - Kompakter 3x5 Pixel-Font für 15-stellige IP-Adressen (z.B. 192.168.178.154)
  - Helligkeitssteuerung via Farbskalierung (updateColors) via Web, CLI & Flash
  - Taste UP   -> Status-Bildschirm (Matrix IP, Shelly IP & Fronius IP)
  - Taste DOWN -> PixelDust Sand-Physik-Demo (LIS3DH Accelerometer)
  ---------------------------------------------------------------------- */

#include <Wire.h>
#include <SPI.h>
#include <WiFiNINA.h>             // Wi-Fi Treiber für ESP32 AirLift
#include <WiFiUdp.h>
#include <FlashStorage_SAMD.h>    // Flash / EEPROM Speicher für SAMD51
#include <Adafruit_GFX.h>         // Grafikbibliothek
#include <Adafruit_Protomatter.h> // Matrix Display Treiber
#include <Adafruit_LIS3DH.h>      // Beschleunigungssensor
#include <Adafruit_PixelDust.h>   // PixelDust Sand-Simulation
#include <ArduinoJson.h>          // JSON Parser (v7)
#include "config.h"

// ----------------------------------------------------------------------
// Hardware Pins (Adafruit MatrixPortal M4)
// ----------------------------------------------------------------------
uint8_t rgbPins[]  = {7, 8, 9, 10, 11, 12};
uint8_t addrPins[] = {17, 18, 19, 20, 21}; // 5 Address Pins für 64x64
uint8_t clockPin   = 14;
uint8_t latchPin   = 15;
uint8_t oePin      = 16;

#define BUTTON_UP_PIN   2 // Taste UP auf MatrixPortal M4
#define BUTTON_DOWN_PIN 3 // Taste DOWN auf MatrixPortal M4

#if defined(ADAFRUIT_MATRIXPORTAL_M4) || defined(_VARIANT_MATRIXPORTAL_M4_)
  // Board-Definitionen von Adafruit SAMD Core
#else
  #define SPIWIFI       SPI
  #define SPIWIFI_SS    33
  #define ESP32_RESETN  30
  #define SPIWIFI_ACK   31
  #define ESP32_GPIO0   -1
#endif

// Protomatter Matrix Instanz (64x64, 4 Bit Farbtiefe)
Adafruit_Protomatter matrix(
  64, 4, 1, rgbPins, 5, addrPins,
  clockPin, latchPin, oePin, true);

// LIS3DH Beschleunigungssensor & PixelDust Sand Simulation
Adafruit_LIS3DH accel = Adafruit_LIS3DH();
bool accelOk = false;

#define DUST_COLORS 8
#define DUST_BOX_HEIGHT 8
#define DUST_GRAINS (DUST_BOX_HEIGHT * DUST_COLORS * 8)
uint16_t dustColors[DUST_COLORS];
Adafruit_PixelDust sand(64, 64, DUST_GRAINS, 1, 128, false);

enum DisplayMode {
  MODE_NORMAL,
  MODE_STATUS,
  MODE_PIXELDUST
};

DisplayMode currentMode = MODE_NORMAL;
bool pixelDustInitialized = false;
unsigned long prevDustTime = 0;

bool lastUpState = HIGH;
bool lastDownState = HIGH;
unsigned long lastButtonCheck = 0;

// Farndefinitionen
uint16_t COLOR_BLACK;
uint16_t COLOR_WHITE;
uint16_t COLOR_GRAY;
uint16_t COLOR_YELLOW;
uint16_t COLOR_GREEN;
uint16_t COLOR_RED;
uint16_t COLOR_CYAN;
uint16_t COLOR_BLUE;
uint16_t COLOR_ORANGE;

// Globaler Speicher für Konfiguration
ConfigData config;

// Status- & Server-Variablen
WiFiServer webServer(80);
WiFiUDP ntpUdp;
WiFiUDP dnsUdp;
bool isAPMode = false;
bool debugMode = false; // Debug-Ausgaben standardmäßig aus (CLI bleibt sauber)
char ap_password[9] = "84729103"; // 8-stellige dynamische PIN
IPAddress apIPAddress(192, 168, 4, 1);
int apStatus = WL_IDLE_STATUS;

unsigned long lastNTPUpdate = 0;
unsigned long lastShellyAttempt = 0;
unsigned long lastInverterAttempt = 0;
unsigned long lastDisplayRedraw = 0;

const unsigned long POLL_ONLINE_MS    = 3000;  // 3 Sek bei aktiver Verbindung
const unsigned long RETRY_OFFLINE_MS = 30000; // 30 Sek Warten bei Verbindungsfehler

// Zeitvariablen
unsigned long localUnixTime = 0;
unsigned long lastTimeSyncMs = 0;

// Leistungswerte
float solarPowerW = 0.0;
float gridPowerW  = 0.0; // Negativ = Einspeisung, Positiv = Bezug
bool solarOk = false;
bool gridOk  = false;

// Serial CLI Puffer
String serialBuffer = "";

// Helper für bedingte Debug-Ausgaben
void logDebug(const String& msg) {
  if (debugMode) {
    Serial.println(msg);
  }
}

// ----------------------------------------------------------------------
// Farbskalierung nach Helligkeit (0-255)
// ----------------------------------------------------------------------
void updateColors() {
  uint32_t b = config.brightness; // 10 .. 255
  COLOR_BLACK  = matrix.color565(0, 0, 0);
  COLOR_WHITE  = matrix.color565((255 * b) / 255, (255 * b) / 255, (255 * b) / 255);
  COLOR_GRAY   = matrix.color565((50  * b) / 255, (50  * b) / 255, (50  * b) / 255);
  COLOR_YELLOW = matrix.color565((255 * b) / 255, (220 * b) / 255, 0);
  COLOR_GREEN  = matrix.color565(0, (255 * b) / 255, (100 * b) / 255);
  COLOR_RED    = matrix.color565((255 * b) / 255, (40  * b) / 255, (40  * b) / 255);
  COLOR_CYAN   = matrix.color565(0, (220 * b) / 255, (255 * b) / 255);
  COLOR_BLUE   = matrix.color565(0, (100 * b) / 255, (255 * b) / 255);
  COLOR_ORANGE = matrix.color565((255 * b) / 255, (120 * b) / 255, 0);
}

void applyBrightness(uint8_t b) {
  config.brightness = constrain(b, 10, 255);
  updateColors();
}

// ----------------------------------------------------------------------
// Ultra-Kompakter 3x5 Pixel Font für 15-stellige IP-Adressen (Ziffern 0-9 & '.')
// ----------------------------------------------------------------------
const uint8_t font3x5[][5] PROGMEM = {
  {0b111, 0b101, 0b101, 0b101, 0b111}, // 0
  {0b010, 0b110, 0b010, 0b010, 0b111}, // 1
  {0b111, 0b001, 0b111, 0b100, 0b111}, // 2
  {0b111, 0b001, 0b111, 0b001, 0b111}, // 3
  {0b101, 0b101, 0b111, 0b001, 0b001}, // 4
  {0b111, 0b100, 0b111, 0b001, 0b111}, // 5
  {0b111, 0b100, 0b111, 0b101, 0b111}, // 6
  {0b111, 0b001, 0b010, 0b100, 0b100}, // 7
  {0b111, 0b101, 0b111, 0b101, 0b111}, // 8
  {0b111, 0b101, 0b111, 0b001, 0b111}, // 9
  {0b000, 0b000, 0b000, 0b000, 0b010}  // .
};

void drawTinyChar(int16_t x, int16_t y, char c, uint16_t color) {
  int idx = -1;
  if (c >= '0' && c <= '9') idx = c - '0';
  else if (c == '.') idx = 10;
  if (idx < 0) return;

  for (int row = 0; row < 5; row++) {
    uint8_t b = pgm_read_byte(&font3x5[idx][row]);
    if (b & 0b100) matrix.drawPixel(x, y + row, color);
    if (b & 0b010) matrix.drawPixel(x + 1, y + row, color);
    if (b & 0b001) matrix.drawPixel(x + 2, y + row, color);
  }
}

void drawTinyString(int16_t x, int16_t y, const String& str, uint16_t color) {
  int16_t curX = x;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == '.') {
      drawTinyChar(curX, y, c, color);
      curX += 3; // Punkt ist 2px + 1px Abstand
    } else {
      drawTinyChar(curX, y, c, color);
      curX += 4; // Ziffer ist 3px + 1px Abstand
    }
  }
}

// ----------------------------------------------------------------------
// Wi-Fi Pixel Icon Zeichner (8x7 Pixel)
// ----------------------------------------------------------------------
void drawWifiIcon(int16_t x, int16_t y, uint16_t color) {
  matrix.fillRect(x + 3, y + 6, 2, 2, color);  // Punkt (unten)
  matrix.drawPixel(x + 1, y + 4, color);      // Mittlerer Bogen
  matrix.drawFastHLine(x + 2, y + 3, 4, color);
  matrix.drawPixel(x + 6, y + 4, color);
  matrix.drawPixel(x + 0, y + 1, color);      // Äußerer Bogen
  matrix.drawFastHLine(x + 1, y + 0, 6, color);
  matrix.drawPixel(x + 7, y + 1, color);
}

// Datumsberechnung aus Unix Epoch
void epochToDate(unsigned long epoch, int &year, int &month, int &day, int &wday) {
  unsigned long days = epoch / 86400L;
  wday = (days + 4) % 7; // 0=So, 1=Mo, 2=Di, 3=Mi, 4=Do, 5=Fr, 6=Sa

  long d = days - 10957; // Tage seit 2000-01-01
  if (d < 0) d = 0;

  long y4 = d / 1461;
  long rem = d % 1461;
  long y1;
  long dayInYear;

  if (rem < 366) {
    y1 = 0;
    dayInYear = rem;
  } else {
    y1 = 1 + (rem - 366) / 365;
    dayInYear = (rem - 366) % 365;
  }

  year = 2000 + (y4 * 4) + y1;
  bool isLeap = (y1 == 0);

  const int daysInMonth[] = {31, isLeap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  month = 1;
  while (month <= 12 && dayInYear >= daysInMonth[month - 1]) {
    dayInYear -= daysInMonth[month - 1];
    month++;
  }
  day = dayInYear + 1;
}

// ----------------------------------------------------------------------
// PixelDust Sand Simulation Initialisierung & Frame Rendering
// ----------------------------------------------------------------------
void initPixelDust() {
  if (pixelDustInitialized) return;
  if (!sand.begin()) {
    Serial.println(F("[PixelDust] Fehler beim Starten von Sand!"));
    return;
  }

  int n = 0;
  for(int i=0; i<DUST_COLORS; i++) {
    int xx = i * 64 / DUST_COLORS;
    int yy = 64 - DUST_BOX_HEIGHT;
    for(int y=0; y<DUST_BOX_HEIGHT; y++) {
      for(int x=0; x < 64 / DUST_COLORS; x++) {
        sand.setPosition(n++, xx + x, yy + y);
      }
    }
  }

  dustColors[0] = matrix.color565(64, 64, 64);   // Dark Gray
  dustColors[1] = matrix.color565(120, 79, 23);  // Brown
  dustColors[2] = matrix.color565(228, 3, 3);    // Red
  dustColors[3] = matrix.color565(255, 140, 0);  // Orange
  dustColors[4] = matrix.color565(255, 237, 0);  // Yellow
  dustColors[5] = matrix.color565(0, 128, 38);   // Green
  dustColors[6] = matrix.color565(0, 77, 255);   // Blue
  dustColors[7] = matrix.color565(117, 7, 135);  // Purple

  pixelDustInitialized = true;
  Serial.println(F("[PixelDust] Simulation erfolgreich initialisiert!"));
}

void runPixelDustFrame() {
  uint32_t t;
  while(((t = micros()) - prevDustTime) < (1000000L / 45));
  prevDustTime = t;

  double xx = 0, yy = 0, zz = 0;
  if (accelOk) {
    sensors_event_t event;
    accel.getEvent(&event);
    xx = event.acceleration.x * 1000;
    yy = event.acceleration.y * 1000;
    zz = event.acceleration.z * 1000;
  } else {
    xx = sin(millis() / 500.0) * 3000;
    yy = cos(millis() / 500.0) * 3000;
  }

  sand.iterate(xx, yy, zz);

  dimension_t x, y;
  matrix.fillScreen(COLOR_BLACK);
  for(int i=0; i<DUST_GRAINS; i++) {
    sand.getPosition(i, &x, &y);
    int n = i / ((64 / DUST_COLORS) * DUST_BOX_HEIGHT);
    matrix.drawPixel(x, y, dustColors[n]);
  }
  matrix.show();
}

// Tastenverarbeitung (UP -> Status Bildschirm, DOWN -> PixelDust Sand Demo)
void handleButtons() {
  if (millis() - lastButtonCheck < 50) return;
  lastButtonCheck = millis();

  bool currentUpState   = digitalRead(BUTTON_UP_PIN);
  bool currentDownState = digitalRead(BUTTON_DOWN_PIN);

  if (lastUpState == HIGH && currentUpState == LOW) {
    if (currentMode == MODE_STATUS) {
      currentMode = MODE_NORMAL;
      Serial.println(F("\n[Taste UP] -> Hauptbildschirm (Uhr & Energie)"));
    } else {
      currentMode = MODE_STATUS;
      Serial.println(F("\n[Taste UP] -> Status-Bildschirm (IP & Netz)"));
    }
  }

  if (lastDownState == HIGH && currentDownState == LOW) {
    if (currentMode == MODE_PIXELDUST) {
      currentMode = MODE_NORMAL;
      Serial.println(F("\n[Taste DOWN] -> Hauptbildschirm (Uhr & Energie)"));
    } else {
      currentMode = MODE_PIXELDUST;
      initPixelDust();
      Serial.println(F("\n[Taste DOWN] -> PixelDust Sand-Physik-Demo"));
    }
  }

  lastUpState   = currentUpState;
  lastDownState = currentDownState;
}

// ----------------------------------------------------------------------
// Captive Portal DNS Server (DNS-Anfragen abfangen & umleiten)
// ----------------------------------------------------------------------
void processDNS() {
  int packetSize = dnsUdp.parsePacket();
  if (packetSize < 12) return;

  byte packetBuffer[512];
  if (packetSize > 512) packetSize = 512;
  dnsUdp.read(packetBuffer, packetSize);

  packetBuffer[2] = 0x84; // Standard Query Response, Authoritative
  packetBuffer[3] = 0x00;
  packetBuffer[6] = 0x00; // Answer Count = 1
  packetBuffer[7] = 0x01;

  int replyLen = packetSize;
  packetBuffer[replyLen++] = 0xC0;
  packetBuffer[replyLen++] = 0x0C;
  packetBuffer[replyLen++] = 0x00; // Type A
  packetBuffer[replyLen++] = 0x01;
  packetBuffer[replyLen++] = 0x00; // Class IN
  packetBuffer[replyLen++] = 0x01;
  packetBuffer[replyLen++] = 0x00; // TTL (60 Sek)
  packetBuffer[replyLen++] = 0x00;
  packetBuffer[replyLen++] = 0x00;
  packetBuffer[replyLen++] = 0x3C;
  packetBuffer[replyLen++] = 0x00; // RDLENGTH = 4 Byte
  packetBuffer[replyLen++] = 0x04;
  packetBuffer[replyLen++] = 192;  // IP: 192.168.4.1
  packetBuffer[replyLen++] = 168;
  packetBuffer[replyLen++] = 4;
  packetBuffer[replyLen++] = 1;

  dnsUdp.beginPacket(dnsUdp.remoteIP(), dnsUdp.remotePort());
  dnsUdp.write(packetBuffer, replyLen);
  dnsUdp.endPacket();

  logDebug(String(F("[DNS] Umleitung an 192.168.4.1 für Anfrager")));
}

// ----------------------------------------------------------------------
// URL Decoder Helper
// ----------------------------------------------------------------------
String urlDecode(String input) {
  String decoded = "";
  char c;
  char code[3];
  for (unsigned int i = 0; i < input.length(); i++) {
    c = input.charAt(i);
    if (c == '+') {
      decoded += ' ';
    } else if (c == '%') {
      code[0] = input.charAt(++i);
      code[1] = input.charAt(++i);
      code[2] = '\0';
      decoded += (char) strtol(code, NULL, 16);
    } else {
      decoded += c;
    }
  }
  return decoded;
}

// ----------------------------------------------------------------------
// Flash Lade- & Speicherfunktionen (SAMD51)
// ----------------------------------------------------------------------
void loadConfig() {
  EEPROM.get(0, config);
  if (config.magic != CONFIG_MAGIC) {
    Serial.println(F("[Config] EEPROM ungültig oder neu. Lade Standardwerte..."));
    config = defaultConfig;
    EEPROM.put(0, config);
    EEPROM.commit();
  } else {
    Serial.println(F("[Config] Konfiguration erfolgreich geladen."));
  }
}

void saveConfig() {
  config.magic = CONFIG_MAGIC;
  EEPROM.put(0, config);
  EEPROM.commit();
  Serial.println(F("[Config] Konfiguration erfolgreich im Flash gespeichert."));
}

void showStatus() {
  Serial.println(F("\n=================================================="));
  Serial.println(F(" HOMENODEMATRIX CONFIG & STATUS"));
  Serial.println(F("=================================================="));
  Serial.print(F(" Mode:          ")); Serial.println(isAPMode ? "ACCESS POINT (Setup)" : "WLAN CLIENT");
  Serial.print(F(" Display Mode:  ")); 
  if (currentMode == MODE_PIXELDUST) Serial.println("PIXEL DUST DEMO");
  else if (currentMode == MODE_STATUS) Serial.println("STATUS SCREEN");
  else Serial.println("UHR & ENERGIE");
  Serial.print(F(" WLAN Status:   ")); Serial.println(WiFi.status() == WL_CONNECTED ? "VERBUNDEN" : "DISCONNECTED");
  Serial.print(F(" Debug Log:     ")); Serial.println(debugMode ? "AN (ON)" : "AUS (OFF)");
  Serial.print(F(" WLAN SSID:     '")); Serial.print(config.wifi_ssid); Serial.println(F("'"));
  Serial.print(F(" WLAN Pass:     '")); Serial.print(config.wifi_pass); Serial.println(F("'"));
  Serial.print(F(" Shelly IP:     '")); Serial.print(config.shelly_ip); Serial.println(F("'"));
  Serial.print(F(" Shelly Path:   '")); Serial.print(config.shelly_path); Serial.println(F("'"));
  Serial.print(F(" Inverter IP:   '")); Serial.print(config.inverter_ip); Serial.println(F("'"));
  Serial.print(F(" Inverter Path: '")); Serial.print(config.inverter_path); Serial.println(F("'"));
  Serial.print(F(" UTC Offset:    ")); Serial.print(config.utc_offset_sec); Serial.println(F(" sek"));
  Serial.print(F(" Helligkeit:    ")); Serial.println(config.brightness);
  Serial.print(F(" IP Adresse:    ")); Serial.println(isAPMode ? apIPAddress : WiFi.localIP());
  Serial.println(F("==================================================\n"));
}

void printHelp() {
  Serial.println(F("\n--- HomeNodeMatrix Serial CLI Befehle ---"));
  Serial.println(F(" status                - Zeigt aktuellen Status & Einstellungen an"));
  Serial.println(F(" mode normal / status / dust - Wechselt den Display-Modus"));
  Serial.println(F(" brightness <10-255>   - Stellt die Display-Helligkeit ein (z.B. brightness 100)"));
  Serial.println(F(" debug on / debug off  - Schaltet Live-Debug-Logs an oder aus"));
  Serial.println(F(" wifi <ssid> <pass>    - Setzt WLAN Name und Passwort"));
  Serial.println(F(" shelly <ip> [pfad]    - Setzt Shelly 3Pro IP-Adresse"));
  Serial.println(F(" inverter <ip> [pfad]  - Setzt Wechselrichter IP-Adresse"));
  Serial.println(F(" utc <offset>          - Setzt GMT Offset in Sekunden (7200 = MESZ)"));
  Serial.println(F(" save                  - Speichert Einstellungen im Flash Speicher"));
  Serial.println(F(" reset                 - Setzt Einstellungen auf Werkseinstellungen zurück"));
  Serial.println(F(" reboot                - Führt einen Hardware-Neustart durch"));
  Serial.println(F(" help / ?              - Zeigt dieses Hilfemenü an\n"));
  Serial.print(F("CLI> "));
}

// ----------------------------------------------------------------------
// Serial CLI (Kommandozeilen-Interface)
// ----------------------------------------------------------------------
void handleSerialCLI() {
  while (Serial.available() > 0) {
    char c = Serial.read();

    if (c == '\b' || c == 0x7F) {
      if (serialBuffer.length() > 0) {
        serialBuffer.remove(serialBuffer.length() - 1);
        Serial.print("\b \b");
      }
      continue;
    }

    if (c != '\r' && c != '\n') {
      Serial.write(c);
    }

    if (c == '\r' || c == '\n') {
      Serial.println();
      serialBuffer.trim();

      if (serialBuffer.length() > 0) {
        if (serialBuffer.equals("help") || serialBuffer.equals("?")) {
          printHelp();
        } else if (serialBuffer.equals("status") || serialBuffer.equals("show")) {
          showStatus();
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("mode status")) {
          currentMode = MODE_STATUS;
          Serial.println(F("[CLI] Status-Bildschirm AKTIVIERT!"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("mode dust") || serialBuffer.equals("pixeldust")) {
          currentMode = MODE_PIXELDUST;
          initPixelDust();
          Serial.println(F("[CLI] PixelDust Sand-Demo AKTIVIERT!"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("mode normal")) {
          currentMode = MODE_NORMAL;
          Serial.println(F("[CLI] Hauptbildschirm (Uhr & Energie) AKTIVIERT!"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("debug on") || serialBuffer.equals("debug 1")) {
          debugMode = true;
          Serial.println(F("[CLI] Live-Debug-Logs AKTIVIERT (on)"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("debug off") || serialBuffer.equals("debug 0")) {
          debugMode = false;
          Serial.println(F("[CLI] Live-Debug-Logs DEAKTIVIERT (off) - CLI bleibt sauber!"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.startsWith("wifi ")) {
          int space1 = serialBuffer.indexOf(' ');
          int space2 = serialBuffer.indexOf(' ', space1 + 1);
          if (space1 > 0) {
            String ssid = (space2 > 0) ? serialBuffer.substring(space1 + 1, space2) : serialBuffer.substring(space1 + 1);
            String pass = (space2 > 0) ? serialBuffer.substring(space2 + 1) : "";
            ssid.toCharArray(config.wifi_ssid, sizeof(config.wifi_ssid));
            pass.toCharArray(config.wifi_pass, sizeof(config.wifi_pass));
            saveConfig();
            Serial.println(F("[CLI] WLAN Einstellungen aktualisiert & gespeichert!"));
            showStatus();
          }
          Serial.print(F("CLI> "));
        } else if (serialBuffer.startsWith("shelly ")) {
          int space1 = serialBuffer.indexOf(' ');
          int space2 = serialBuffer.indexOf(' ', space1 + 1);
          if (space1 > 0) {
            String ip = (space2 > 0) ? serialBuffer.substring(space1 + 1, space2) : serialBuffer.substring(space1 + 1);
            ip.toCharArray(config.shelly_ip, sizeof(config.shelly_ip));
            if (space2 > 0) {
              String path = serialBuffer.substring(space2 + 1);
              path.toCharArray(config.shelly_path, sizeof(config.shelly_path));
            }
            saveConfig();
            Serial.println(F("[CLI] Shelly Einstellungen aktualisiert & gespeichert!"));
          }
          Serial.print(F("CLI> "));
        } else if (serialBuffer.startsWith("inverter ")) {
          int space1 = serialBuffer.indexOf(' ');
          int space2 = serialBuffer.indexOf(' ', space1 + 1);
          if (space1 > 0) {
            String ip = (space2 > 0) ? serialBuffer.substring(space1 + 1, space2) : serialBuffer.substring(space1 + 1);
            ip.toCharArray(config.inverter_ip, sizeof(config.inverter_ip));
            if (space2 > 0) {
              String path = serialBuffer.substring(space2 + 1);
              path.toCharArray(config.inverter_path, sizeof(config.inverter_path));
            }
            saveConfig();
            Serial.println(F("[CLI] Wechselrichter Einstellungen aktualisiert & gespeichert!"));
          }
          Serial.print(F("CLI> "));
        } else if (serialBuffer.startsWith("utc ")) {
          config.utc_offset_sec = serialBuffer.substring(4).toInt();
          saveConfig();
          Serial.println(F("[CLI] UTC Offset aktualisiert!"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.startsWith("brightness ") || serialBuffer.startsWith("b ")) {
          int spacePos = serialBuffer.indexOf(' ');
          int val = serialBuffer.substring(spacePos + 1).toInt();
          applyBrightness(val);
          saveConfig();
          Serial.println(String(F("[CLI] Display-Helligkeit gesetzt auf: ")) + config.brightness);
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("save")) {
          saveConfig();
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("reset")) {
          config = defaultConfig;
          applyBrightness(config.brightness);
          saveConfig();
          Serial.println(F("[CLI] Einstellungen auf Werkseinstellungen zurückgesetzt!"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("reboot")) {
          Serial.println(F("[CLI] Neustart wird durchgeführt..."));
          delay(500);
          NVIC_SystemReset();
        } else {
          Serial.println(F("Unbekannter Befehl. Tippe 'help' für Befehlsübersicht."));
          Serial.print(F("CLI> "));
        }
      } else {
        printHelp();
      }
      serialBuffer = "";
    } else {
      serialBuffer += c;
    }
  }
}

// ----------------------------------------------------------------------
// HTML Konfigurationsseite (Shelly-Style Dark Theme) & Webserver
// ----------------------------------------------------------------------
void handleWebClient() {
  WiFiClient client = webServer.available();
  if (!client) return;

  logDebug(F("\n>>> [Web] Client hat sich mit dem Webserver verbunden!"));

  String requestHeader = "";
  String requestBody = "";
  String firstLine = "";
  boolean currentLineIsBlank = true;
  boolean isPost = false;
  int contentLength = 0;

  unsigned long timeout = millis() + 1500;
  while (client.connected() && millis() < timeout) {
    if (client.available()) {
      char c = client.read();
      requestHeader += c;

      if (firstLine.length() < 120 && c != '\r' && c != '\n' && requestHeader.indexOf('\n') == -1) {
        firstLine += c;
      }

      if (requestHeader.indexOf("Content-Length: ") >= 0) {
        int pos = requestHeader.indexOf("Content-Length: ") + 16;
        contentLength = requestHeader.substring(pos, requestHeader.indexOf("\r\n", pos)).toInt();
      }

      if (c == '\n' && currentLineIsBlank) {
        if (firstLine.startsWith("POST")) {
          isPost = true;
        }

        if (isPost && contentLength > 0) {
          unsigned long bodyTimeout = millis() + 1500;
          while (client.connected() && ((int)requestBody.length() < contentLength) && millis() < bodyTimeout) {
            if (client.available()) {
              requestBody += (char)client.read();
            }
          }
        }
        break;
      }

      if (c == '\n') {
        currentLineIsBlank = true;
      } else if (c != '\r') {
        currentLineIsBlank = false;
      }
    }
  }

  logDebug(String(F("[Web] Anfrage-Zeile: ")) + firstLine);

  // POST Request (Formular verarbeiten & Speichern)
  if (isPost && requestBody.length() > 0) {
    logDebug(F("[Web] POST Formulardaten empfangen:"));
    logDebug(requestBody);

    int paramPos = 0;
    while (paramPos < (int)requestBody.length()) {
      int nextAmp = requestBody.indexOf('&', paramPos);
      if (nextAmp == -1) nextAmp = requestBody.length();
      
      String pair = requestBody.substring(paramPos, nextAmp);
      int eqPos = pair.indexOf('=');
      if (eqPos > 0) {
        String key = pair.substring(0, eqPos);
        String val = urlDecode(pair.substring(eqPos + 1));

        if (key == "ssid") val.toCharArray(config.wifi_ssid, sizeof(config.wifi_ssid));
        else if (key == "pass") val.toCharArray(config.wifi_pass, sizeof(config.wifi_pass));
        else if (key == "shelly_ip") val.toCharArray(config.shelly_ip, sizeof(config.shelly_ip));
        else if (key == "shelly_path") val.toCharArray(config.shelly_path, sizeof(config.shelly_path));
        else if (key == "inverter_ip") val.toCharArray(config.inverter_ip, sizeof(config.inverter_ip));
        else if (key == "inverter_path") val.toCharArray(config.inverter_path, sizeof(config.inverter_path));
        else if (key == "utc_offset") config.utc_offset_sec = val.toInt();
        else if (key == "brightness") applyBrightness(val.toInt());
      }
      paramPos = nextAmp + 1;
    }

    saveConfig();

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Connection: close");
    client.println();
    client.println("<!DOCTYPE html><html><head><meta charset='utf-8'><style>");
    client.println("body{background:#121212;color:#fff;font-family:sans-serif;text-align:center;padding-top:50px;}");
    client.println(".card{background:#1e1e1e;max-width:400px;margin:auto;padding:30px;border-radius:12px;box-shadow:0 4px 15px rgba(0,0,0,0.5);}");
    client.println("h2{color:#00e676;}</style></head><body>");
    client.println("<div class='card'><h2>Einstellungen gespeichert!</h2>");
    client.println("<p>Das MatrixPortal startet jetzt neu und verbindet sich mit dem WLAN...</p>");
    client.println("<p>Bitte 10 Sekunden warten.</p></div></body></html>");
    client.flush();
    delay(10);
    client.stop();

    Serial.println(F("[Web] Einstellungen gespeichert. Führe Hardware-Reset durch..."));
    delay(1000);
    NVIC_SystemReset();
    return;
  }

  // Captive Portal Auto-Popup Redirect für externe Host-Anfragen
  if (isAPMode && firstLine.indexOf("192.168.4.1") == -1 && firstLine.indexOf("GET / ") == -1) {
    logDebug(F("[Web] Captive Portal Redirect an http://192.168.4.1/"));
    client.println("HTTP/1.1 302 Found");
    client.println("Location: http://192.168.4.1/");
    client.println("Connection: close");
    client.println();
    client.flush();
    delay(10);
    client.stop();
    return;
  }

  // GET Request (HTML Konfigurationsseite ausgeben)
  logDebug(F("[Web] Sende Konfigurations-Webseite an den Client..."));

  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/html; charset=utf-8");
  client.println("Connection: close");
  client.println();

  client.println("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  client.println("<title>HomeNodeMatrix Setup</title><style>");
  client.println("body{background:#121212;color:#e0e0e0;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;padding:15px;}");
  client.println(".container{max-width:440px;margin:0 auto;background:#1e1e1e;padding:20px;border-radius:14px;box-shadow:0 8px 24px rgba(0,0,0,0.6);}");
  client.println("h1{color:#00e676;font-size:20px;margin-bottom:16px;text-align:center;font-weight:600;}");
  client.println(".section{background:#2a2a2a;padding:14px;border-radius:10px;margin-bottom:14px;}");
  client.println("h2{font-size:13px;color:#80d8ff;margin-top:0;text-transform:uppercase;letter-spacing:1px;}");
  client.println("label{display:block;font-size:12px;margin:8px 0 4px;color:#aaa;}");
  client.println("input[type=text],input[type=password],input[type=number]{width:100%;padding:10px;border-radius:6px;border:1px solid #444;background:#333;color:#fff;box-sizing:border-box;font-size:14px;}");
  client.println("input:focus{border-color:#00e676;outline:none;}");
  client.println("button{width:100%;padding:14px;background:#00c853;border:none;border-radius:8px;color:#fff;font-size:16px;font-weight:bold;cursor:pointer;margin-top:10px;}");
  client.println("button:hover{background:#00e676;}");
  client.println(".info{font-size:11px;color:#888;text-align:center;margin-top:12px;}");
  client.println("</style></head><body>");
  client.println("<div class='container'><h1>HomeNodeMatrix Setup</h1>");
  client.println("<form method='POST' action='/save'>");

  client.println("<div class='section'><h2>WLAN Einstellungen</h2>");
  client.println("<label>WLAN Name (SSID)</label>");
  client.print("<input type='text' name='ssid' value='"); client.print(config.wifi_ssid); client.println("' required>");
  client.println("<label>WLAN Passwort</label>");
  client.print("<input type='password' name='pass' value='"); client.print(config.wifi_pass); client.println("'>");
  client.println("</div>");

  client.println("<div class='section'><h2>Shelly 3Pro (Netz)</h2>");
  client.println("<label>Shelly IP Adresse</label>");
  client.print("<input type='text' name='shelly_ip' value='"); client.print(config.shelly_ip); client.println("' required>");
  client.println("<label>API Pfad</label>");
  client.print("<input type='text' name='shelly_path' value='"); client.print(config.shelly_path); client.println("'>");
  client.println("</div>");

  client.println("<div class='section'><h2>Wechselrichter (Fronius / OpenDTU)</h2>");
  client.println("<label>Wechselrichter IP</label>");
  client.print("<input type='text' name='inverter_ip' value='"); client.print(config.inverter_ip); client.println("' required>");
  client.println("<label>API Pfad</label>");
  client.print("<input type='text' name='inverter_path' value='"); client.print(config.inverter_path); client.println("'>");
  client.println("</div>");

  client.println("<div class='section'><h2>System & Display</h2>");
  client.println("<label>UTC Offset (Sekunden, z.B. 7200 = MESZ)</label>");
  client.print("<input type='number' name='utc_offset' value='"); client.print(config.utc_offset_sec); client.println("'>");
  client.println("<label>Helligkeit (10 - 255)</label>");
  client.print("<input type='number' name='brightness' min='10' max='255' value='"); client.print(config.brightness); client.println("'>");
  client.println("</div>");

  client.println("<button type='submit'>Speichern & Neustarten</button>");
  client.println("</form>");
  client.println("<div class='info'>MatrixPortal M4 Smart Energy Display</div>");
  client.println("</div></body></html>");

  client.flush();
  delay(10);
  client.stop();

  logDebug(F("<<< [Web] Antwort vollständig gesendet. Client getrennt."));
}

// ----------------------------------------------------------------------
// NTP Zeitabfrage über UDP (mit Timeout)
// ----------------------------------------------------------------------
const int NTP_PACKET_SIZE = 48;
byte packetBuffer[NTP_PACKET_SIZE];

unsigned long fetchNTPTime() {
  ntpUdp.begin(2390);
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  packetBuffer[0] = 0b11100011;
  packetBuffer[1] = 0;
  packetBuffer[2] = 6;
  packetBuffer[3] = 0xEC;

  ntpUdp.beginPacket(NTP_SERVER, 123);
  ntpUdp.write(packetBuffer, NTP_PACKET_SIZE);
  ntpUdp.endPacket();

  unsigned long startWait = millis();
  while (millis() - startWait < 500) {
    if (ntpUdp.parsePacket()) {
      ntpUdp.read(packetBuffer, NTP_PACKET_SIZE);
      unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
      unsigned long lowWord  = word(packetBuffer[42], packetBuffer[43]);
      unsigned long secsSince1900 = (highWord << 16) | lowWord;
      const unsigned long seventyYears = 2208988800UL;
      unsigned long epoch = secsSince1900 - seventyYears;
      return epoch + config.utc_offset_sec;
    }
    delay(10);
  }
  return 0;
}

// ----------------------------------------------------------------------
// Abfrage Shelly 3Pro (Gen2 HTTP API)
// ----------------------------------------------------------------------
void updateShellyData() {
  if (strlen(config.shelly_ip) == 0 || strcmp(config.shelly_ip, "0.0.0.0") == 0) {
    gridOk = false;
    return;
  }

  WiFiClient client;
  client.setTimeout(1000);
  if (!client.connect(config.shelly_ip, 80)) {
    logDebug(F("[Netz] Shelly 3Pro nicht erreichbar. Warten..."));
    gridOk = false;
    return;
  }

  client.print(String("GET ") + config.shelly_path + " HTTP/1.1\r\n" +
               "Host: " + config.shelly_ip + "\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 1500) { client.stop(); gridOk = false; return; }
  }

  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, client);
  client.stop();

  if (!err) {
    gridOk = true;
    if (!doc["em:0"]["total_act_power"].isNull()) {
      gridPowerW = doc["em:0"]["total_act_power"].as<float>();
    } else if (doc["emmeters"].is<JsonArray>()) {
      float sum = 0;
      for (JsonObject em : doc["emmeters"].as<JsonArray>()) sum += em["power"].as<float>();
      gridPowerW = sum;
    } else if (!doc["total_power"].isNull()) {
      gridPowerW = doc["total_power"].as<float>();
    }
    logDebug("[Netz] Aktuelle Netzleistung (Shelly): " + String(gridPowerW) + " W");
  } else {
    logDebug("[Netz] Shelly JSON Fehler: " + String(err.c_str()));
    gridOk = false;
  }
}

// ----------------------------------------------------------------------
// Abfrage Wechselrichter (Fronius Solar API v1 & OpenDTU Parser)
// ----------------------------------------------------------------------
void updateInverterData() {
  if (strlen(config.inverter_ip) == 0 || strcmp(config.inverter_ip, "0.0.0.0") == 0) {
    solarOk = false;
    return;
  }

  WiFiClient client;
  client.setTimeout(1000);
  if (!client.connect(config.inverter_ip, 80)) {
    logDebug(F("[Solar] Fronius / Wechselrichter nicht erreichbar. Warten..."));
    solarOk = false;
    return;
  }

  client.print(String("GET ") + config.inverter_path + " HTTP/1.1\r\n" +
               "Host: " + config.inverter_ip + "\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 1500) { client.stop(); solarOk = false; return; }
  }

  while (client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, client);
  client.stop();

  if (!err) {
    solarOk = true;
    // 1. Fronius Solar API: Site P_PV
    if (!doc["Body"]["Data"]["Site"]["P_PV"].isNull()) {
      solarPowerW = doc["Body"]["Data"]["Site"]["P_PV"].as<float>();
    } 
    // 2. Fronius Solar API: Inverters "1" P
    else if (!doc["Body"]["Data"]["Inverters"]["1"]["P"].isNull()) {
      solarPowerW = doc["Body"]["Data"]["Inverters"]["1"]["P"].as<float>();
    }
    // 3. Fronius Solar API: Realtime PAC Values "1"
    else if (!doc["Body"]["Data"]["PAC"]["Values"]["1"].isNull()) {
      solarPowerW = doc["Body"]["Data"]["PAC"]["Values"]["1"].as<float>();
    }
    // 4. OpenDTU / AhoyDTU
    else if (!doc["total"]["power"]["val"].isNull()) {
      solarPowerW = doc["total"]["power"]["val"].as<float>();
    } else if (!doc["power"].isNull()) {
      solarPowerW = doc["power"].as<float>();
    } else {
      solarPowerW = 0.0;
    }

    logDebug("[Solar] Aktuelle Solarleistung (Fronius): " + String(solarPowerW) + " W");
  } else {
    logDebug("[Solar] Inverter JSON Fehler: " + String(err.c_str()));
    solarOk = false;
  }
}

String formatPower(float watts) {
  float absWatts = abs(watts);
  if (absWatts >= 1000.0) {
    return String(watts / 1000.0, 2) + "kW";
  } else {
    return String((int)watts) + "W";
  }
}

// ----------------------------------------------------------------------
// Display Rendering Modes
// ----------------------------------------------------------------------

// Access Point Setup Bildschirm mit Wi-Fi Symbol (Ohne klobigen Text)
void drawAPScreen() {
  matrix.fillScreen(COLOR_BLACK);
  matrix.setTextWrap(false);
  matrix.setTextSize(1);

  // Wi-Fi Symbol oben zentriert
  drawWifiIcon(28, 2, COLOR_CYAN);

  matrix.setTextColor(COLOR_WHITE);
  matrix.setCursor(2, 13);
  matrix.print(F("PIN:"));
  matrix.setTextColor(COLOR_YELLOW);
  matrix.setCursor(2, 23);
  matrix.print(ap_password);

  matrix.setTextColor(COLOR_WHITE);
  matrix.setCursor(2, 34);
  matrix.print(F("SETUP IP:"));

  matrix.setTextColor(COLOR_GREEN);
  matrix.setCursor(0, 45);
  matrix.print(F("192.168.4.1"));

  matrix.setTextColor(COLOR_GRAY);
  matrix.setCursor(8, 56);
  matrix.print(F("HomeNode"));

  matrix.show();
}

// Dedicated Status Bildschirm (Taste UP) mit ultra-kompakter IP Schrift (3x5 Pixel Font)
void drawStatusScreen() {
  matrix.fillScreen(COLOR_BLACK);
  matrix.setTextWrap(false);
  matrix.setTextSize(1);

  // Header: Wi-Fi Icon + "STATUS"
  drawWifiIcon(2, 2, WiFi.status() == WL_CONNECTED ? COLOR_GREEN : COLOR_RED);
  matrix.setTextColor(COLOR_CYAN);
  matrix.setCursor(14, 2);
  matrix.print(F("STATUS"));

  matrix.drawFastHLine(0, 11, 64, COLOR_GRAY);

  // 1. Matrix IP Adresse
  matrix.setTextColor(COLOR_YELLOW);
  matrix.setCursor(2, 14);
  matrix.print(F("MATRIX IP:"));

  IPAddress localIp = WiFi.localIP();
  String matrixIpStr = isAPMode ? "192.168.4.1" : (WiFi.status() == WL_CONNECTED ? (String(localIp[0]) + "." + String(localIp[1]) + "." + String(localIp[2]) + "." + String(localIp[3])) : "DISCONNECTED");
  drawTinyString(2, 23, matrixIpStr, COLOR_WHITE);

  // 2. Shelly IP & Status
  matrix.setTextColor(COLOR_CYAN);
  matrix.setCursor(2, 31);
  matrix.print(F("SHELLY:"));
  matrix.setCursor(46, 31);
  matrix.setTextColor(gridOk ? COLOR_GREEN : COLOR_RED);
  matrix.print(gridOk ? F("OK") : F("ERR"));

  String shellyIpStr = strlen(config.shelly_ip) > 0 ? String(config.shelly_ip) : "0.0.0.0";
  drawTinyString(2, 40, shellyIpStr, COLOR_WHITE);

  // 3. Fronius IP & Status
  matrix.setTextColor(COLOR_ORANGE);
  matrix.setCursor(2, 48);
  matrix.print(F("FRONIUS:"));
  matrix.setCursor(46, 48);
  matrix.setTextColor(solarOk ? COLOR_GREEN : COLOR_RED);
  matrix.print(solarOk ? F("OK") : F("ERR"));

  String inverterIpStr = strlen(config.inverter_ip) > 0 ? String(config.inverter_ip) : "0.0.0.0";
  drawTinyString(2, 57, inverterIpStr, COLOR_WHITE);

  matrix.show();
}

// Hauptbildschirm: Uhrzeit, Datum & Energie (Sauber & ohne Balken)
void drawNormalScreen() {
  matrix.fillScreen(COLOR_BLACK);
  matrix.setTextWrap(false);

  unsigned long nowEpoch = localUnixTime + ((millis() - lastTimeSyncMs) / 1000);
  int hours   = (nowEpoch % 86400L) / 3600;
  int minutes = (nowEpoch % 3600) / 60;
  int seconds = (nowEpoch % 60);

  int year, month, day, wday;
  epochToDate(nowEpoch, year, month, day, wday);

  // Wochentags-Mapping für wday = (days + 4) % 7 (0=Sonntag, 1=Montag, ..., 4=Donnerstag)
  const char* wdays[] = {"So.", "Mo.", "Di.", "Mi.", "Do.", "Fr.", "Sa."};

  // 1. Uhrzeit (Cyan, y=2) & Wi-Fi Icon (oben rechts, y=2, x=54)
  matrix.setTextColor(COLOR_CYAN);
  matrix.setTextSize(1);
  char timeBuf[10];
  sprintf(timeBuf, "%02d:%02d:%02d", hours, minutes, seconds);
  matrix.setCursor(2, 2);
  matrix.print(timeBuf);

  // Wi-Fi Symbol (Grün = Verbunden, Rot = Getrennt)
  drawWifiIcon(54, 2, WiFi.status() == WL_CONNECTED ? COLOR_GREEN : COLOR_RED);

  // 2. Datum & Wochentag (Weiß, y=11) -> "Mo. 3.8.26"
  matrix.setTextColor(COLOR_WHITE);
  char dateBuf[16];
  sprintf(dateBuf, "%s %d.%d.%02d", wdays[wday], day, month, year % 100);
  
  int textWidth = strlen(dateBuf) * 6;
  int xPos = (64 - textWidth) / 2;
  if (xPos < 0) xPos = 0;
  matrix.setCursor(xPos, 11);
  matrix.print(dateBuf);

  matrix.drawFastHLine(0, 20, 64, COLOR_GRAY);

  // 3. Solar Leistung (y=23..33)
  matrix.setCursor(2, 23);
  matrix.setTextColor(COLOR_YELLOW);
  matrix.print(F("SOLAR"));

  matrix.setCursor(2, 33);
  if (solarOk) {
    matrix.setTextColor(COLOR_GREEN);
    matrix.print(formatPower(solarPowerW));
  } else {
    matrix.setTextColor(COLOR_RED);
    matrix.print(F("OFFLINE"));
  }

  matrix.drawFastHLine(0, 43, 64, COLOR_GRAY);

  // 4. Netz Leistung (y=46..55) -> Einheitliche Überschrift "NETZ"
  matrix.setCursor(2, 46);
  matrix.setTextColor(COLOR_CYAN);
  matrix.print(F("NETZ"));

  matrix.setCursor(2, 55);
  if (gridOk) {
    matrix.setTextColor(gridPowerW <= 0 ? COLOR_GREEN : COLOR_ORANGE);
    matrix.print(formatPower(gridPowerW));
  } else {
    matrix.setTextColor(COLOR_RED);
    matrix.print(F("OFFLINE"));
  }

  matrix.show();
}

// ----------------------------------------------------------------------
// SETUP
// ----------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_UP_PIN, INPUT_PULLUP);
  pinMode(BUTTON_DOWN_PIN, INPUT_PULLUP);

#if defined(SPIWIFI_SS) && defined(ESP32_RESETN)
  WiFi.setPins(SPIWIFI_SS, SPIWIFI_ACK, ESP32_RESETN, ESP32_GPIO0, &SPIWIFI);
#endif

  loadConfig();

  ProtomatterStatus status = matrix.begin();
  Serial.printf("[Display] Protomatter Status: %d\n", status);

  // Dynamische Farbskalierung & Helligkeit setzen (10-255)
  updateColors();

  if (accel.begin(0x19)) {
    accel.setRange(LIS3DH_RANGE_4_G);
    accelOk = true;
    Serial.println(F("[Sensor] LIS3DH Beschleunigungssensor (PixelDust) bereit."));
  } else {
    Serial.println(F("[Sensor] LIS3DH nicht gefunden (Software-Gravitation aktiv)."));
  }

  if (strlen(config.wifi_ssid) == 0) {
    Serial.println(F("[WLAN] Keine SSID gespeichert. Starte Access Point Mode..."));
    isAPMode = true;
  } else {
    matrix.fillScreen(COLOR_BLACK);
    drawWifiIcon(28, 25, COLOR_CYAN);
    matrix.show();

    Serial.print(F("[WLAN] Verbinde mit SSID: '"));
    Serial.print(config.wifi_ssid);
    Serial.println(F("'"));

    WiFi.begin(config.wifi_ssid, config.wifi_pass);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 25) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("\n[WLAN] Verbindung fehlgeschlagen! Starte Access Point Mode..."));
      isAPMode = true;
    }
  }

  if (isAPMode) {
    randomSeed(micros());
    unsigned long pin = random(10000000UL, 99999999UL);
    sprintf(ap_password, "%08lu", pin);

    apStatus = WiFi.beginAP(AP_SSID, ap_password);
    delay(3000);

    apIPAddress = IPAddress(192, 168, 4, 1);
    dnsUdp.begin(53);
    webServer.begin();

    Serial.println(F("\n=================================================="));
    Serial.println(F(" ACCESS POINT & CAPTIVE PORTAL GESTARTET"));
    Serial.print(F(" SSID:     ")); Serial.println(AP_SSID);
    Serial.print(F(" PASS/PIN: ")); Serial.println(ap_password);
    Serial.print(F(" AP IP:    ")); Serial.println(apIPAddress);
    Serial.println(F(" Drücke Taste UP für Status-Bildschirm, DOWN für PixelDust Demo!"));
    Serial.println(F(" Tippe 'help' auf der Serial Konsole für CLI Befehle"));
    Serial.println(F("==================================================\n"));

    drawAPScreen();
  } else {
    Serial.println(F("\n[WLAN] Erfolgreich Verbunden!"));
    Serial.print(F("[WLAN] IP Adresse: "));
    Serial.println(WiFi.localIP());

    matrix.fillScreen(COLOR_BLACK);
    drawWifiIcon(28, 25, COLOR_GREEN);
    matrix.show();
    delay(1000);

    unsigned long ntpEpoch = fetchNTPTime();
    if (ntpEpoch > 0) {
      localUnixTime = ntpEpoch;
      lastTimeSyncMs = millis();
    }

    webServer.begin();
  }

  Serial.println(F("[Web] HTTP Webserver auf Port 80 gestartet!"));
  printHelp();
}

// ----------------------------------------------------------------------
// MAIN LOOP
// ----------------------------------------------------------------------
void loop() {
  // 1. Tasten verarbeiten (UP -> Status, DOWN -> PixelDust)
  handleButtons();

  // 2. Serial CLI verarbeiten
  handleSerialCLI();

  // 3. Ausführung des ausgewählten Display-Modus
  if (currentMode == MODE_PIXELDUST) {
    runPixelDustFrame();
    return;
  }

  // 4. AP Mode Handler
  if (isAPMode) {
    if (apStatus != WiFi.status()) {
      apStatus = WiFi.status();
      if (apStatus == WL_AP_CONNECTED) {
        logDebug(F("[WLAN] Client hat sich mit dem Hotspot verbunden!"));
      }
    }
    processDNS();
    handleWebClient();
    delay(10);
    return;
  }

  // 5. Webserver Anfragen verarbeiten
  handleWebClient();

  unsigned long currentMs = millis();

  // 6. Display Rendering je nach Modus
  if (currentMs - lastDisplayRedraw >= 100) {
    if (currentMode == MODE_STATUS) {
      drawStatusScreen();
    } else {
      drawNormalScreen();
    }
    lastDisplayRedraw = currentMs;
  }

  // 7. NTP Update (stündlich)
  if (currentMs - lastNTPUpdate > 3600000UL || localUnixTime == 0) {
    if (WiFi.status() == WL_CONNECTED) {
      unsigned long ntpEpoch = fetchNTPTime();
      if (ntpEpoch > 0) {
        localUnixTime = ntpEpoch;
        lastTimeSyncMs = currentMs;
      }
    }
    lastNTPUpdate = currentMs;
  }

  // 8. Abfrage Shelly 3Pro
  unsigned long shellyInterval = gridOk ? POLL_ONLINE_MS : RETRY_OFFLINE_MS;
  if (currentMs - lastShellyAttempt >= shellyInterval) {
    if (WiFi.status() == WL_CONNECTED) {
      updateShellyData();
    }
    lastShellyAttempt = currentMs;
  }

  // 9. Abfrage Wechselrichter (Fronius)
  unsigned long inverterInterval = solarOk ? POLL_ONLINE_MS : RETRY_OFFLINE_MS;
  if (currentMs - lastInverterAttempt >= inverterInterval) {
    if (WiFi.status() == WL_CONNECTED) {
      updateInverterData();
    }
    lastInverterAttempt = currentMs;
  }

  delay(10);
}
