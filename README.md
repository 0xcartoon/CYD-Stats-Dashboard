# CYD Dashboard

<p align="center">
  <img src="https://ssd-cdn.nest.rip/uploads/021b34bb-d555-409c-87b3-7a00070a8921.png" width="260">
  <img src="https://ssd-cdn.nest.rip/uploads/3aa80b32-6520-4ee8-96be-565f51361bab.png" width="260">
  <img src="https://ssd-cdn.nest.rip/uploads/5c82ed1c-2aef-4def-9a31-f192264a2cb7.png" width="260">
</p>

A full-featured system monitor dashboard for the **ESP32-2432S028** (Cheap Yellow Display). Pulls live stats from your PC over WiFi and displays them across 6 touch-navigable pages with animated UI, Spotify controls, Telegram notifications, and a settings menu.

---

## Pages

| Page | What it shows |
|---|---|
| **SYSTEM** | CPU %, RAM usage, temperature, uptime |
| **SPOTIFY** | Now playing track/artist, playback controls, live progress bar |
| **STORAGE** | Disk usage % and GB used/total |
| **NETWORK** | Download/upload speeds, WiFi RSSI, local IP |
| **CPU GRAPH** | Scrolling 80-point CPU history graph |
| **SETTINGS** | Brightness, sleep timer, back LED, accent color theme |

---

## Hardware

- **ESP32-2432S028** (CYD — "Cheap Yellow Display")
  - 320×240 ILI9341 TFT, landscape, inverted
  - XPT2046 resistive touchscreen
  - RGB back LED (active LOW, GPIO 4/16/17)
  - BOOT button on GPIO 0
  - Backlight PWM on GPIO 21

Other CYD variants may work but touch calibration and LED pins may differ.

---

## Dependencies

Install all of these via the Arduino Library Manager before compiling:

- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — display driver
- [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen) — touch driver
- ArduinoOTA (bundled with ESP32 Arduino core)
- Preferences (bundled with ESP32 Arduino core)

**TFT_eSPI requires a `User_Setup.h` configured for the ILI9341 + CYD pinout.** Use the setup file for the ESP32-2432S028 — many community presets exist. The library won't compile correctly without this.

---

## Server

The dashboard polls a local HTTP server for stats JSON. A minimal Python server is included in the `server/` folder.

```
server/
  stats_server.py    # FastAPI/Flask server — runs on your PC
  requirements.txt
```

The server exposes:

```
GET /stats.json          → system + Spotify stats
POST /spotify/toggle     → play/pause
POST /spotify/next       → skip forward
POST /spotify/previous   → skip back
```

### Running the server

```bash
cd server
pip install -r requirements.txt
python stats_server.py
```

The server binds to port `5050` by default. Make sure your PC and CYD are on the same network.

### Stats JSON format

The server must return a JSON object with at least these fields:

```json
{
  "cpu": 12.4,
  "ram": 55.2,
  "ram_used_gb": 8.8,
  "ram_total_gb": 16.0,
  "temp": 48.0,
  "uptime": 86400,
  "disk": 62.0,
  "disk_used_gb": 248.0,
  "disk_total_gb": 400.0,
  "net_down_kb": 340.5,
  "net_up_kb": 12.1,
  "spotify_ok": true,
  "spotify_playing": true,
  "spotify_track": "Track Name",
  "spotify_artist": "Artist Name",
  "spotify_album": "Album Name",
  "spotify_progress_ms": 95000,
  "spotify_duration_ms": 210000
}
```

Spotify and alert fields are optional — the dashboard handles missing or zeroed values gracefully.

---

## Setup

### 1. Clone the repo

```bash
git clone https://github.com/0xcartoon/CYD-Stats-Dashboard
```

### 2. Edit credentials

Open `cyd_dashboard.ino` and fill in the **USER CONFIG** section at the top:

```cpp
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

const char* statsURL       = "http://192.168.1.100:5050/stats.json";
const char* commandBaseURL = "http://192.168.1.100:5050";

const char* otaHostname = "cyd-dashboard";
const char* otaPassword = "YOUR_OTA_PASSWORD";
```

Replace `192.168.1.100` with the LAN IP of the machine running the stats server.

### 3. Configure TFT_eSPI

Copy the correct `User_Setup.h` for your CYD board into the TFT_eSPI library folder. Typical CYD pins:

| Signal | GPIO |
|---|---|
| TFT_MOSI | 13 |
| TFT_MISO | 12 |
| TFT_SCLK | 14 |
| TFT_CS | 15 |
| TFT_DC | 2 |
| TFT_RST | -1 |
| TFT_BL | 21 |

### 4. Flash

Select board **ESP32 Dev Module**, then upload over USB. After the first flash, OTA becomes available — the board will show up as `cyd-dashboard` in the Arduino IDE network port list.

---

## Touch Controls

| Gesture | Action |
|---|---|
| Tap left edge (hold ~0.5s) | Previous page |
| Tap right edge (hold ~0.5s) | Next page |
| Swipe left/right | Previous/next page |
| Swipe down from top | Sleep |
| BOOT button | Next page |
| Touch anywhere (while sleeping) | Wake |
| Spotify page — tap buttons | Previous / Play-Pause / Next |
| Spotify page — long press middle | Force refresh |

---

## Settings

Accessible on the last page. All settings persist in flash via `Preferences`.

- **Brightness** — 5 steps from dim to full
- **Sleep timer** — Off, 1 min, 5 min, 1 hour, 3 hours
- **Back LED** — toggle the RGB back LED on/off
- **Accent color** — Cyan, Purple, Green, Amber

---

## OTA Updates

After the first USB flash, you can upload new firmware wirelessly:

1. Make sure the board is on and connected to WiFi
2. In Arduino IDE, go to **Tools → Port** and select the network port named `cyd-dashboard`
3. Click Upload as normal

The OTA password is set in `otaPassword` in the sketch.

---

## Touch Calibration

If touch is off or mirrored, adjust these constants at the top of the sketch:

```cpp
#define TOUCH_MIN_X 250
#define TOUCH_MAX_X 3800
#define TOUCH_MIN_Y 250
#define TOUCH_MAX_Y 3800

#define TOUCH_SWAP_XY   false
#define TOUCH_INVERT_X  false
#define TOUCH_INVERT_Y  false
```

Print raw touch values from `touch.getPoint()` over Serial to dial in the min/max range for your specific board.

---

## License

MIT — do whatever, attribution appreciated.
