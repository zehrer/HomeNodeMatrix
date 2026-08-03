# HomeNodeMatrix ⚡ MatrixPortal M4

**HomeNodeMatrix** is a smart energy and time display built for the **Adafruit MatrixPortal M4** connected to a **64x64 RGB LED Matrix Display**.

It fetches real-time solar generation data from your photovoltaic system (**Fronius Inverter**) as well as grid power consumption/feed-in from your smart meter (**Shelly 3Pro / Pro 3EM**), displaying them alongside synchronized clock and date information.

---

## 🌟 Features

- **Time & Date Display**: Precise NTP clock synchronization and dynamic weekday calculation (`Mo. 3.8.26`).
- **Solar Generation (`SOLAR`)**: Real-time PV power readings using the **Fronius Solar API v1** (`/solar_api/v1/GetPowerFlowRealtimeData.fcgi`).
- **Grid Power (`NETZ`)**: Live grid import and export readings using the **Shelly Gen2 HTTP API** (`/rpc/Shelly.GetStatus`).
- **Wi-Fi Access Point & Captive Portal**:
  - Automatic setup hotspot (`HomeNodeMatrix-Setup`) if no saved Wi-Fi is reachable.
  - Displays a random **8-digit PIN** directly on the 64x64 LED matrix.
  - Automatic DNS Captive Portal redirect to `http://192.168.4.1/`.
- **Dark Mode Web Configuration Page**: Easily configure Wi-Fi credentials, IP addresses, API endpoints, UTC offset, and display brightness via any web browser.
- **Serial Command Line Interface (CLI)**:
  - Full-featured interactive serial console over USB (compatible with Chrome Web Serial Terminal).
  - Commands: `status`, `debug on/off`, `mode normal/status/dust`, `wifi`, `shelly`, `inverter`, `brightness`, `save`, `reset`, `reboot`.
  - Mutable background debug logging (`debug off`) to keep the CLI prompt pristine while typing.
- **On-Board Hardware Button Controls**:
  - **UP Button**: Toggles the **Status Screen** (showing Matrix Portal IP, Shelly IP, and Fronius IP using an ultra-compact 3x5 pixel font).
  - **DOWN Button**: Activates the **PixelDust Sand Physics Demo** (uses the on-board LIS3DH accelerometer for real-time tilt/gravity sand particle simulation).

---

## 🛠 Hardware & Requirements

- **Board**: Adafruit MatrixPortal M4 (ATSAMD51 ARM Cortex-M4 + ESP32 AirLift Wi-Fi co-processor)
- **Display**: 64x64 RGB LED Matrix (5 Address Pins)
- **Grid Power Meter**: Shelly 3Pro / Pro 3EM (Default IP: `192.168.178.154`)
- **PV Inverter**: Fronius Symo / Primo / Gen24 with Datamanager 2.0 or Solar API v1 (Default IP: `192.168.178.24`)

---

## 🚀 Building & Flashing

Using `arduino-cli`:

```bash
# Compile
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4 .

# Upload
arduino-cli upload -p /dev/cu.usbmodem1101 --fqbn adafruit:samd:adafruit_matrixportal_m4 .
```

---

## 💻 Serial CLI Reference

Connect at `115200` baud (e.g. via [Chrome Serial Terminal](https://googlechromelabs.github.io/serial-terminal/)):

| Command | Description |
| :--- | :--- |
| `status` | Display current network status and configuration |
| `debug on` / `debug off` | Enable or disable background live debug log output |
| `mode normal` / `status` / `dust` | Switch display mode (Clock/Energy, Status Info, PixelDust) |
| `wifi <ssid> <password>` | Configure Wi-Fi credentials |
| `shelly <ip> [path]` | Configure Shelly 3Pro IP address and API path |
| `inverter <ip> [path]` | Configure Fronius inverter IP address and API path |
| `utc <offset_in_sec>` | Set GMT offset in seconds (e.g. `7200` for CEST) |
| `brightness <10-255>` | Adjust LED matrix brightness (e.g. `brightness 100`) |
| `save` | Persist settings to Flash storage |
| `reset` | Restore default factory settings |
| `reboot` | Perform a hardware reset |

---

## 📜 License

Distributed under the MIT License.
