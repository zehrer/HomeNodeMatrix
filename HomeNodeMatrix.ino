/* ----------------------------------------------------------------------
  HomeNodeMatrix - MatrixPortal M4 (64x64 LED Matrix)
  Smart Energy & Time Display with Web Configuration & Serial CLI
  - Fix Date truncation overflow: 2-letter English weekdays (Su., Mo., Tu., etc.)
    preventing 11/12-char dates from overflowing 64px matrix width.
  - Multi-Language Support: English & German (Configurable via Web & CLI)
  - Boot Wi-Fi Connection Screen: Clean Wi-Fi icon with 8 filling-up progress dots (no text)
  - Automatic transition to AP Setup Mode when all 8 dots fill up without connection
  - Ultra-compact 3x5 Pixel Font for 15-char IP addresses (e.g. 192.168.178.154)
  - Dynamic Brightness Control via Web, CLI & Flash persistence
  - UP Button   -> Status Screen (Matrix IP, Shelly IP & Fronius IP)
  - DOWN Button -> PixelDust Sand Physics Demo (LIS3DH Accelerometer)
  ---------------------------------------------------------------------- */

#include <Wire.h>
#include <SPI.h>
#include <WiFiNINA.h>             // Wi-Fi driver for ESP32 AirLift
#include <WiFiUdp.h>
#include <FlashStorage_SAMD.h>    // Flash / EEPROM storage for SAMD51
#include <Adafruit_GFX.h>         // Graphics library
#include <Adafruit_Protomatter.h> // RGB Matrix display driver
#include <Adafruit_LIS3DH.h>      // On-board accelerometer
#include <Adafruit_PixelDust.h>   // Particle physics simulation
#include <ArduinoJson.h>          // JSON parser (v7)
#include "config.h"

// ----------------------------------------------------------------------
// Hardware Pins (Adafruit MatrixPortal M4)
// ----------------------------------------------------------------------
uint8_t rgbPins[]  = {7, 8, 9, 10, 11, 12};
uint8_t addrPins[] = {17, 18, 19, 20, 21}; // 5 Address Pins for 64x64 matrix
uint8_t clockPin   = 14;
uint8_t latchPin   = 15;
uint8_t oePin      = 16;

#define BUTTON_UP_PIN   2 // UP Button on MatrixPortal M4
#define BUTTON_DOWN_PIN 3 // DOWN Button on MatrixPortal M4

#if defined(ADAFRUIT_MATRIXPORTAL_M4) || defined(_VARIANT_MATRIXPORTAL_M4_)
  // Board definitions from Adafruit SAMD Core
#else
  #define SPIWIFI       SPI
  #define SPIWIFI_SS    33
  #define ESP32_RESETN  30
  #define SPIWIFI_ACK   31
  #define ESP32_GPIO0   -1
#endif

// Protomatter Matrix Instance (64x64, 4-bit color depth)
Adafruit_Protomatter matrix(
  64, 4, 1, rgbPins, 5, addrPins,
  clockPin, latchPin, oePin, true);

// LIS3DH Accelerometer & PixelDust Sand Simulation Instance
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

// Color Definitions
uint16_t COLOR_BLACK;
uint16_t COLOR_WHITE;
uint16_t COLOR_GRAY;
uint16_t COLOR_YELLOW;
uint16_t COLOR_GREEN;
uint16_t COLOR_RED;
uint16_t COLOR_CYAN;
uint16_t COLOR_BLUE;
uint16_t COLOR_ORANGE;

// Configuration Storage
ConfigData config;

// Status & Server Variables
WiFiServer webServer(80);
WiFiUDP ntpUdp;
WiFiUDP dnsUdp;
bool isAPMode = false;
bool debugMode = false; // Background debug logging OFF by default (keeps CLI clean)
char ap_password[9] = "84729103"; // 8-digit random PIN
IPAddress apIPAddress(192, 168, 4, 1);
int apStatus = WL_IDLE_STATUS;

unsigned long lastNTPUpdate = 0;
unsigned long lastShellyAttempt = 0;
unsigned long lastInverterAttempt = 0;
unsigned long lastDisplayRedraw = 0;

const unsigned long POLL_ONLINE_MS    = 3000;  // Poll every 3 sec when target is online
const unsigned long RETRY_OFFLINE_MS = 30000; // Back off 30 sec when target is offline

// Time Variables
unsigned long localUnixTime = 0;
unsigned long lastTimeSyncMs = 0;

// Power Values
float solarPowerW = 0.0;
float gridPowerW  = 0.0; // Negative = Feed-in (Export), Positive = Import
bool solarOk = false;
bool gridOk  = false;

// Serial CLI Buffer
String serialBuffer = "";

// Helper for conditional debug logging
void logDebug(const String& msg) {
  if (debugMode) {
    Serial.println(msg);
  }
}

// ----------------------------------------------------------------------
// Dynamic Color Scaling for Brightness Control (0-255)
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
// Ultra-Compact 3x5 Pixel Font for 15-char IP Addresses (Digits 0-9 & '.')
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
      curX += 3; // Dot is 2px + 1px spacing
    } else {
      drawTinyChar(curX, y, c, color);
      curX += 4; // Digit is 3px + 1px spacing
    }
  }
}

// ----------------------------------------------------------------------
// Wi-Fi Pixel Icon Renderer (8x7 Pixels)
// ----------------------------------------------------------------------
void drawWifiIcon(int16_t x, int16_t y, uint16_t color) {
  matrix.fillRect(x + 3, y + 6, 2, 2, color);  // Bottom Dot
  matrix.drawPixel(x + 1, y + 4, color);      // Inner Arc
  matrix.drawFastHLine(x + 2, y + 3, 4, color);
  matrix.drawPixel(x + 6, y + 4, color);
  matrix.drawPixel(x + 0, y + 1, color);      // Outer Arc
  matrix.drawFastHLine(x + 1, y + 0, 6, color);
  matrix.drawPixel(x + 7, y + 1, color);
}

// ----------------------------------------------------------------------
// Animated Wi-Fi Connection Screen: Ring of 8 Dots Filling Up (No Text)
// ----------------------------------------------------------------------
const int8_t spinnerDots[8][2] PROGMEM = {
  { 0, -14}, // 1. Top (12 o'clock)
  { 10, -10}, // 2. Top-Right (1:30)
  { 14,   0}, // 3. Right (3 o'clock)
  { 10,  10}, // 4. Bottom-Right (4:30)
  { 0,  14}, // 5. Bottom (6 o'clock)
  {-10,  10}, // 6. Bottom-Left (7:30)
  {-14,   0}, // 7. Left (9 o'clock)
  {-10, -10}  // 8. Top-Left (10:30)
};

void drawWifiFillProgressScreen(int filledCount) {
  matrix.fillScreen(COLOR_BLACK);

  // Wi-Fi Icon centered at (28, 24)
  drawWifiIcon(28, 24, COLOR_CYAN);

  int cx = 31;
  int cy = 27;

  // Draw 8 dots around the Wi-Fi icon, filling up clock-wise
  for (int i = 0; i < 8; i++) {
    int dx = (int8_t)pgm_read_byte(&spinnerDots[i][0]);
    int dy = (int8_t)pgm_read_byte(&spinnerDots[i][1]);

    if (i < filledCount) {
      // Filled dot (glowing yellow)
      matrix.fillRect(cx + dx - 1, cy + dy - 1, 2, 2, COLOR_YELLOW);
    } else {
      // Empty dot (subtle gray)
      matrix.drawPixel(cx + dx, cy + dy, COLOR_GRAY);
    }
  }

  matrix.show();
}

// Date calculation from Unix Epoch
void epochToDate(unsigned long epoch, int &year, int &month, int &day, int &wday) {
  unsigned long days = epoch / 86400L;
  wday = (days + 4) % 7; // 0=Su, 1=Mo, 2=Tu, 3=We, 4=Th, 5=Fr, 6=Sa

  long d = days - 10957; // Days since 2000-01-01
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
// PixelDust Sand Simulation Initialization & Frame Rendering
// ----------------------------------------------------------------------
void initPixelDust() {
  if (pixelDustInitialized) return;
  if (!sand.begin()) {
    Serial.println(F("[PixelDust] Error starting sand simulation!"));
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
  Serial.println(F("[PixelDust] Simulation initialized successfully!"));
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

// Hardware Button Handler (UP -> Status Screen, DOWN -> PixelDust Sand Demo)
void handleButtons() {
  if (millis() - lastButtonCheck < 50) return;
  lastButtonCheck = millis();

  bool currentUpState   = digitalRead(BUTTON_UP_PIN);
  bool currentDownState = digitalRead(BUTTON_DOWN_PIN);

  if (lastUpState == HIGH && currentUpState == LOW) {
    if (currentMode == MODE_STATUS) {
      currentMode = MODE_NORMAL;
      Serial.println(F("\n[Button UP] -> Normal Screen"));
    } else {
      currentMode = MODE_STATUS;
      Serial.println(F("\n[Button UP] -> Status Screen"));
    }
  }

  if (lastDownState == HIGH && currentDownState == LOW) {
    if (currentMode == MODE_PIXELDUST) {
      currentMode = MODE_NORMAL;
      Serial.println(F("\n[Button DOWN] -> Normal Screen"));
    } else {
      currentMode = MODE_PIXELDUST;
      initPixelDust();
      Serial.println(F("\n[Button DOWN] -> PixelDust Demo"));
    }
  }

  lastUpState   = currentUpState;
  lastDownState = currentDownState;
}

// ----------------------------------------------------------------------
// Captive Portal DNS Server (Intercepts DNS queries in AP mode)
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
  packetBuffer[replyLen++] = 0x00; // TTL (60 sec)
  packetBuffer[replyLen++] = 0x00;
  packetBuffer[replyLen++] = 0x00;
  packetBuffer[replyLen++] = 0x3C;
  packetBuffer[replyLen++] = 0x00; // RDLENGTH = 4 bytes
  packetBuffer[replyLen++] = 0x04;
  packetBuffer[replyLen++] = 192;  // IP: 192.168.4.1
  packetBuffer[replyLen++] = 168;
  packetBuffer[replyLen++] = 4;
  packetBuffer[replyLen++] = 1;

  dnsUdp.beginPacket(dnsUdp.remoteIP(), dnsUdp.remotePort());
  dnsUdp.write(packetBuffer, replyLen);
  dnsUdp.endPacket();

  logDebug(String(F("[DNS] Redirected to 192.168.4.1 for client")));
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
// Flash Storage Read/Write Functions (SAMD51 EEPROM Emulation)
// ----------------------------------------------------------------------
void loadConfig() {
  EEPROM.get(0, config);
  if (config.magic != CONFIG_MAGIC) {
    Serial.println(F("[Config] Invalid or new EEPROM. Loading default values..."));
    config = defaultConfig;
    EEPROM.put(0, config);
    EEPROM.commit();
  } else {
    Serial.println(F("[Config] Configuration loaded successfully."));
  }
}

void saveConfig() {
  config.magic = CONFIG_MAGIC;
  EEPROM.put(0, config);
  EEPROM.commit();
  Serial.println(F("[Config] Configuration saved to Flash storage."));
}

void showStatus() {
  Serial.println(F("\n=================================================="));
  Serial.println(F(" HOMENODEMATRIX CONFIG & STATUS"));
  Serial.println(F("=================================================="));
  Serial.print(F(" Mode:          ")); Serial.println(isAPMode ? "ACCESS POINT (Setup)" : "WLAN CLIENT");
  Serial.print(F(" Display Mode:  ")); 
  if (currentMode == MODE_PIXELDUST) Serial.println("PIXEL DUST DEMO");
  else if (currentMode == MODE_STATUS) Serial.println("STATUS SCREEN");
  else Serial.println("CLOCK & ENERGY");
  Serial.print(F(" Language:      ")); Serial.println(config.lang == 0 ? "DEUTSCH (German)" : "ENGLISH");
  Serial.print(F(" Wi-Fi Status:  ")); Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
  Serial.print(F(" Debug Log:     ")); Serial.println(debugMode ? "ON" : "OFF");
  Serial.print(F(" Wi-Fi SSID:    '")); Serial.print(config.wifi_ssid); Serial.println(F("'"));
  Serial.print(F(" Wi-Fi Pass:    '")); Serial.print(config.wifi_pass); Serial.println(F("'"));
  Serial.print(F(" Shelly IP:     '")); Serial.print(config.shelly_ip); Serial.println(F("'"));
  Serial.print(F(" Shelly Path:   '")); Serial.print(config.shelly_path); Serial.println(F("'"));
  Serial.print(F(" Inverter IP:   '")); Serial.print(config.inverter_ip); Serial.println(F("'"));
  Serial.print(F(" Inverter Path: '")); Serial.print(config.inverter_path); Serial.println(F("'"));
  Serial.print(F(" UTC Offset:    ")); Serial.print(config.utc_offset_sec); Serial.println(F(" sec"));
  Serial.print(F(" Brightness:    ")); Serial.println(config.brightness);
  Serial.print(F(" IP Address:    ")); Serial.println(isAPMode ? apIPAddress : WiFi.localIP());
  Serial.println(F("==================================================\n"));
}

void printHelp() {
  Serial.println(F("\n--- HomeNodeMatrix Serial CLI Commands ---"));
  Serial.println(F(" status                - Print current configuration & status"));
  Serial.println(F(" mode normal/status/dust - Switch active display mode"));
  Serial.println(F(" lang de / lang en     - Switch system language (German / English)"));
  Serial.println(F(" brightness <10-255>   - Adjust display brightness (e.g. brightness 100)"));
  Serial.println(F(" debug on / debug off  - Enable or disable background live debug logs"));
  Serial.println(F(" wifi <ssid> <pass>    - Set Wi-Fi network credentials"));
  Serial.println(F(" shelly <ip> [path]    - Set Shelly 3Pro IP address and API path"));
  Serial.println(F(" inverter <ip> [path]  - Set Fronius inverter IP address and API path"));
  Serial.println(F(" utc <offset>          - Set GMT offset in seconds (e.g. 7200 for CEST)"));
  Serial.println(F(" save                  - Save settings to Flash memory"));
  Serial.println(F(" reset                 - Restore factory default settings"));
  Serial.println(F(" reboot                - Perform hardware system reset"));
  Serial.println(F(" help / ?              - Show this help menu\n"));
  Serial.print(F("CLI> "));
}

// ----------------------------------------------------------------------
// Serial Command Line Interface (CLI) Handler
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
        } else if (serialBuffer.equals("lang de")) {
          config.lang = 0;
          saveConfig();
          Serial.println(F("[CLI] Sprache geändert zu: DEUTSCH"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("lang en")) {
          config.lang = 1;
          saveConfig();
          Serial.println(F("[CLI] Language changed to: ENGLISH"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("mode status")) {
          currentMode = MODE_STATUS;
          Serial.println(F("[CLI] Status screen ENABLED!"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("mode dust") || serialBuffer.equals("pixeldust")) {
          currentMode = MODE_PIXELDUST;
          initPixelDust();
          Serial.println(F("[CLI] PixelDust sand demo ENABLED!"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("mode normal")) {
          currentMode = MODE_NORMAL;
          Serial.println(F("[CLI] Main display (Clock & Energy) ENABLED!"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("debug on") || serialBuffer.equals("debug 1")) {
          debugMode = true;
          Serial.println(F("[CLI] Live debug logging ENABLED (on)"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("debug off") || serialBuffer.equals("debug 0")) {
          debugMode = false;
          Serial.println(F("[CLI] Live debug logging DISABLED (off) - CLI stays clean!"));
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
            Serial.println(F("[CLI] Wi-Fi credentials updated & saved!"));
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
            Serial.println(F("[CLI] Shelly settings updated & saved!"));
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
            Serial.println(F("[CLI] Inverter settings updated & saved!"));
          }
          Serial.print(F("CLI> "));
        } else if (serialBuffer.startsWith("utc ")) {
          config.utc_offset_sec = serialBuffer.substring(4).toInt();
          saveConfig();
          Serial.println(F("[CLI] UTC offset updated!"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.startsWith("brightness ") || serialBuffer.startsWith("b ")) {
          int spacePos = serialBuffer.indexOf(' ');
          int val = serialBuffer.substring(spacePos + 1).toInt();
          applyBrightness(val);
          saveConfig();
          Serial.println(String(F("[CLI] Display brightness set to: ")) + config.brightness);
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("save")) {
          saveConfig();
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("reset")) {
          config = defaultConfig;
          applyBrightness(config.brightness);
          saveConfig();
          Serial.println(F("[CLI] Reset configuration to factory defaults!"));
          Serial.print(F("CLI> "));
        } else if (serialBuffer.equals("reboot")) {
          Serial.println(F("[CLI] Rebooting system..."));
          delay(500);
          NVIC_SystemReset();
        } else {
          Serial.println(F("Unknown command. Type 'help' for command summary."));
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
// HTML Configuration Web Page (Dark Theme) & Webserver Handler
// ----------------------------------------------------------------------
void handleWebClient() {
  WiFiClient client = webServer.available();
  if (!client) return;

  logDebug(F("\n>>> [Web] Client connected to web server!"));

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

  logDebug(String(F("[Web] Request line: ")) + firstLine);

  // POST Request (Process form submission & save)
  if (isPost && requestBody.length() > 0) {
    logDebug(F("[Web] POST body received:"));
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
        else if (key == "lang") config.lang = val.toInt();
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
    client.println("<div class='card'><h2>Settings Saved! / Einstellungen gespeichert!</h2>");
    client.println("<p>MatrixPortal is rebooting...</p>");
    client.println("<p>Please wait 10 seconds.</p></div></body></html>");
    client.flush();
    delay(10);
    client.stop();

    Serial.println(F("[Web] Settings saved. Executing hardware reset..."));
    delay(1000);
    NVIC_SystemReset();
    return;
  }

  // Captive Portal Auto-Popup Redirect
  if (isAPMode && firstLine.indexOf("192.168.4.1") == -1 && firstLine.indexOf("GET / ") == -1) {
    logDebug(F("[Web] Captive Portal Redirect to http://192.168.4.1/"));
    client.println("HTTP/1.1 302 Found");
    client.println("Location: http://192.168.4.1/");
    client.println("Connection: close");
    client.println();
    client.flush();
    delay(10);
    client.stop();
    return;
  }

  // GET Request (Render HTML Configuration Form)
  logDebug(F("[Web] Sending configuration web page..."));

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
  client.println("input[type=text],input[type=password],input[type=number],select{width:100%;padding:10px;border-radius:6px;border:1px solid #444;background:#333;color:#fff;box-sizing:border-box;font-size:14px;}");
  client.println("input:focus,select:focus{border-color:#00e676;outline:none;}");
  client.println("button{width:100%;padding:14px;background:#00c853;border:none;border-radius:8px;color:#fff;font-size:16px;font-weight:bold;cursor:pointer;margin-top:10px;}");
  client.println("button:hover{background:#00e676;}");
  client.println(".info{font-size:11px;color:#888;text-align:center;margin-top:12px;}");
  client.println("</style></head><body>");
  client.println("<div class='container'><h1>HomeNodeMatrix Setup</h1>");
  client.println("<form method='POST' action='/save'>");

  client.println("<div class='section'><h2>Wi-Fi / WLAN Settings</h2>");
  client.println("<label>Wi-Fi Name (SSID)</label>");
  client.print("<input type='text' name='ssid' value='"); client.print(config.wifi_ssid); client.println("' required>");
  client.println("<label>Wi-Fi Password</label>");
  client.print("<input type='password' name='pass' value='"); client.print(config.wifi_pass); client.println("'>");
  client.println("</div>");

  client.println("<div class='section'><h2>Shelly 3Pro (Grid / Netz)</h2>");
  client.println("<label>Shelly IP Address</label>");
  client.print("<input type='text' name='shelly_ip' value='"); client.print(config.shelly_ip); client.println("' required>");
  client.println("<label>API Path</label>");
  client.print("<input type='text' name='shelly_path' value='"); client.print(config.shelly_path); client.println("'>");
  client.println("</div>");

  client.println("<div class='section'><h2>Solar Inverter (Fronius / OpenDTU)</h2>");
  client.println("<label>Inverter IP Address</label>");
  client.print("<input type='text' name='inverter_ip' value='"); client.print(config.inverter_ip); client.println("' required>");
  client.println("<label>API Path</label>");
  client.print("<input type='text' name='inverter_path' value='"); client.print(config.inverter_path); client.println("'>");
  client.println("</div>");

  client.println("<div class='section'><h2>System & Display</h2>");
  client.println("<label>Language / Sprache</label>");
  client.println("<select name='lang'>");
  client.print("<option value='0'"); if(config.lang == 0) client.print(" selected"); client.println(">Deutsch (German)</option>");
  client.print("<option value='1'"); if(config.lang == 1) client.print(" selected"); client.println(">English</option>");
  client.println("</select>");

  client.println("<label>UTC Offset (seconds, e.g. 7200 = CEST)</label>");
  client.print("<input type='number' name='utc_offset' value='"); client.print(config.utc_offset_sec); client.println("'>");
  client.println("<label>Brightness / Helligkeit (10 - 255)</label>");
  client.print("<input type='number' name='brightness' min='10' max='255' value='"); client.print(config.brightness); client.println("'>");
  client.println("</div>");

  client.println("<button type='submit'>Save & Reboot / Speichern & Neustart</button>");
  client.println("</form>");
  client.println("<div class='info'>MatrixPortal M4 Smart Energy Display</div>");
  client.println("</div></body></html>");

  client.flush();
  delay(10);
  client.stop();

  logDebug(F("<<< [Web] Response sent completely. Client disconnected."));
}

// ----------------------------------------------------------------------
// NTP Time Query via UDP (with Timeout)
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
// Query Shelly 3Pro Meter (Gen2 HTTP API)
// ----------------------------------------------------------------------
void updateShellyData() {
  if (strlen(config.shelly_ip) == 0 || strcmp(config.shelly_ip, "0.0.0.0") == 0) {
    gridOk = false;
    return;
  }

  WiFiClient client;
  client.setTimeout(1000);
  if (!client.connect(config.shelly_ip, 80)) {
    logDebug(F("[Netz] Shelly 3Pro unreachable. Waiting..."));
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
    logDebug("[Netz] Current grid power (Shelly): " + String(gridPowerW) + " W");
  } else {
    logDebug("[Netz] Shelly JSON error: " + String(err.c_str()));
    gridOk = false;
  }
}

// ----------------------------------------------------------------------
// Query Solar Inverter (Fronius Solar API v1 & OpenDTU Parser)
// ----------------------------------------------------------------------
void updateInverterData() {
  if (strlen(config.inverter_ip) == 0 || strcmp(config.inverter_ip, "0.0.0.0") == 0) {
    solarOk = false;
    return;
  }

  WiFiClient client;
  client.setTimeout(1000);
  if (!client.connect(config.inverter_ip, 80)) {
    logDebug(F("[Solar] Fronius / Inverter unreachable. Waiting..."));
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

    logDebug("[Solar] Current solar power (Fronius): " + String(solarPowerW) + " W");
  } else {
    logDebug("[Solar] Inverter JSON error: " + String(err.c_str()));
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

// Access Point Setup Screen with Wi-Fi Symbol (Ohne klobigen Text)
void drawAPScreen() {
  matrix.fillScreen(COLOR_BLACK);
  matrix.setTextWrap(false);
  matrix.setTextSize(1);

  // Wi-Fi Symbol centered at top
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

// Dedicated Status Screen (UP Button) with ultra-compact 3x5 Pixel Font for IP addresses
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

  // 1. Matrix IP Address
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

// Main Normal Screen: Time, Date & Energy (Clean, no bars)
void drawNormalScreen() {
  matrix.fillScreen(COLOR_BLACK);
  matrix.setTextWrap(false);

  unsigned long nowEpoch = localUnixTime + ((millis() - lastTimeSyncMs) / 1000);
  int hours   = (nowEpoch % 86400L) / 3600;
  int minutes = (nowEpoch % 3600) / 60;
  int seconds = (nowEpoch % 60);

  int year, month, day, wday;
  epochToDate(nowEpoch, year, month, day, wday);

  // Dynamic Weekday Arrays for German / English (2-letter format prevents >64px overflow)
  const char* wdaysDE[] = {"So.", "Mo.", "Di.", "Mi.", "Do.", "Fr.", "Sa."};
  const char* wdaysEN[] = {"Su.", "Mo.", "Tu.", "We.", "Th.", "Fr.", "Sa."};
  const char* dayStr    = (config.lang == 0) ? wdaysDE[wday] : wdaysEN[wday];

  // 1. Time (Cyan, y=2) & Wi-Fi Icon (top right, y=2, x=54)
  matrix.setTextColor(COLOR_CYAN);
  matrix.setTextSize(1);
  char timeBuf[10];
  sprintf(timeBuf, "%02d:%02d:%02d", hours, minutes, seconds);
  matrix.setCursor(2, 2);
  matrix.print(timeBuf);

  // Wi-Fi Symbol (Green = Connected, Red = Disconnected)
  drawWifiIcon(54, 2, WiFi.status() == WL_CONNECTED ? COLOR_GREEN : COLOR_RED);

  // 2. Date & Weekday (White, y=11) -> "Mo. 10.8.26" / "Mo. 10.12.26"
  matrix.setTextColor(COLOR_WHITE);
  char dateBuf[16];
  sprintf(dateBuf, "%s %d.%d.%02d", dayStr, day, month, year % 100);
  
  int textWidth = strlen(dateBuf) * 6;
  int xPos = (64 - textWidth) / 2;
  if (xPos < 0) xPos = 0;
  matrix.setCursor(xPos, 11);
  matrix.print(dateBuf);

  matrix.drawFastHLine(0, 20, 64, COLOR_GRAY);

  // 3. Solar Power (y=23..33)
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

  // 4. Grid Power (y=46..55) -> "NETZ" (DE) / "GRID" (EN)
  matrix.setCursor(2, 46);
  matrix.setTextColor(COLOR_CYAN);
  matrix.print((config.lang == 0) ? F("NETZ") : F("GRID"));

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

  // Apply dynamic color brightness (10-255)
  updateColors();

  if (accel.begin(0x19)) {
    accel.setRange(LIS3DH_RANGE_4_G);
    accelOk = true;
    Serial.println(F("[Sensor] LIS3DH Accelerometer (PixelDust) ready."));
  } else {
    Serial.println(F("[Sensor] LIS3DH not found (Software gravity fallback active)."));
  }

  if (strlen(config.wifi_ssid) == 0) {
    Serial.println(F("[WLAN] No SSID saved. Starting Access Point Mode..."));
    isAPMode = true;
  } else {
    Serial.print(F("[WLAN] Connecting to SSID: '"));
    Serial.print(config.wifi_ssid);
    Serial.println(F("'"));

    WiFi.begin(config.wifi_ssid, config.wifi_pass);

    // 8 fill-up steps around the Wi-Fi icon (no text)
    for (int step = 0; step <= 8; step++) {
      drawWifiFillProgressScreen(step);
      if (WiFi.status() == WL_CONNECTED) break;
      delay(450);
      Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F("\n[WLAN] Connection failed! Switching to Access Point Mode..."));
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
    Serial.println(F(" ACCESS POINT & CAPTIVE PORTAL STARTED"));
    Serial.print(F(" SSID:     ")); Serial.println(AP_SSID);
    Serial.print(F(" PASS/PIN: ")); Serial.println(ap_password);
    Serial.print(F(" AP IP:    ")); Serial.println(apIPAddress);
    Serial.println(F(" Press UP for Status screen, DOWN for PixelDust Sand Demo!"));
    Serial.println(F(" Type 'help' on Serial Console for CLI commands"));
    Serial.println(F("==================================================\n"));

    drawAPScreen();
  } else {
    Serial.println(F("\n[WLAN] Wi-Fi Connected Successfully!"));
    Serial.print(F("[WLAN] IP Address: "));
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

  Serial.println(F("[Web] HTTP Web server started on port 80!"));
  printHelp();
}

// ----------------------------------------------------------------------
// MAIN LOOP
// ----------------------------------------------------------------------
void loop() {
  // 1. Process Hardware Buttons (UP -> Status, DOWN -> PixelDust)
  handleButtons();

  // 2. Process Serial CLI
  handleSerialCLI();

  // 3. Render active mode if PixelDust is active
  if (currentMode == MODE_PIXELDUST) {
    runPixelDustFrame();
    return;
  }

  // 4. AP Mode Handler
  if (isAPMode) {
    if (apStatus != WiFi.status()) {
      apStatus = WiFi.status();
      if (apStatus == WL_AP_CONNECTED) {
        logDebug(F("[WLAN] Client connected to hotspot!"));
      }
    }
    processDNS();
    handleWebClient();
    delay(10);
    return;
  }

  // 5. Process Web Server Requests
  handleWebClient();

  unsigned long currentMs = millis();

  // 6. Display Redraw according to active mode
  if (currentMs - lastDisplayRedraw >= 100) {
    if (currentMode == MODE_STATUS) {
      drawStatusScreen();
    } else {
      drawNormalScreen();
    }
    lastDisplayRedraw = currentMs;
  }

  // 7. NTP Time Sync (hourly)
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

  // 8. Query Shelly 3Pro
  unsigned long shellyInterval = gridOk ? POLL_ONLINE_MS : RETRY_OFFLINE_MS;
  if (currentMs - lastShellyAttempt >= shellyInterval) {
    if (WiFi.status() == WL_CONNECTED) {
      updateShellyData();
    }
    lastShellyAttempt = currentMs;
  }

  // 9. Query Solar Inverter
  unsigned long inverterInterval = solarOk ? POLL_ONLINE_MS : RETRY_OFFLINE_MS;
  if (currentMs - lastInverterAttempt >= inverterInterval) {
    if (WiFi.status() == WL_CONNECTED) {
      updateInverterData();
    }
    lastInverterAttempt = currentMs;
  }

  delay(10);
}
