// CYD Dashboard v17
// ESP32-2432S028 (Cheap Yellow Display) all-in-one system monitor
// Pages: System | Spotify | Storage | Network | CPU Graph | Settings
// Touch controls, OTA updates, sleep timer, RGB back LED, themed UI
//
// Setup: fill in your WiFi, server IP, and OTA password below, then flash.

#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <ArduinoOTA.h>

// ============================================================
// USER CONFIG — edit these before flashing
// ============================================================
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// IP and port of your stats server (see server/stats_server.py)
const char* statsURL       = "http://192.168.1.100:5050/stats.json";
const char* commandBaseURL = "http://192.168.1.100:5050";

// OTA: flash wirelessly after first USB upload.
// Board shows up as "cyd-dashboard" in Arduino IDE network ports.
const char* otaHostname = "cyd-dashboard";
const char* otaPassword = "YOUR_OTA_PASSWORD";
// ============================================================

#define TFT_BACKLIGHT 21
#define BOOT_BTN 0

// CYD RGB back LEDs — active LOW on most ESP32-2432S028 boards.
// If yours doesn't respond, try pins 4/16/17 or check your board variant.
#define BACK_LED_R_PIN 4
#define BACK_LED_G_PIN 16
#define BACK_LED_B_PIN 17
#define BACK_LED_ACTIVE_LOW true

#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25
#define TOUCH_CS   33

#define TOUCH_DEBOUNCE_MS 260
#define PAGE_HOLD_MS      550

#define TOUCH_MIN_X 250
#define TOUCH_MAX_X 3800
#define TOUCH_MIN_Y 250
#define TOUCH_MAX_Y 3800

// Flip these if touch is mirrored or axes are swapped on your board.
#define TOUCH_SWAP_XY   false
#define TOUCH_INVERT_X  false
#define TOUCH_INVERT_Y  false

#define FETCH_INTERVAL_MS           2500
#define ANIM_INTERVAL_MS            800
#define SPOTIFY_PROGRESS_INTERVAL_MS 3000
#define API_FAILS_BEFORE_LOST       5
#define CPU_HISTORY_SIZE            80

#define SETTINGS_TAP_DEBOUNCE_MS 900

#define SWIPE_SLEEP_START_Y  45
#define SWIPE_SLEEP_END_Y    175
#define SWIPE_SLEEP_MIN_DIST 110

#define FAST_TAP_MAX_MS 220

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSPI = SPIClass(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
Preferences prefs;
bool otaReady = false;

uint16_t PANEL;
uint16_t PANEL_DARK;
uint16_t CYAN_DARK;

float cpu = 0, ram = 0, temp = 0;
float ramUsedGB = 0, ramTotalGB = 0;
float disk = 0, diskUsedGB = 0, diskTotalGB = 0;
float netDownKB = 0, netUpKB = 0;
int uptimeSeconds = 0;

bool spotifyOK = false;
bool spotifyPlaying = false;
String spotifyTrack = "";
String spotifyArtist = "";
String spotifyAlbum = "";

int spotifyProgressMs = 0;
int spotifyDurationMs = 0;

bool wifiOK = false;
bool serverOK = false;
String lastError = "BOOT";

String lastSpotifySeed = "";
String lastSpotifyTrack = "";
String lastSpotifyArtist = "";
bool lastSpotifyOK = false;
bool lastSpotifyPlaying = false;

int page = 0;
int lastPage = -1;

bool lastBtn = HIGH;
bool touchWasDown = false;
bool touchPageTriggered = false;

int touchStartX = 0;
int touchStartY = 0;
int touchRawStartX = 0;
int touchRawStartY = 0;
int touchLastX = 0;
int touchLastY = 0;

unsigned long lastBtnTime = 0;
unsigned long lastTouchTime = 0;
unsigned long touchDownTime = 0;
unsigned long lastFetch = 0;
unsigned long lastAnim = 0;
unsigned long lastWiFiTry = 0;
unsigned long spotifyUpdatedAt = 0;
unsigned long lastSpotifyProgressDraw = 0;
unsigned long lastActivity = 0;
unsigned long lastSettingsTap = 0;
unsigned long restoreButtonsAt = 0;
unsigned long pendingSpotifyRefreshAt = 0;
unsigned long skipFetchUntil = 0;
unsigned long nextSpotifyFastRefreshAt = 0;
int spotifyFastRefreshCount = 0;

int apiFailCount = 0;
bool spotifyCommandBusy = false;
bool sleeping = false;
bool backlightPwmAttached = false;

float cpuHistory[CPU_HISTORY_SIZE];
int cpuIndex = 0;
int cpuCount = 0;

uint8_t pixelPhase = 0;

bool screenShellReady = false;

// SETTINGS
int brightness = 180;
bool backLedOn = true;
int sleepModeIndex = 0;
int themeIndex = 0;

unsigned long sleepTimeouts[] = {
  0UL, 60000UL, 300000UL, 3600000UL, 10800000UL
};

String sleepLabels[] = { "OFF", "1 MIN", "5 MIN", "1 HOUR", "3 HOURS" };
String themeLabels[] = { "CYAN", "PURPLE", "GREEN", "AMBER" };

enum SettingsMode {
  SETTINGS_MAIN, SETTINGS_BRIGHTNESS, SETTINGS_SLEEP, SETTINGS_LED, SETTINGS_THEME
};

SettingsMode settingsMode = SETTINGS_MAIN;
bool settingsForceRedraw = true;

// Forward declarations
void drawStaticPage();
void drawPageShell(String title);
void updatePageValues();
void drawCurrentPageValues();
void clearPanelArea(int x, int y, int w, int h);
void drawPlusStar(int x, int y, uint16_t color);
void enterSleepMode();
void drawSpotifyProgress(bool force = false);
void drawSettingsStatic();
void updateSettings();
void handleSettingsTouch(int x, int y);
void noteActivity();
void setupOTA();
void applyBrightness();
void applyBackLed();
void loadSettings();
void saveSettings();
void saveLastPage();
uint16_t accentColor();
uint16_t accentDarkColor();
void updateThemeSettings();
String currentPageTitle();
void header(String title);

static inline uint8_t lerpU8(uint8_t a, uint8_t b, float t) {
  return (uint8_t)(a + t * (float)(b - a));
}

uint16_t blend565(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(r, g, b);
}

uint16_t accentColor() {
  if (themeIndex == 1) return tft.color565(190, 105, 255);
  if (themeIndex == 2) return tft.color565(70, 255, 120);
  if (themeIndex == 3) return tft.color565(255, 175, 40);
  return TFT_CYAN;
}

uint16_t accentDarkColor() {
  if (themeIndex == 1) return tft.color565(100, 45, 150);
  if (themeIndex == 2) return tft.color565(25, 120, 65);
  if (themeIndex == 3) return tft.color565(135, 80, 15);
  return tft.color565(0, 80, 100);
}

uint16_t bgGradientColor(int y) {
  y = constrain(y, 0, 239);
  float t = y / 239.0f;
  t = t * t * (3.0f - 2.0f * t);

  uint8_t r = lerpU8(28, 50, t);
  uint8_t g = lerpU8(19, 34, t);
  uint8_t b = lerpU8(66, 98, t);

  if (y > 150) {
    float k = (y - 150) / 89.0f;
    r = constrain((int)r + (int)(4 * k), 0, 255);
    b = constrain((int)b + (int)(6 * k), 0, 255);
  }

  return tft.color565(r, g, b);
}

uint16_t panelBgColor(int y) {
  y = constrain(y, 0, 239);
  float t = y / 239.0f;
  t = t * t * (3.0f - 2.0f * t);

  uint8_t r = lerpU8(18, 31, t);
  uint8_t g = lerpU8(14, 25, t);
  uint8_t b = lerpU8(42, 68, t);

  return tft.color565(r, g, b);
}

String getValue(String data, String key) {
  int start = data.indexOf("\"" + key + "\":");
  if (start == -1) return "0";
  start = data.indexOf(":", start) + 1;
  int end = data.indexOf(",", start);
  if (end == -1) end = data.indexOf("}", start);
  return data.substring(start, end);
}

String getStringValue(String data, String key) {
  int start = data.indexOf("\"" + key + "\":");
  if (start == -1) return "";
  start = data.indexOf("\"", start + key.length() + 3);
  if (start == -1) return "";
  start++;
  String out = "";
  bool esc = false;
  for (int i = start; i < (int)data.length(); i++) {
    char c = data.charAt(i);
    if (esc) { out += c; esc = false; }
    else if (c == '\\') { esc = true; }
    else if (c == '"') { break; }
    else { out += c; }
  }
  return out;
}

bool getBoolValue(String data, String key) {
  String v = getValue(data, key);
  v.trim();
  return v == "true" || v == "1";
}

String clipText(String text, int maxChars) {
  if (text.length() <= (unsigned)maxChars) return text;
  return text.substring(0, maxChars - 1) + ".";
}

String uptimeText(int seconds) {
  int d = seconds / 86400;
  int h = (seconds % 86400) / 3600;
  int m = (seconds % 3600) / 60;
  char buf[24];
  if (d > 0) sprintf(buf, "%dd %02dh", d, h);
  else sprintf(buf, "%02dh %02dm", h, m);
  return String(buf);
}

String speedText(float kb) {
  if (kb > 1024) return String(kb / 1024.0, 2) + " MB/s";
  return String(kb, 1) + " KB/s";
}

String timeText(int ms) {
  if (ms <= 0) return "--:--";
  int seconds = ms / 1000;
  int m = seconds / 60;
  int s = seconds % 60;
  char buf[8];
  sprintf(buf, "%d:%02d", m, s);
  return String(buf);
}

int bestSpotifyNumber(String data, const char* a, const char* b, const char* c) {
  int v = getValue(data, String(a)).toInt();
  if (v > 0) return v;
  v = getValue(data, String(b)).toInt();
  if (v > 0) return v;
  return getValue(data, String(c)).toInt();
}

uint16_t tempColor(float t) {
  if (t >= 75) return TFT_RED;
  if (t >= 60) return TFT_ORANGE;
  return TFT_GREEN;
}

uint16_t diskColor(float d) {
  if (d >= 90) return TFT_RED;
  if (d >= 70) return TFT_ORANGE;
  return TFT_GREEN;
}

void noteActivity() {
  lastActivity = millis();
}

void setBacklightLevel(int level) {
  level = constrain(level, 0, 255);
  if (level <= 0) {
    ledcWrite(TFT_BACKLIGHT, 0);
    delay(15);
    pinMode(TFT_BACKLIGHT, OUTPUT);
    digitalWrite(TFT_BACKLIGHT, LOW);
    backlightPwmAttached = false;
    return;
  }
  if (!backlightPwmAttached) {
    ledcAttach(TFT_BACKLIGHT, 5000, 8);
    backlightPwmAttached = true;
  }
  ledcWrite(TFT_BACKLIGHT, level);
}

void applyBrightness() {
  brightness = constrain(brightness, 20, 255);
  setBacklightLevel(brightness);
}

void writeLedPin(int pin, bool on) {
  if (BACK_LED_ACTIVE_LOW) digitalWrite(pin, on ? LOW : HIGH);
  else digitalWrite(pin, on ? HIGH : LOW);
}

void applyBackLed() {
  writeLedPin(BACK_LED_R_PIN, backLedOn);
  writeLedPin(BACK_LED_G_PIN, backLedOn);
  writeLedPin(BACK_LED_B_PIN, backLedOn);
}

void loadSettings() {
  prefs.begin("cydDash", false);
  sleepModeIndex = prefs.getInt("sleep", 0);
  if (sleepModeIndex < 0 || sleepModeIndex > 4) sleepModeIndex = 0;
  themeIndex = prefs.getInt("theme", 0);
  if (themeIndex < 0 || themeIndex > 3) themeIndex = 0;
  backLedOn = prefs.getBool("backLed", true);
  brightness = prefs.getInt("bright", 180);
  brightness = constrain(brightness, 20, 255);
  int savedPage = prefs.getInt("page", 0);
  if (savedPage >= 0 && savedPage <= 5) page = savedPage;
}

void saveSettings() {
  prefs.putInt("sleep", sleepModeIndex);
  prefs.putBool("backLed", backLedOn);
  prefs.putInt("theme", themeIndex);
  prefs.putInt("bright", brightness);
}

void saveLastPage() {
  if (page >= 0 && page <= 5) prefs.putInt("page", page);
}

void drawTextBox(String text, int x, int y, int maxW, uint16_t color, int maxSize, int maxLines) {
  if (!text.length()) text = "-";
  int size = maxSize;
  while (size > 1 && (int)text.length() * 6 * size > maxW * maxLines) size--;
  int maxChars = maxW / (6 * size);
  tft.setTextSize(size);
  tft.setTextColor(color);
  int line = 0;
  while (text.length() > 0 && line < maxLines) {
    int take = min(maxChars, (int)text.length());
    int split = take;
    if (take < (int)text.length()) {
      for (int i = take; i > max(0, take - 12); i--) {
        if (text.charAt(i) == ' ') { split = i; break; }
      }
    }
    if (split <= 0) split = take;
    String part = text.substring(0, split);
    part.trim();
    tft.drawString(part, x, y + line * (8 * size + 2));
    text = text.substring(split);
    text.trim();
    line++;
  }
}

void drawPlusStar(int x, int y, uint16_t color) {
  tft.fillRect(x - 1, y - 1, 3, 3, color);
  tft.fillRect(x - 5, y, 11, 1, color);
  tft.fillRect(x, y - 5, 1, 11, color);
}

void drawMiniStar(int x, int y, uint16_t color) {
  tft.fillRect(x, y, 2, 2, color);
}

void drawMeteor(int x, int y, uint16_t head, uint16_t tail) {
  for (int i = 0; i < 8; i++) {
    tft.fillRect(x + i * 5, y - i * 4, 5, 3, (i < 2) ? head : tail);
  }
}

void drawStarField(uint8_t phase) {
  const int sx[] = {36,54,82,108,136,166,194,225,256,284,300,306,48,132,218,278,42,101,188,252};
  const int sy[] = {18,42,26,58,34,72,24,51,29,66,20,48,86,92,82,96,112,116,108,118};
  for (int i = 0; i < 20; i++) {
    uint16_t c = ((i + phase) % 5 == 0) ? accentColor() : TFT_WHITE;
    if (i % 6 == 0) drawPlusStar(sx[i], sy[i], c);
    else drawMiniStar(sx[i], sy[i], c);
  }
}

void drawPanelScene() {
  for (int y = 14; y < 229; y++) {
    tft.drawFastHLine(14, y, 292, panelBgColor(y));
  }
  const int sx[] = {30,54,93,128,169,212,248,286};
  const int sy[] = {54,82,48,104,70,96,54,112};
  for (int i = 0; i < 8; i++) {
    if (i % 5 == 0) drawPlusStar(sx[i], sy[i], (i % 2) ? accentColor() : TFT_WHITE);
    else drawMiniStar(sx[i], sy[i], (i % 3) ? TFT_WHITE : accentColor());
  }
  drawMeteor(202, 86, blend565(206, 170, 230), blend565(112, 79, 161));
}

void clearPanelArea(int x, int y, int w, int h) {
  for (int row = y; row < y + h; row++) {
    tft.drawFastHLine(x, row, w, panelBgColor(row));
  }
}

void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  lastWiFiTry = millis();
}

void serviceWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiOK = true;
    setupOTA();
    return;
  }
  wifiOK = false;
  lastError = "WiFi reconnecting";
  if (millis() - lastWiFiTry >= 10000) {
    WiFi.reconnect();
    lastWiFiTry = millis();
  }
}

bool fetchStats(bool force = false) {
  if (!force && millis() < skipFetchUntil) return false;
  if (WiFi.status() != WL_CONNECTED) {
    wifiOK = false;
    apiFailCount++;
    if (apiFailCount >= API_FAILS_BEFORE_LOST) { serverOK = false; lastError = "WiFi lost"; }
    return false;
  }
  wifiOK = true;
  WiFiClient client;
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(1800);
  http.setTimeout(1800);
  if (!http.begin(client, statsURL)) {
    apiFailCount++;
    if (apiFailCount >= API_FAILS_BEFORE_LOST) { lastError = "HTTP begin failed"; serverOK = false; }
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    http.end();
    apiFailCount++;
    if (apiFailCount >= API_FAILS_BEFORE_LOST) { lastError = "HTTP " + String(code); serverOK = false; }
    return false;
  }
  String p = http.getString();
  http.end();

  cpu = getValue(p, "cpu").toFloat();
  ram = getValue(p, "ram").toFloat();
  ramUsedGB = getValue(p, "ram_used_gb").toFloat();
  ramTotalGB = getValue(p, "ram_total_gb").toFloat();
  temp = getValue(p, "temp").toFloat();
  uptimeSeconds = getValue(p, "uptime").toInt();
  disk = getValue(p, "disk").toFloat();
  diskUsedGB = getValue(p, "disk_used_gb").toFloat();
  diskTotalGB = getValue(p, "disk_total_gb").toFloat();
  netDownKB = getValue(p, "net_down_kb").toFloat();
  netUpKB = getValue(p, "net_up_kb").toFloat();
  spotifyOK = getBoolValue(p, "spotify_ok");
  spotifyPlaying = getBoolValue(p, "spotify_playing");
  spotifyTrack = getStringValue(p, "spotify_track");
  spotifyArtist = getStringValue(p, "spotify_artist");
  spotifyAlbum = getStringValue(p, "spotify_album");

  spotifyProgressMs = bestSpotifyNumber(p, "spotify_progress_ms", "spotify_position_ms", "progress_ms");
  spotifyDurationMs = bestSpotifyNumber(p, "spotify_duration_ms", "spotify_length_ms", "duration_ms");

  if (spotifyDurationMs > 0 && spotifyDurationMs < 10000) spotifyDurationMs *= 1000;
  if (spotifyProgressMs > 0 && spotifyProgressMs < 10000 && spotifyDurationMs > 10000) spotifyProgressMs *= 1000;

  spotifyUpdatedAt = millis();
  cpuHistory[cpuIndex] = cpu;
  cpuIndex = (cpuIndex + 1) % CPU_HISTORY_SIZE;
  if (cpuCount < CPU_HISTORY_SIZE) cpuCount++;

  serverOK = true;
  apiFailCount = 0;
  lastError = "";
  return true;
}

bool sendSpotifyCommand(const char* cmd) {
  if (spotifyCommandBusy) return false;
  spotifyCommandBusy = true;
  if (WiFi.status() != WL_CONNECTED) { spotifyCommandBusy = false; return false; }
  WiFiClient client;
  HTTPClient http;
  String url = String(commandBaseURL) + "/spotify/" + cmd;
  http.setReuse(false);
  http.setConnectTimeout(1200);
  http.setTimeout(1500);
  if (!http.begin(client, url)) { spotifyCommandBusy = false; return false; }
  int code = http.POST("");
  String body = http.getString();
  http.end();
  spotifyCommandBusy = false;
  return code >= 200 && code < 300 && body.indexOf("\"ok\": true") >= 0;
}

bool sendSpotifyAction(const char* cmd) {
  String c = String(cmd);
  if (c == "previous") return sendSpotifyCommand("previous");
  if (c == "next")     return sendSpotifyCommand("next");
  if (c == "toggle")   return sendSpotifyCommand("toggle");
  return false;
}

void drawBackground() {
  for (int y = 0; y < 240; y++) {
    tft.drawFastHLine(0, y, 320, bgGradientColor(y));
  }
  for (int y = 0; y < 160; y += 8) {
    for (int x = 0; x < 320; x += 8) {
      if (((x / 8) * 5 + (y / 8) * 9) % 41 == 0) {
        tft.fillRect(x, y, 4, 4, blend565(45, 33, 86));
      }
    }
  }
  drawStarField(pixelPhase);
  drawMeteor(188, 84, blend565(214, 174, 235), blend565(122, 82, 164));
  drawMeteor(242, 128, blend565(130, 232, 240), blend565(64, 116, 158));
  tft.fillRoundRect(6, 6, 308, 228, 12, PANEL);
  drawPanelScene();
  tft.drawRoundRect(6, 6, 308, 228, 12, accentColor());
  tft.drawRoundRect(10, 10, 300, 220, 9, TFT_NAVY);
}

void drawPageShell(String title) {
  if (!screenShellReady) {
    drawBackground();
    screenShellReady = true;
  } else {
    drawPanelScene();
    tft.drawRoundRect(6, 6, 308, 228, 12, accentColor());
    tft.drawRoundRect(10, 10, 300, 220, 9, TFT_NAVY);
  }
  header(title);
}

void header(String title) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(3);
  tft.setTextColor(TFT_DARKGREY);
  tft.drawString(title, 18, 18);
  tft.setTextColor(accentColor());
  tft.drawString(title, 15, 15);
  clearPanelArea(242, 16, 65, 28);
  tft.setTextSize(1);
  tft.setTextColor(serverOK ? TFT_GREEN : TFT_RED);
  tft.drawString(serverOK ? "API OK" : "API LOST", 245, 18);
  tft.setTextColor(wifiOK ? TFT_GREEN : TFT_RED);
  tft.drawString(wifiOK ? "WiFi OK" : "WiFi LOST", 245, 32);
  tft.setTextColor(TFT_DARKGREY);
  tft.drawString("hold sides/swipe = page | swipe down = sleep", 15, 220);
}

void updateHeaderStatus() {
  clearPanelArea(242, 16, 65, 28);
  tft.setTextSize(1);
  tft.setTextColor(serverOK ? TFT_GREEN : TFT_RED);
  tft.drawString(serverOK ? "API OK" : "API LOST", 245, 18);
  tft.setTextColor(wifiOK ? TFT_GREEN : TFT_RED);
  tft.drawString(wifiOK ? "WiFi OK" : "WiFi LOST", 245, 32);
}

void barFrame(int x, int y, int w, int h) {
  tft.drawRoundRect(x, y, w, h, 5, TFT_DARKGREY);
}

void updateBar(int x, int y, int w, int h, float pct, uint16_t color) {
  pct = constrain(pct, 0, 100);
  int fill = (w - 4) * pct / 100.0;
  tft.fillRoundRect(x + 2, y + 2, w - 4, h - 4, 4, PANEL_DARK);
  if (fill > 0) tft.fillRoundRect(x + 2, y + 2, fill, h - 4, 4, color);
}

void clearValue(int x, int y, int w, int h) {
  clearPanelArea(x, y, w, h);
}

void drawSystemStatic() {
  drawPageShell("SYSTEM");
  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("CPU", 18, 62);
  tft.drawString("RAM", 18, 125);
  barFrame(18, 96, 284, 16);
  barFrame(18, 159, 284, 16);
}

void updateSystem() {
  updateHeaderStatus();
  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE);
  clearValue(200, 62, 95, 32);
  tft.drawString(String(cpu, 1) + "%", 205, 62);
  updateBar(18, 96, 284, 16, cpu, accentColor());
  clearValue(120, 125, 180, 32);
  tft.drawString(String(ramUsedGB, 1) + "/" + String(ramTotalGB, 1) + "G", 125, 125);
  updateBar(18, 159, 284, 16, ram, TFT_GREEN);
  tft.setTextSize(2);
  clearValue(18, 188, 120, 25);
  tft.setTextColor(tempColor(temp));
  tft.drawString("TEMP " + String(temp, 1) + "C", 18, 188);
  clearValue(150, 188, 145, 25);
  tft.setTextColor(TFT_MAGENTA);
  tft.drawString("UP " + uptimeText(uptimeSeconds), 150, 188);
}

void drawStatusPill(int x, int y, String text, uint16_t color) {
  int w = 72;
  tft.fillRoundRect(x, y, w, 18, 9, blend565(0, 3, 8));
  tft.drawRoundRect(x, y, w, 18, 9, color);
  tft.setTextSize(1);
  tft.setTextColor(color);
  tft.drawString(text, x + 10, y + 5);
}

int currentSpotifyProgressMs() {
  if (!spotifyPlaying || spotifyDurationMs <= 0) return spotifyProgressMs;
  unsigned long elapsed = millis() - spotifyUpdatedAt;
  long progress = (long)spotifyProgressMs + (long)elapsed;
  if (progress > spotifyDurationMs) progress = spotifyDurationMs;
  if (progress < 0) progress = 0;
  return (int)progress;
}

void clearSpotifyProgressArea() {
  clearPanelArea(18, 150, 288, 38);
}

void drawSpotifyProgress(bool force) {
  static int lastFill = -1;
  static int lastShownSecond = -1;
  static bool lastReady = false;

  int x = 22, y = 166, w = 276, h = 10;
  bool ready = spotifyOK && spotifyDurationMs > 0;
  int progress = currentSpotifyProgressMs();

  if (!ready) {
    if (!force && lastReady == false) return;
    clearSpotifyProgressArea();
    tft.drawRoundRect(x, y, w, h, 5, accentDarkColor());
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY);
    tft.drawString("--:--", x, 181);
    tft.drawString("--:--", 264, 181);
    lastFill = -1; lastShownSecond = -1; lastReady = false;
    return;
  }

  progress = constrain(progress, 0, spotifyDurationMs);
  int fill = map(progress, 0, spotifyDurationMs, 0, w - 2);
  fill = constrain(fill, 0, w - 2);
  int shownSecond = progress / 1000;

  if (!force && lastReady && fill == lastFill && shownSecond == lastShownSecond) return;

  clearSpotifyProgressArea();
  tft.drawRoundRect(x, y, w, h, 5, accentDarkColor());
  if (fill > 0) tft.fillRoundRect(x + 1, y + 1, fill, h - 2, 4, accentColor());
  int knobX = constrain(x + 1 + fill, x + 3, x + w - 4);
  tft.fillCircle(knobX, y + 5, 3, TFT_WHITE);

  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY);
  tft.drawString(timeText(progress), x, 181);
  tft.drawString("-" + timeText(max(spotifyDurationMs - progress, 0)), 248, 181);

  lastFill = fill; lastShownSecond = shownSecond; lastReady = true;
}

void drawSpotifyButton(int x, int y, int w, String label, uint16_t color) {
  tft.fillRoundRect(x, y, w, 28, 7, PANEL_DARK);
  tft.drawRoundRect(x, y, w, 28, 7, color);
  tft.setTextSize(2);
  tft.setTextColor(color);
  int textX = x + (w - label.length() * 12) / 2;
  tft.drawString(label, textX, y + 7);
}

void drawSpotifyControls() {
  clearPanelArea(18, 196, 288, 32);
  drawSpotifyButton(28, 198, 72, "<<", accentColor());
  drawSpotifyButton(124, 198, 72, spotifyPlaying ? "||" : ">", spotifyPlaying ? TFT_ORANGE : TFT_GREEN);
  drawSpotifyButton(220, 198, 72, ">>", accentColor());
}

void updateSpotifyMotion() {
  if (page != 1 || lastPage != 1) return;
  if (millis() - lastSpotifyProgressDraw < SPOTIFY_PROGRESS_INTERVAL_MS) return;
  lastSpotifyProgressDraw = millis();
  drawSpotifyProgress(false);
}

void drawSpotifyStatic() {
  drawPageShell("SPOTIFY");
  tft.fillRoundRect(18, 58, 284, 126, 10, PANEL_DARK);
  tft.drawRoundRect(18, 58, 284, 126, 10, TFT_NAVY);
  tft.drawRoundRect(22, 62, 276, 118, 8, accentDarkColor());
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY);
  tft.drawString("NOW PLAYING", 30, 70);
  tft.drawFastHLine(30, 86, 260, accentDarkColor());
  tft.drawFastHLine(30, 154, 260, TFT_NAVY);
  drawSpotifyControls();
  lastSpotifySeed = "";
  lastSpotifyTrack = "";
  lastSpotifyArtist = "";
  lastSpotifyOK = false;
  lastSpotifyPlaying = false;
}

void updateSpotify() {
  updateHeaderStatus();
  String seed = spotifyTrack + spotifyArtist + spotifyAlbum;
  if (!seed.length()) seed = "spotify";

  bool changed =
    seed != lastSpotifySeed ||
    spotifyTrack != lastSpotifyTrack ||
    spotifyArtist != lastSpotifyArtist ||
    spotifyOK != lastSpotifyOK ||
    spotifyPlaying != lastSpotifyPlaying;

  if (!changed) return;

  clearPanelArea(24, 66, 272, 112);
  tft.fillRoundRect(18, 58, 284, 126, 10, PANEL_DARK);
  tft.drawRoundRect(18, 58, 284, 126, 10, TFT_NAVY);
  tft.drawRoundRect(22, 62, 276, 118, 8, accentDarkColor());
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY);
  tft.drawString("NOW PLAYING", 30, 70);
  tft.drawFastHLine(30, 86, 260, accentDarkColor());
  tft.drawFastHLine(30, 154, 260, TFT_NAVY);

  if (!spotifyOK) {
    drawStatusPill(216, 68, "OFF", TFT_RED);
    tft.setTextSize(3);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("Idle", 30, 100);
    tft.setTextSize(2);
    tft.setTextColor(TFT_DARKGREY);
    tft.drawString("No Spotify data", 30, 136);
  } else {
    drawStatusPill(216, 68, spotifyPlaying ? "PLAY" : "PAUSE", spotifyPlaying ? TFT_GREEN : TFT_ORANGE);
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY);
    tft.drawString("TRACK", 30, 94);
    drawTextBox(spotifyTrack.length() ? spotifyTrack : "Unknown track", 30, 108, 260, TFT_WHITE, 2, 2);
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY);
    tft.drawString("ARTIST", 30, 158);
    drawTextBox(spotifyArtist.length() ? spotifyArtist : "Unknown artist", 78, 158, 210, accentColor(), 1, 1);
  }

  drawSpotifyControls();
  lastSpotifySeed = seed;
  lastSpotifyTrack = spotifyTrack;
  lastSpotifyArtist = spotifyArtist;
  lastSpotifyOK = spotifyOK;
  lastSpotifyPlaying = spotifyPlaying;
}

String currentPageTitle() {
  if (page == 0) return "SYSTEM";
  if (page == 1) return "SPOTIFY";
  if (page == 2) return "STORAGE";
  if (page == 3) return "NETWORK";
  if (page == 4) return "CPU GRAPH";
  return "SETTINGS";
}





void drawStorageStatic() {
  drawPageShell("STORAGE");
  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE);
  tft.drawString("DISK", 18, 72);
  barFrame(18, 112, 284, 20);
  tft.setTextSize(2);
  tft.setTextColor(accentColor());
  tft.drawString("USED", 18, 154);
  tft.drawString("TOTAL", 18, 184);
}

void updateStorage() {
  updateHeaderStatus();
  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE);
  clearValue(200, 72, 95, 32);
  tft.drawString(String(disk, 1) + "%", 205, 72);
  updateBar(18, 112, 284, 20, disk, diskColor(disk));
  tft.setTextSize(2);
  tft.setTextColor(accentColor());
  clearValue(90, 154, 200, 25);
  tft.drawString(String(diskUsedGB, 1) + " GB", 90, 154);
  clearValue(90, 184, 200, 25);
  tft.drawString(String(diskTotalGB, 1) + " GB", 90, 184);
}

void drawNetworkStatic() {
  drawPageShell("NETWORK");
  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN);
  tft.drawString("DOWN", 18, 72);
  tft.setTextColor(TFT_ORANGE);
  tft.drawString("UP", 18, 122);
  tft.setTextColor(accentColor());
  tft.drawString("RSSI", 18, 174);
}

void updateNetwork() {
  updateHeaderStatus();
  tft.setTextSize(3);
  clearValue(120, 66, 180, 36);
  tft.setTextColor(TFT_GREEN);
  tft.drawString(speedText(netDownKB), 120, 66);
  clearValue(120, 116, 180, 36);
  tft.setTextColor(TFT_ORANGE);
  tft.drawString(speedText(netUpKB), 120, 116);
  tft.setTextSize(2);
  clearValue(80, 174, 145, 25);
  tft.setTextColor(accentColor());
  tft.drawString(String(WiFi.RSSI()) + " dBm", 80, 174);
  tft.setTextSize(1);
  clearValue(210, 220, 95, 10);
  tft.setTextColor(TFT_DARKGREY);
  tft.drawString(WiFi.localIP().toString(), 210, 220);
}

void drawGraphStatic() {
  drawPageShell("CPU GRAPH");
  int gx = 18, gy = 68, gw = 284, gh = 115;
  tft.drawRoundRect(gx, gy, gw, gh, 7, TFT_DARKGREY);
  tft.drawFastHLine(gx, gy + gh / 2, gw, TFT_NAVY);
}

void updateGraph() {
  updateHeaderStatus();
  int gx = 18, gy = 68, gw = 284, gh = 115;
  clearPanelArea(gx + 2, gy + 2, gw - 4, gh - 4);
  tft.drawFastHLine(gx, gy + gh / 2, gw, TFT_NAVY);
  int points = cpuCount;
  int oldest = (cpuIndex - cpuCount + CPU_HISTORY_SIZE) % CPU_HISTORY_SIZE;
  int lastX = -1, lastY = -1;
  for (int i = 0; i < points; i++) {
    int idx = (oldest + i) % CPU_HISTORY_SIZE;
    float v = constrain(cpuHistory[idx], 0, 100);
    int denom = max(points - 1, 1);
    int x = gx + 4 + i * (gw - 8) / denom;
    int y = gy + gh - 5 - (int)(v * (gh - 10) / 100.0f);
    if (lastX >= 0) {
      tft.drawLine(lastX, lastY + 2, x, y + 2, TFT_NAVY);
      tft.drawLine(lastX, lastY, x, y, accentColor());
    }
    lastX = x; lastY = y;
  }
  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE);
  clearValue(120, 192, 115, 32);
  tft.drawString(String(cpu, 1) + "%", 125, 192);
}

void drawStatusStatic() {
  drawPageShell("STATUS");
  tft.setTextSize(2);
  tft.setTextColor(TFT_ORANGE);
  tft.drawString("Showing cached stats", 38, 80);
  tft.setTextColor(TFT_DARKGREY);
  tft.drawString("Retrying quietly", 38, 176);
}

void updateStatus() {
  updateHeaderStatus();
  tft.setTextSize(2);
  clearValue(38, 112, 240, 25);
  tft.setTextColor(TFT_WHITE);
  tft.drawString(lastError, 38, 112);
  clearValue(38, 144, 240, 25);
  tft.setTextColor(accentColor());
  tft.drawString(WiFi.localIP().toString(), 38, 144);
}

void drawButtonBox(int x, int y, int w, int h, String title, String value, uint16_t color) {
  tft.fillRoundRect(x, y, w, h, 8, PANEL_DARK);
  tft.drawRoundRect(x, y, w, h, 8, color);
  tft.setTextSize(2);
  tft.setTextColor(color);
  tft.drawString(title, x + 10, y + 8);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.drawString(value, x + 12, y + h - 16);
}

void drawSmallButton(int x, int y, int w, int h, String label, uint16_t color) {
  tft.fillRoundRect(x, y, w, h, 8, PANEL_DARK);
  tft.drawRoundRect(x, y, w, h, 8, color);
  tft.setTextSize(2);
  tft.setTextColor(color);
  int textX = x + (w - label.length() * 12) / 2;
  tft.drawString(label, textX, y + 10);
}

void drawSettingsStatic() {
  drawPageShell("SETTINGS");
  settingsMode = SETTINGS_MAIN;
  settingsForceRedraw = true;
  updateSettings();
}

void drawSettingsCard(int x, int y, int w, int h, String title, String value, uint16_t color) {
  tft.fillRoundRect(x, y, w, h, 9, PANEL_DARK);
  tft.drawRoundRect(x, y, w, h, 9, color);
  tft.setTextSize(1);
  tft.setTextColor(color);
  tft.drawString(title, x + 9, y + 8);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  int maxChars = max(1, (w - 18) / 12);
  String v = value;
  if ((int)v.length() > maxChars) v = v.substring(0, maxChars - 1) + ".";
  tft.drawString(v, x + 9, y + 24);
}

void drawBackChip() {
  tft.fillRoundRect(28, 66, 74, 30, 8, PANEL_DARK);
  tft.drawRoundRect(28, 66, 74, 30, 8, TFT_DARKGREY);
  tft.setTextSize(2);
  tft.setTextColor(TFT_DARKGREY);
  tft.drawString("BACK", 40, 74);
}

String brightnessLabel() {
  int pct = map(brightness, 20, 255, 5, 100);
  pct = constrain(pct, 5, 100);
  return String(pct) + "%";
}

void updateSettingsMain() {
  clearPanelArea(18, 58, 284, 160);
  tft.fillRoundRect(20, 58, 280, 152, 12, PANEL_DARK);
  tft.drawRoundRect(20, 58, 280, 152, 12, accentDarkColor());
  tft.drawRoundRect(24, 62, 272, 144, 9, TFT_NAVY);
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY);
  tft.drawString("SETTINGS", 34, 70);
  drawSettingsCard(30, 88, 126, 48, "BRIGHTNESS", brightnessLabel(), TFT_CYAN);
  drawSettingsCard(164, 88, 126, 48, "SLEEP", sleepLabels[sleepModeIndex], TFT_ORANGE);
  drawSettingsCard(30, 146, 126, 48, "BACK LED", backLedOn ? "ON" : "OFF", backLedOn ? TFT_GREEN : TFT_RED);
  drawSettingsCard(164, 146, 126, 48, "ACCENT", themeLabels[themeIndex], accentColor());
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY);
  tft.drawString("swipe left/right or BOOT = page", 64, 214);
}

void updateBrightnessSettings() {
  clearPanelArea(18, 58, 284, 160);
  tft.fillRoundRect(20, 58, 280, 152, 12, PANEL_DARK);
  tft.drawRoundRect(20, 58, 280, 152, 12, TFT_CYAN);
  drawBackChip();
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN);
  tft.drawString("BRIGHTNESS", 106, 72);
  tft.setTextSize(4);
  tft.setTextColor(TFT_WHITE);
  String label = brightnessLabel();
  int labelX = 160 - (label.length() * 24) / 2;
  tft.drawString(label, labelX, 108);
  drawSmallButton(34, 160, 104, 40, "-", TFT_ORANGE);
  drawSmallButton(182, 160, 104, 40, "+", TFT_GREEN);
}

void updateSleepSettings() {
  clearPanelArea(18, 58, 284, 160);
  tft.fillRoundRect(20, 58, 280, 152, 12, PANEL_DARK);
  tft.drawRoundRect(20, 58, 280, 152, 12, TFT_ORANGE);
  drawBackChip();
  tft.setTextSize(2);
  tft.setTextColor(TFT_ORANGE);
  tft.drawString("SLEEP TIMER", 110, 72);
  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE);
  int labelX = 160 - (sleepLabels[sleepModeIndex].length() * 18) / 2;
  tft.drawString(sleepLabels[sleepModeIndex], labelX, 112);
  drawSmallButton(34, 160, 104, 40, "<", accentColor());
  drawSmallButton(182, 160, 104, 40, ">", accentColor());
}

void updateLedSettings() {
  clearPanelArea(18, 58, 284, 160);
  tft.fillRoundRect(20, 58, 280, 152, 12, PANEL_DARK);
  tft.drawRoundRect(20, 58, 280, 152, 12, backLedOn ? TFT_GREEN : TFT_RED);
  drawBackChip();
  tft.setTextSize(2);
  tft.setTextColor(backLedOn ? TFT_GREEN : TFT_RED);
  tft.drawString("BACK LED", 116, 72);
  tft.setTextSize(4);
  tft.setTextColor(backLedOn ? TFT_GREEN : TFT_RED);
  tft.drawString(backLedOn ? "ON" : "OFF", 120, 112);
  drawButtonBox(56, 160, 208, 40, "TOGGLE", backLedOn ? "turn off" : "turn on", accentColor());
}

void updateThemeSettings() {
  clearPanelArea(18, 58, 284, 160);
  tft.fillRoundRect(20, 58, 280, 152, 12, PANEL_DARK);
  tft.drawRoundRect(20, 58, 280, 152, 12, accentColor());
  drawBackChip();
  tft.setTextSize(2);
  tft.setTextColor(accentColor());
  tft.drawString("ACCENT", 120, 72);
  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE);
  int labelX = 160 - (themeLabels[themeIndex].length() * 18) / 2;
  tft.drawString(themeLabels[themeIndex], labelX, 112);
  drawSmallButton(34, 160, 104, 40, "<", accentColor());
  drawSmallButton(182, 160, 104, 40, ">", accentColor());
}

void updateSettings() {
  updateHeaderStatus();

  static SettingsMode drawnMode = (SettingsMode)-1;
  static int drawnSleep = -1;
  static int drawnTheme = -1;
  static int drawnBrightness = -1;
  static bool drawnLed = false;
  static bool drawnLedValid = false;

  bool dirty = settingsForceRedraw ||
               drawnMode != settingsMode ||
               drawnSleep != sleepModeIndex ||
               drawnTheme != themeIndex ||
               drawnBrightness != brightness ||
               !drawnLedValid ||
               drawnLed != backLedOn;

  if (!dirty) return;

  if (settingsMode == SETTINGS_MAIN) updateSettingsMain();
  else if (settingsMode == SETTINGS_BRIGHTNESS) updateBrightnessSettings();
  else if (settingsMode == SETTINGS_SLEEP) updateSleepSettings();
  else if (settingsMode == SETTINGS_LED) updateLedSettings();
  else if (settingsMode == SETTINGS_THEME) updateThemeSettings();
  else { settingsMode = SETTINGS_MAIN; updateSettingsMain(); }

  drawnMode = settingsMode;
  drawnSleep = sleepModeIndex;
  drawnTheme = themeIndex;
  drawnBrightness = brightness;
  drawnLed = backLedOn;
  drawnLedValid = true;
  settingsForceRedraw = false;
}

void handleSettingsTouch(int x, int y) {
  if (page != 5) return;
  if (millis() - lastSettingsTap < SETTINGS_TAP_DEBOUNCE_MS) return;
  lastSettingsTap = millis();
  noteActivity();

  if (settingsMode != SETTINGS_MAIN && y < 104) {
    settingsMode = SETTINGS_MAIN;
    settingsForceRedraw = true;
    updateSettings();
    return;
  }

  if (settingsMode == SETTINGS_MAIN) {
    if      (x >= 28 && x <= 158 && y >= 84  && y <= 140) { settingsMode = SETTINGS_BRIGHTNESS; }
    else if (x >= 160 && x <= 292 && y >= 84  && y <= 140) { settingsMode = SETTINGS_SLEEP;      }
    else if (x >= 28 && x <= 158 && y >= 142 && y <= 198) { settingsMode = SETTINGS_LED;        }
    else if (x >= 160 && x <= 292 && y >= 142 && y <= 198) { settingsMode = SETTINGS_THEME;      }
    else return;
    settingsForceRedraw = true;
    updateSettings();
    return;
  }

  if (settingsMode == SETTINGS_BRIGHTNESS && y >= 150 && y <= 214) {
    const int levels[] = {30, 80, 140, 200, 255};
    int idx = 0, bestDiff = 999;
    for (int i = 0; i < 5; i++) {
      int d = abs(brightness - levels[i]);
      if (d < bestDiff) { bestDiff = d; idx = i; }
    }
    idx = constrain((x < 160) ? idx - 1 : idx + 1, 0, 4);
    brightness = levels[idx];
    applyBrightness();
    saveSettings();
    updateSettings();
    return;
  }

  if (settingsMode == SETTINGS_SLEEP && y >= 150 && y <= 214) {
    sleepModeIndex += (x < 160) ? -1 : 1;
    if (sleepModeIndex < 0) sleepModeIndex = 4;
    if (sleepModeIndex > 4) sleepModeIndex = 0;
    saveSettings();
    updateSettings();
    return;
  }

  if (settingsMode == SETTINGS_LED && y >= 146 && y <= 216) {
    backLedOn = !backLedOn;
    applyBackLed();
    saveSettings();
    updateSettings();
    return;
  }

  if (settingsMode == SETTINGS_THEME && y >= 150 && y <= 214) {
    themeIndex += (x < 160) ? -1 : 1;
    if (themeIndex < 0) themeIndex = 3;
    if (themeIndex > 3) themeIndex = 0;
    saveSettings();
    screenShellReady = false;
    settingsForceRedraw = true;
    drawStaticPage();
    updateSettings();
    return;
  }
}

void drawStaticPage() {
  if (!serverOK && cpu == 0 && ram == 0) {
    drawStatusStatic();
    lastPage = -2;
    return;
  }
  if      (page == 0) drawSystemStatic();
  else if (page == 1) drawSpotifyStatic();
  else if (page == 2) drawStorageStatic();
  else if (page == 3) drawNetworkStatic();
  else if (page == 4) drawGraphStatic();
  else drawSettingsStatic();
  lastPage = page;
}

void drawCurrentPageValues() {
  if      (page == 0) updateSystem();
  else if (page == 1) updateSpotify();
  else if (page == 2) updateStorage();
  else if (page == 3) updateNetwork();
  else if (page == 4) updateGraph();
  else updateSettings();
}

void updatePageValues() {
  if (!serverOK && cpu == 0 && ram == 0) {
    if (lastPage != -2) drawStatusStatic();
    updateStatus();
    lastPage = -2;
    return;
  }
  if (lastPage != page) drawStaticPage();
  drawCurrentPageValues();
}

void animatePixels() {
  pixelPhase = (pixelPhase + 1) % 24;
  const int tx[] = {44, 78, 116, 154, 196, 232, 276};
  const int ty[] = {46, 51,  45,  52,  47,  50,  45};
  for (int i = 0; i < 7; i++) {
    clearPanelArea(tx[i] - 5, ty[i] - 5, 11, 11);
    if (((pixelPhase + i * 3) % 8) < 3) drawPlusStar(tx[i], ty[i], TFT_WHITE);
    else drawMiniStar(tx[i], ty[i], (i % 2) ? accentColor() : accentDarkColor());
  }
  tft.drawRoundRect(6, 6, 308, 228, 12, accentColor());
  tft.drawRoundRect(10, 10, 300, 220, 9, TFT_NAVY);
}

void enterSleepMode() {
  sleeping = true;
  tft.fillScreen(TFT_BLACK);
  delay(40);
  setBacklightLevel(0);
  touchWasDown = false;
  touchPageTriggered = false;

  while (digitalRead(BOOT_BTN) == HIGH && !touch.touched()) delay(30);
  while (digitalRead(BOOT_BTN) == LOW  ||  touch.touched()) delay(30);

  setBacklightLevel(brightness);
  delay(120);
  touchWasDown = false;
  touchPageTriggered = false;
  lastTouchTime = millis();
  sleeping = false;
  noteActivity();
  screenShellReady = false;
  drawStaticPage();
  updatePageValues();
}

void changePage(int dir) {
  page += dir;
  if (page > 5) page = 0;
  if (page < 0) page = 5;
  settingsMode = SETTINGS_MAIN;
  saveLastPage();
  tft.startWrite();
  drawStaticPage();
  drawCurrentPageValues();
  tft.endWrite();
}

void checkButton() {
  bool b = digitalRead(BOOT_BTN);
  if (lastBtn == HIGH && b == LOW && millis() - lastBtnTime > 350) {
    lastBtnTime = millis();
    changePage(1);
  }
  lastBtn = b;
}

void mapTouchPoint(TS_Point p, int &tx, int &ty) {
  int rawX = p.x, rawY = p.y;
  if (TOUCH_SWAP_XY) { int tmp = rawX; rawX = rawY; rawY = tmp; }
  tx = map(rawX, TOUCH_MIN_X, TOUCH_MAX_X, 0, 319);
  ty = map(rawY, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, 239);
  if (TOUCH_INVERT_X) tx = 319 - tx;
  if (TOUCH_INVERT_Y) ty = 239 - ty;
  tx = constrain(tx, 0, 319);
  ty = constrain(ty, 0, 239);
}

void flashSpotifyButton(int which, uint16_t color) {
  if (page != 1) return;
  int x = 28;
  String label = "<<";
  if (which == 1) { x = 124; label = spotifyPlaying ? "||" : ">"; }
  else if (which == 2) { x = 220; label = ">>"; }
  tft.fillRoundRect(x, 198, 72, 28, 7, color);
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLACK);
  int textX = x + (72 - label.length() * 12) / 2;
  tft.drawString(label, textX, 205);
  restoreButtonsAt = millis() + 500;
}

void handleSpotifyTouch(int x, int y) {
  if (page != 1 || y < 170 || spotifyCommandBusy) return;
  int button = -1;
  const char* action = "";
  if      (x < 112)             { button = 0; action = "previous"; }
  else if (x >= 112 && x < 208) { button = 1; action = "toggle";   }
  else if (x >= 208)            { button = 2; action = "next";      }
  if (button == -1) return;
  flashSpotifyButton(button, TFT_GREEN);
  bool sent = sendSpotifyAction(action);
  if (!sent) { flashSpotifyButton(button, TFT_RED); restoreButtonsAt = millis() + 500; return; }
  if (String(action) == "toggle") { spotifyPlaying = !spotifyPlaying; lastSpotifySeed = ""; updateSpotify(); }
  skipFetchUntil = 0;
  pendingSpotifyRefreshAt = millis() + 350;
  nextSpotifyFastRefreshAt = millis() + 850;
  spotifyFastRefreshCount = 0;
  lastFetch = millis();
}

void handleSpotifyLongPress(int x, int y) {
  if (page != 1 || y < 170 || spotifyCommandBusy) return;
  if (x >= 112 && x < 208) {
    flashSpotifyButton(1, accentColor());
    if (fetchStats(true)) updatePageValues();
    restoreButtonsAt = millis() + 500;
  }
}

void checkTouch() {
  if (!touch.touched()) {
    if (touchWasDown) {
      unsigned long tapLen = millis() - touchDownTime;
      int dx = touchLastX - touchStartX;
      int dy = touchLastY - touchStartY;
      noteActivity();

      bool swipeDownSleep =
        touchStartY <= SWIPE_SLEEP_START_Y &&
        touchLastY  >= SWIPE_SLEEP_END_Y   &&
        dy >= SWIPE_SLEEP_MIN_DIST          &&
        tapLen < 1600;

      if (swipeDownSleep) {
        touchWasDown = false; touchPageTriggered = false;
        enterSleepMode();
        return;
      }

      if (!touchPageTriggered && tapLen < 1200 && abs(dx) >= 95 && abs(dx) > abs(dy) + 25) {
        touchWasDown = false; touchPageTriggered = false;
        changePage((dx < 0) ? 1 : -1);
        return;
      }

      if (!touchPageTriggered && page == 1 && tapLen >= 700 && tapLen < 1800 && touchStartY >= 170) {
        handleSpotifyLongPress(touchStartX, touchStartY);
      } else if (!touchPageTriggered && tapLen < 700) {
        if (page == 1) handleSpotifyTouch(touchStartX, touchStartY);
        else if (page == 5) handleSettingsTouch(touchStartX, touchStartY);
      }
    }
    touchWasDown = false; touchPageTriggered = false;
    return;
  }

  TS_Point p = touch.getPoint();
  int tx, ty;
  mapTouchPoint(p, tx, ty);
  touchLastX = tx; touchLastY = ty;

  if (!touchWasDown) {
    touchWasDown = true; touchPageTriggered = false;
    touchDownTime = millis();
    touchRawStartX = p.x; touchRawStartY = p.y;
    touchStartX = tx; touchStartY = ty;
    touchLastX = tx; touchLastY = ty;
    return;
  }

  bool bottomSpotifyZone = (page == 1 && touchStartY >= 165);
  bool settingsTapZone   = (page == 5);
  bool possibleSleepSwipe = touchStartY <= SWIPE_SLEEP_START_Y;

  if (!bottomSpotifyZone && !settingsTapZone && !possibleSleepSwipe &&
      !touchPageTriggered &&
      millis() - touchDownTime >= PAGE_HOLD_MS &&
      millis() - lastTouchTime >= TOUCH_DEBOUNCE_MS) {
    if (touchStartX < 95) {
      touchPageTriggered = true; lastTouchTime = millis(); changePage(-1);
    } else if (touchStartX > 225) {
      touchPageTriggered = true; lastTouchTime = millis(); changePage(1);
    }
  }
}

void setupOTA() {
  if (otaReady || WiFi.status() != WL_CONNECTED) return;

  ArduinoOTA.setHostname(otaHostname);
  ArduinoOTA.setPassword(otaPassword);

  ArduinoOTA.onStart([]() {
    sleeping = false;
    setBacklightLevel(brightness);
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(2); tft.setTextColor(accentColor());
    tft.drawString("OTA UPDATE", 92, 72);
    tft.setTextSize(1); tft.setTextColor(TFT_WHITE);
    tft.drawString("Uploading firmware...", 92, 102);
    tft.drawRoundRect(52, 130, 216, 14, 6, TFT_DARKGREY);
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (!total) return;
    int fill = constrain(map(progress * 100 / total, 0, 100, 0, 212), 0, 212);
    tft.fillRoundRect(54, 132, 212, 10, 5, PANEL_DARK);
    if (fill > 0) tft.fillRoundRect(54, 132, fill, 10, 5, accentColor());
    tft.fillRect(132, 152, 70, 18, TFT_BLACK);
    tft.setTextSize(2); tft.setTextColor(TFT_WHITE);
    tft.drawString(String(progress * 100 / total) + "%", 138, 152);
  });

  ArduinoOTA.onEnd([]() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2); tft.setTextColor(TFT_GREEN);
    tft.drawString("OTA DONE", 108, 104);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(2); tft.setTextColor(TFT_RED);
    tft.drawString("OTA ERROR", 102, 92);
    tft.setTextSize(1); tft.setTextColor(TFT_WHITE);
    tft.drawString("Code: " + String((int)error), 128, 122);
    delay(1500);
    screenShellReady = false;
    drawStaticPage(); updatePageValues();
  });

  ArduinoOTA.begin();
  otaReady = true;
}

void connectWiFiStartup() {
  for (int y = 0; y < 240; y++) tft.drawFastHLine(0, y, 320, bgGradientColor(y));
  tft.fillRoundRect(6, 6, 308, 228, 12, PANEL);
  for (int y = 14; y < 229; y++) tft.drawFastHLine(14, y, 292, panelBgColor(y));
  tft.drawRoundRect(6, 6, 308, 228, 12, accentColor());
  tft.drawRoundRect(10, 10, 300, 220, 9, TFT_NAVY);
  tft.setTextSize(3); tft.setTextColor(TFT_WHITE);
  tft.drawString("Connecting", 70, 90);
  tft.setTextColor(accentColor());
  tft.drawString("WiFi...", 105, 125);

  startWiFi();
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(100);
  wifiOK = WiFi.status() == WL_CONNECTED;
  screenShellReady = false;
}

void setup() {
  Serial.begin(115200);

  pinMode(BACK_LED_R_PIN, OUTPUT);
  pinMode(BACK_LED_G_PIN, OUTPUT);
  pinMode(BACK_LED_B_PIN, OUTPUT);

  loadSettings();
  applyBackLed();

  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);
  delay(20);
  ledcAttach(TFT_BACKLIGHT, 5000, 8);
  backlightPwmAttached = true;
  applyBrightness();

  pinMode(BOOT_BTN, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);
  tft.invertDisplay(true);

  PANEL      = tft.color565(20, 16, 46);
  PANEL_DARK = tft.color565(10, 8, 24);
  CYAN_DARK  = tft.color565(0, 80, 100);

  touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSPI);
  touch.setRotation(1);

  for (int i = 0; i < CPU_HISTORY_SIZE; i++) cpuHistory[i] = 0;

  noteActivity();
  connectWiFiStartup();
  setupOTA();
  fetchStats();
  drawStaticPage();
  updatePageValues();
}

void loop() {
  checkButton();
  checkTouch();
  serviceWiFi();
  if (otaReady) ArduinoOTA.handle();

  if (!sleeping && sleepTimeouts[sleepModeIndex] > 0 &&
      millis() - lastActivity >= sleepTimeouts[sleepModeIndex]) {
    enterSleepMode();
  }

  if (restoreButtonsAt && millis() >= restoreButtonsAt) {
    restoreButtonsAt = 0;
    if (page == 1) drawSpotifyControls();
  }

  if (pendingSpotifyRefreshAt && millis() >= pendingSpotifyRefreshAt) {
    pendingSpotifyRefreshAt = 0;
    if (fetchStats(true)) updatePageValues();
  }

  if (nextSpotifyFastRefreshAt && millis() >= nextSpotifyFastRefreshAt) {
    nextSpotifyFastRefreshAt = 0;
    spotifyFastRefreshCount++;
    if (fetchStats(true)) updatePageValues();
    if (page == 1 && spotifyFastRefreshCount < 4) {
      nextSpotifyFastRefreshAt = millis() + 700;
    }
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastFetch >= FETCH_INTERVAL_MS) {
    lastFetch = millis();
    bool ok = fetchStats();
    if (ok) {
      tft.startWrite(); updatePageValues(); tft.endWrite();
    } else {
      updateHeaderStatus();
    }
  }

  if (millis() - lastAnim >= ANIM_INTERVAL_MS) {
    lastAnim = millis();
    animatePixels();
  }
}
