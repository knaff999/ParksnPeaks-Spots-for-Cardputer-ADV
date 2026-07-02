/*
  ParksNPeaks Monitor — M5Stack Cardputer / Cardputer-Adv
  --------------------------------------------------------
  On first boot (or when 'W' is held at startup): scans for Wi-Fi
  networks, lets you pick one with ; / . keys, types the password,
  and saves both to NVS (Preferences) so they survive reboots.

  Board:     "M5Cardputer"  (covers Cardputer and Cardputer-Adv)
  Libraries (install via Library Manager):
    - M5Cardputer  >= 1.1.0  (pulls in M5Unified + M5GFX automatically)
    - ArduinoJson  >= 6.x

  Startup Wi-Fi wizard controls:
    ;       scroll network list up
    .       scroll network list down
    Enter   select highlighted network / confirm password
    Bksp    delete last password character
    W (hold at power-on)  force wizard even when creds are saved

  Spot list controls:
    ;       scroll up
    .       scroll down
    Enter   force-refresh now
    W       re-open Wi-Fi wizard

  The .ino filename must match its parent folder name.
  Keep this file in a folder called "ParksNPeaksMonitor".
*/

#include "M5Cardputer.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ---- constants ----
const char* API_URL              = "https://parksnpeaks.org/api/ALL";
const unsigned long REFRESH_MS   = 5UL * 60UL * 1000UL;   // 5-minute auto-refresh
const int  MAX_SPOTS             = 40;
const int  ROWS_VISIBLE          = 4;
const int  MAX_NETS              = 20;

// ---- NVS (saved credentials) ----
Preferences prefs;
String savedSSID;
String savedPass;

// ---- Wi-Fi scanner state ----
struct NetEntry { String ssid; int rssi; };
NetEntry nets[MAX_NETS];
int netCount    = 0;
int netScroll   = 0;  // top-of-list index
int netSel      = 0;  // highlighted index

// ---- spot data ----
struct Spot {
  String time;
  String callsign;
  String mode;
  String freq;
  String cls;
  String siteID;
  String location;
};
Spot spots[MAX_SPOTS];
int spotCount   = 0;
int scrollIndex = 0;

// ---- misc state ----
bool       needRedraw = true;
String     statusLine = "Booting...";
unsigned long lastFetch = 0;
WiFiClientSecure secureClient;

// ============================================================
//  UTILITIES
// ============================================================
uint16_t classColor(const String& c) {
  if (c == "WWFF")    return TFT_GREEN;
  if (c == "SOTA")    return TFT_YELLOW;
  if (c == "SANPCPA") return TFT_CYAN;
  if (c == "KRMNPA")  return TFT_ORANGE;
  return TFT_WHITE;
}

// Block until no key is physically held (debounce for mode transitions)
void waitKeyRelease() {
  delay(80);
  while (true) {
    M5Cardputer.update();
    if (!M5Cardputer.Keyboard.isPressed()) break;
    delay(20);
  }
  delay(80);
}

// ============================================================
//  MAIN SPOT SCREEN
// ============================================================
void drawSpotScreen() {
  auto& d = M5Cardputer.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);

  // header
  d.fillRect(0, 0, d.width(), 14, TFT_NAVY);
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_NAVY);
  d.setCursor(2, 3);
  d.print("ParksNPeaks (UTC)");
  String stat = statusLine;
  if (stat.length() > 14) stat = stat.substring(0, 14);
  d.setCursor(d.width() - (int)(stat.length() * 6) - 2, 3);
  d.print(stat);

  // entries
  int y = 18, rowH = 26, shown = 0;
  for (int i = scrollIndex; i < spotCount && shown < ROWS_VISIBLE; i++, shown++) {
    Spot& s = spots[i];
    d.setTextColor(classColor(s.cls), TFT_BLACK);
    d.setCursor(2, y);
    String row1 = s.time + " " + s.callsign + "  " + s.freq + " " + s.mode;
    if (row1.length() > 39) row1 = row1.substring(0, 39);
    d.print(row1);

    d.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    d.setCursor(2, y + 11);
    String row2 = s.cls + " " + s.siteID + " " + s.location;
    if (row2.length() > 39) row2 = row2.substring(0, 39);
    d.print(row2);

    y += rowH;
  }

  if (spotCount == 0) {
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(4, 50);
    d.print("No current activity");
  }

  // footer
  d.fillRect(0, d.height() - 12, d.width(), 12, TFT_DARKGREY);
  d.setTextColor(TFT_WHITE, TFT_DARKGREY);
  d.setCursor(2, d.height() - 10);
  d.print(";/. scroll  Ent=refresh W=wifi");

  d.endWrite();
}

// ============================================================
//  WI-FI WIZARD SCREENS
// ============================================================
void drawNetList(const String& title) {
  auto& d = M5Cardputer.Display;
  const int NETS_PER_PAGE = 6;
  d.startWrite();
  d.fillScreen(TFT_BLACK);

  // header
  d.fillRect(0, 0, d.width(), 14, TFT_NAVY);
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_NAVY);
  d.setCursor(2, 3);
  d.print(title);

  // list
  int y = 18;
  int topIndex = (netSel / NETS_PER_PAGE) * NETS_PER_PAGE;
  for (int i = topIndex; i < netCount && i < topIndex + NETS_PER_PAGE; i++) {
    bool sel = (i == netSel);
    d.fillRect(0, y, d.width(), 13, sel ? TFT_NAVY : TFT_BLACK);
    d.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? TFT_NAVY : TFT_BLACK);
    d.setCursor(4, y + 2);
    String line = nets[i].ssid;
    if (line.length() > 28) line = line.substring(0, 28);
    line += "  " + String(nets[i].rssi) + "dBm";
    d.print(line);
    y += 14;
  }

  // footer
  d.fillRect(0, d.height() - 12, d.width(), 12, TFT_DARKGREY);
  d.setTextColor(TFT_WHITE, TFT_DARKGREY);
  d.setCursor(2, d.height() - 10);
  d.print(";/. move   Enter=select");

  d.endWrite();
}

void drawPasswordScreen(const String& ssid, const String& pass) {
  auto& d = M5Cardputer.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);

  d.fillRect(0, 0, d.width(), 14, TFT_NAVY);
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_NAVY);
  d.setCursor(2, 3);
  d.print("Password for:");

  d.setTextColor(TFT_GREEN, TFT_BLACK);
  d.setCursor(2, 18);
  String ssidd = ssid.length() > 38 ? ssid.substring(0, 38) : ssid;
  d.print(ssidd);

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(2, 36);
  d.print("PW: ");
  // Show password masked as asterisks
  String masked = "";
  for (unsigned int i = 0; i < pass.length(); i++) masked += '*';
  // append actual last char so user can see what they just typed
  if (pass.length() > 0) {
    masked.remove(masked.length() - 1);
    masked += pass.charAt(pass.length() - 1);
  }
  d.print(masked);

  d.fillRect(0, d.height() - 24, d.width(), 12, TFT_DARKGREY);
  d.setTextColor(TFT_WHITE, TFT_DARKGREY);
  d.setCursor(2, d.height() - 22);
  d.print("Type password  Bksp=del");

  d.fillRect(0, d.height() - 12, d.width(), 12, TFT_DARKGREY);
  d.setCursor(2, d.height() - 10);
  d.print("Enter=connect  (blank=none)");

  d.endWrite();
}

void drawConnecting(const String& ssid) {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(4, 40);
  d.print("Connecting to:");
  d.setCursor(4, 56);
  d.print(ssid);
  d.setCursor(4, 72);
  d.print("Please wait...");
}

void drawConnectResult(bool ok, const String& ssid) {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setCursor(4, 40);
  if (ok) {
    d.setTextColor(TFT_GREEN, TFT_BLACK);
    d.print("Connected!");
    d.setCursor(4, 56);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.print(ssid);
    d.setCursor(4, 72);
    d.print("Saving & loading spots...");
  } else {
    d.setTextColor(TFT_RED, TFT_BLACK);
    d.print("Connection failed.");
    d.setCursor(4, 56);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.print("Press Enter to retry.");
  }
}

// ============================================================
//  WI-FI WIZARD — full flow, returns true when connected
// ============================================================
bool runWifiWizard() {
  auto& d = M5Cardputer.Display;

  // --- scan ---
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setCursor(4, 40);
  d.print("Scanning Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(200);
  int found = WiFi.scanNetworks();
  netCount = 0;
  // collect unique SSIDs sorted by RSSI (already sorted by SDK)
  for (int i = 0; i < found && netCount < MAX_NETS; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    // deduplicate
    bool dup = false;
    for (int j = 0; j < netCount; j++) {
      if (nets[j].ssid == s) { dup = true; break; }
    }
    if (!dup) {
      nets[netCount].ssid = s;
      nets[netCount].rssi = WiFi.RSSI(i);
      netCount++;
    }
  }
  WiFi.scanDelete();

  if (netCount == 0) {
    d.fillScreen(TFT_BLACK);
    d.setTextColor(TFT_RED, TFT_BLACK);
    d.setCursor(4, 40);
    d.print("No networks found.");
    d.setCursor(4, 56);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.print("Press Enter to retry.");
    waitKeyRelease();
    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) return runWifiWizard();
      }
      delay(20);
    }
  }

  netSel = 0;
  drawNetList("Select Wi-Fi");
  waitKeyRelease();

  // --- network selection loop ---
  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (netSel > 0) netSel--;
        drawNetList("Select Wi-Fi");
      }
      if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (netSel < netCount - 1) netSel++;
        drawNetList("Select Wi-Fi");
      }
      if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
        break;
      }
    }
    delay(20);
  }

  String chosenSSID = nets[netSel].ssid;
  waitKeyRelease();

  // --- password entry loop ---
  String pass = "";
  drawPasswordScreen(chosenSSID, pass);

  while (true) {
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState ks = M5Cardputer.Keyboard.keysState();

      for (char c : ks.word) {
        pass += c;
      }
      if (ks.del && pass.length() > 0) {
        pass.remove(pass.length() - 1);
      }
      if (ks.enter) {
        break;
      }
      drawPasswordScreen(chosenSSID, pass);
    }
    delay(20);
  }

  // --- connect ---
  drawConnecting(chosenSSID);
  WiFi.begin(chosenSSID.c_str(), pass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
  }
  bool ok = (WiFi.status() == WL_CONNECTED);

  if (ok) {
    // Save to NVS
    prefs.begin("wifi", false);
    prefs.putString("ssid", chosenSSID);
    prefs.putString("pass", pass);
    prefs.end();
    savedSSID = chosenSSID;
    savedPass = pass;
  }

  drawConnectResult(ok, chosenSSID);
  delay(ok ? 1500 : 0);

  if (!ok) {
    // wait for Enter then let user retry
    waitKeyRelease();
    while (true) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) return runWifiWizard();
      }
      delay(20);
    }
  }

  return true;
}

// ============================================================
//  CONNECT USING SAVED CREDENTIALS
// ============================================================
bool connectSaved() {
  if (savedSSID.length() == 0) return false;
  statusLine = "Connecting...";
  drawSpotScreen();
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
  }
  return (WiFi.status() == WL_CONNECTED);
}

// ============================================================
//  FETCH SPOTS
// ============================================================
bool fetchSpots() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectSaved()) {
      statusLine = "WiFi lost";
      return false;
    }
  }

  statusLine = "Fetching...";
  drawSpotScreen();

  secureClient.setInsecure();
  HTTPClient http;
  http.begin(secureClient, API_URL);
  int code = http.GET();
  if (code != 200) {
    statusLine = "HTTP " + String(code);
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(40960);
  if (deserializeJson(doc, payload)) {
    statusLine = "JSON err";
    return false;
  }

  spotCount = 0;
  for (JsonObject obj : doc.as<JsonArray>()) {
    if (spotCount >= MAX_SPOTS) break;
    Spot& s     = spots[spotCount];
    String t    = obj["actTime"].as<String>();
    s.time      = (t.length() >= 16) ? t.substring(11, 16) : t;
    s.callsign  = obj["actCallsign"].as<String>();
    s.mode      = obj["actMode"].as<String>();
    s.freq      = obj["actFreq"].as<String>();
    s.cls       = obj["actClass"].as<String>();
    s.siteID    = obj["actSiteID"].as<String>();
    s.location  = obj["actLocation"].as<String>();
    spotCount++;
  }

  scrollIndex = 0;
  statusLine  = String(spotCount) + " spots";
  lastFetch   = millis();

  Serial.printf("[PNP] Fetched %d spots\n", spotCount);
  return true;
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);   // true = enable keyboard
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(1);

  // Load saved credentials from NVS
  prefs.begin("wifi", true);
  savedSSID = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  prefs.end();

  Serial.printf("[PNP] Saved SSID: '%s'\n", savedSSID.c_str());

  // Brief window to detect held 'W' key → force wizard
  bool forceWizard = false;
  {
    auto& d = M5Cardputer.Display;
    d.fillScreen(TFT_BLACK);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setCursor(4, 40);
    d.print("Hold W for Wi-Fi setup...");
    unsigned long t0 = millis();
    while (millis() - t0 < 2000) {
      M5Cardputer.update();
      if (M5Cardputer.Keyboard.isKeyPressed('w') ||
          M5Cardputer.Keyboard.isKeyPressed('W')) {
        forceWizard = true;
        break;
      }
      delay(20);
    }
  }

  if (forceWizard || savedSSID.length() == 0) {
    runWifiWizard();
  } else {
    connectSaved();
  }

  fetchSpots();
  needRedraw = true;
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
      if (scrollIndex > 0) { scrollIndex--; needRedraw = true; }
    }
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
      if (scrollIndex < max(0, spotCount - ROWS_VISIBLE)) { scrollIndex++; needRedraw = true; }
    }
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_ENTER)) {
      fetchSpots();
      needRedraw = true;
    }
    if (M5Cardputer.Keyboard.isKeyPressed('w') ||
        M5Cardputer.Keyboard.isKeyPressed('W')) {
      waitKeyRelease();
      runWifiWizard();
      fetchSpots();
      needRedraw = true;
    }
  }

  if (millis() - lastFetch > REFRESH_MS) {
    fetchSpots();
    needRedraw = true;
  }

  if (needRedraw) {
    drawSpotScreen();
    needRedraw = false;
  }

  delay(10);
}
