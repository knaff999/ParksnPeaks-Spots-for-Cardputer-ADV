/*
  ParksNPeaks Monitor — M5Stack Cardputer / Cardputer-Adv
  --------------------------------------------------------
  Connects to Wi-Fi, polls https://parksnpeaks.org/api/ALL
  (live WWFF/SOTA/etc activation spots for VK/ZL), and shows
  the most recent activity on the built-in screen.

  Board:     "M5Cardputer" (covers both Cardputer and Cardputer-Adv)
  Libraries: M5Cardputer (>=1.1.0) — pulls in M5Unified + M5GFX
             ArduinoJson (>=6.x)
  Install via Library Manager: search "M5Cardputer", then "ArduinoJson".

  Controls:
    ;      -> scroll up
    .      -> scroll down
    Enter  -> force refresh now

  NOTE: Arduino requires the .ino filename to match its parent
  folder name — keep this file inside a folder called
  "ParksNPeaksMonitor".
*/

#include "M5Cardputer.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ---------------- USER CONFIG ----------------
const char* WIFI_SSID     = "SSID";
const char* WIFI_PASSWORD = "PASSWORD";
const char* API_URL       = "https://parksnpeaks.org/api/ALL";
const unsigned long REFRESH_INTERVAL_MS = 5UL * 60UL * 1000UL; // auto-refresh every 5 min
// -----------------------------------------------

struct Spot {
  String time;      // HH:MM (UTC), parsed out of actTime
  String callsign;   // actCallsign
  String mode;        // actMode
  String freq;         // actFreq
  String cls;            // actClass (WWFF, SOTA, SANPCPA, KRMNPA, ...)
  String siteID;          // actSiteID
  String location;        // actLocation
  String comments;        // actComments
};

#define MAX_SPOTS 20
Spot spots[MAX_SPOTS];
int spotCount = 0;

int scrollIndex = 0;
const int ROWS_VISIBLE = 3;     // entries shown per screen (3 text lines each)
bool needRedraw = true;
String statusLine = "Booting...";
unsigned long lastFetch = 0;

WiFiClientSecure secureClient;

void drawScreen();

uint16_t classColor(const String& c) {
  if (c == "WWFF")    return TFT_GREEN;
  if (c == "SOTA")    return TFT_YELLOW;
  if (c == "SANPCPA") return TFT_CYAN;
  if (c == "KRMNPA")  return TFT_ORANGE;
  return TFT_WHITE;
}

void connectWifi() {
  statusLine = "Connecting WiFi";
  drawScreen();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
  }
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");  // UTC, no DST offset
}



bool fetchSpots() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
    if (WiFi.status() != WL_CONNECTED) {
      statusLine = "WiFi failed";
      return false;
    }
  }

  statusLine = "Fetching...";
  drawScreen();

  secureClient.setInsecure();   // skip TLS cert validation (simplest for this API)
  HTTPClient http;
  http.begin(secureClient, API_URL);
  int code = http.GET();
  if (code != 200) {
    statusLine = "HTTP err " + String(code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  // Sized generously for ~40 spots worth of JSON; raise if you see "JSON err"
  // during big contest weekends with lots of simultaneous activations.
  DynamicJsonDocument doc(40960);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    statusLine = "JSON err";
    return false;
  }

  spotCount = 0;
  for (JsonObject obj : doc.as<JsonArray>()) {
    if (spotCount >= MAX_SPOTS) break;
    Spot& s = spots[spotCount];
    String t   = obj["actTime"].as<String>();
    s.time     = t.length() >= 16 ? t.substring(11, 16) : t;  // "HH:MM"
    s.callsign = obj["actCallsign"].as<String>();
    s.mode     = obj["actMode"].as<String>();
    s.freq     = obj["actFreq"].as<String>();
    s.cls      = obj["actClass"].as<String>();
    s.siteID   = obj["actSiteID"].as<String>();
    s.location = obj["actLocation"].as<String>();
    s.comments = obj["actComments"].as<String>();
    spotCount++;
  }

  scrollIndex = 0;
  //statusLine  = String(spotCount) + " spots";
  struct tm ti;
  char timeBuf[9];  // "HH:MM:SS\0"
  if (getLocalTime(&ti, 500)) {
    strftime(timeBuf, sizeof(timeBuf), "%H:%M", &ti);
  } else {
    strcpy(timeBuf, "--:--");
  }
  statusLine = String(spotCount) + "sp " + timeBuf + "z";

  lastFetch   = millis();
  return true;
}

void drawScreen() {
  auto& d = M5Cardputer.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);

  // header bar
  d.fillRect(0, 0, d.width(), 14, TFT_NAVY);
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_NAVY);
  d.setCursor(2, 3);
  d.print("ParksNPeaks (UTC)");
  String stat = statusLine.length() > 16 ? statusLine.substring(0, 16) : statusLine;
  d.setCursor(d.width() - (stat.length() * 6) - 4, 3);
  d.print(stat);

  // spot list
  int y = 18, rowH = 37, shown = 0;
  for (int i = scrollIndex; i < spotCount && shown < ROWS_VISIBLE; i++, shown++) {
    Spot& s = spots[i];
    d.setTextColor(classColor(s.cls), TFT_BLACK);
    d.setCursor(2, y);
    d.print(s.time + " " + s.callsign + "  " + s.freq + " " + s.mode);

    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.setCursor(2, y + 11);
    String line2 = s.cls + " " + s.siteID + " " + s.location;
    if (line2.length() > 39) line2 = line2.substring(0, 39);
    d.print(line2);

    d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.setCursor(2, y + 22);
    String line3 = s.comments;
    if (line3.length() > 39) line3 = line3.substring(0, 39);
    d.print(line3);

    y += rowH;
  }

  if (spotCount == 0) {
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(4, 50);
    d.print("No current activity");
  }

  // footer bar
  d.fillRect(0, d.height() - 12, d.width(), 12, TFT_DARKGREY);
  d.setTextColor(TFT_WHITE, TFT_DARKGREY);
  d.setCursor(2, d.height() - 10);
  d.print(";/. scroll   Enter=refresh");

  d.endWrite();
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);  // true = enable keyboard
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(1);

  fetchSpots();
  needRedraw = true;
}

void loop() {
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
      if (scrollIndex > 0) scrollIndex--;
      needRedraw = true;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
      if (scrollIndex < max(0, spotCount - ROWS_VISIBLE)) scrollIndex++;
      needRedraw = true;
    }
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
      fetchSpots();
      needRedraw = true;
    }
  }

  if (millis() - lastFetch > REFRESH_INTERVAL_MS) {
    fetchSpots();
    needRedraw = true;
  }

  if (needRedraw) {
    drawScreen();
    needRedraw = false;
  }

  delay(10);
}
