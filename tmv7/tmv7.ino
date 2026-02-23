#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <WiFiClientSecureBearSSL.h>
#include <time.h>

// Hardware: sensors are ordered from top to bottom.
constexpr uint8_t SENSOR_PINS[4] = {D5, D6, D7, D2};
constexpr uint8_t STATUS_LED_PIN = D1;

// Adjust if your LED wiring or sensor output polarity is opposite.
constexpr uint8_t LED_ON_STATE = HIGH;
constexpr uint8_t LED_OFF_STATE = LOW;
constexpr bool SENSOR_INVERTED[4] = {false, false, false, false};

constexpr unsigned long SENSOR_POLL_MS = 1500UL;
constexpr unsigned long WIFI_RECONNECT_MS = 10000UL;
constexpr unsigned long WIFI_BLINK_MS = 250UL;

constexpr char AP_NAME[] = "TankMonitorAP";

// Update manifest format:
// version_code=2026022202
// version_name=7.1.2
// firmware_url=https://raw.githubusercontent.com/gavc/tmv71/main/ota/tmv7-2026022303.bin
constexpr long FW_VERSION_CODE = 2026022303;
constexpr char FW_VERSION_NAME[] = "7.1.5";
constexpr char UPDATE_MANIFEST_URL[] = "https://raw.githubusercontent.com/gavc/tmv71/main/ota/manifest.txt";

constexpr char NTP_SERVER[] = "pool.ntp.org";
constexpr long TIMEZONE_OFFSET_SEC = 0;
constexpr int DAYLIGHT_OFFSET_SEC = 0;
constexpr time_t MIN_VALID_EPOCH = 1704067200;  // 2024-01-01 00:00:00 UTC
constexpr unsigned long CLOCK_RESYNC_MS = 21600000UL;  // 6 hours
constexpr unsigned long CLOCK_RETRY_MS = 60000UL;      // retry every 60s until synced

ESP8266WebServer server(80);
WiFiManager wifiManager;

struct SensorSnapshot {
  bool wet[4];
  uint8_t wetCount;
  uint8_t tankPercent;
  unsigned long sampledAtMs;
};

struct UpdateManifest {
  long versionCode;
  String versionName;
  String firmwareUrl;
};

SensorSnapshot snapshot = {{false, false, false, false}, 0, 0, 0};
UpdateManifest pendingUpdate = {0, "", ""};

bool updateAvailable = false;
String updateStatus = "No update check yet.";
bool sensorInitialized[4] = {false, false, false, false};
time_t sensorChangedAtEpoch[4] = {0, 0, 0, 0};
unsigned long sensorChangedAtMs[4] = {0, 0, 0, 0};

unsigned long lastSensorPollMs = 0;
unsigned long lastReconnectMs = 0;
unsigned long lastBlinkMs = 0;
unsigned long lastClockSyncMs = 0;
bool blinkState = false;

bool intervalElapsed(unsigned long lastMs, unsigned long intervalMs) {
  return (unsigned long)(millis() - lastMs) >= intervalMs;
}

void setStatusLed(bool on) {
  digitalWrite(STATUS_LED_PIN, on ? LED_ON_STATE : LED_OFF_STATE);
}

bool isTimeSynced() {
  return time(nullptr) >= MIN_VALID_EPOCH;
}

void syncClock() {
  configTime(TIMEZONE_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  lastClockSyncMs = millis();
}

void backfillSensorEpochTimes() {
  if (!isTimeSynced()) {
    return;
  }

  const time_t nowEpoch = time(nullptr);
  const unsigned long nowMs = millis();

  for (uint8_t i = 0; i < 4; i++) {
    if (!sensorInitialized[i] || sensorChangedAtMs[i] == 0 || sensorChangedAtEpoch[i] >= MIN_VALID_EPOCH) {
      continue;
    }

    const unsigned long ageMs = (unsigned long)(nowMs - sensorChangedAtMs[i]);
    const time_t ageSec = (time_t)(ageMs / 1000UL);
    sensorChangedAtEpoch[i] = nowEpoch > ageSec ? (nowEpoch - ageSec) : nowEpoch;
  }
}

String formatEpoch(time_t t) {
  if (t <= 0) {
    return "-";
  }
  struct tm tmInfo;
  if (!localtime_r(&t, &tmInfo)) {
    return "-";
  }
  char buf[20];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmInfo);
  return String(buf);
}

String sensorChangeTimeToStr(uint8_t idx) {
  if (idx > 3 || sensorChangedAtMs[idx] == 0) {
    return "-";
  }

  if (isTimeSynced() && sensorChangedAtEpoch[idx] >= MIN_VALID_EPOCH) {
    return formatEpoch(sensorChangedAtEpoch[idx]);
  }

  return "t+" + String(sensorChangedAtMs[idx] / 1000UL) + "s";
}

uint8_t rssiToBars(int rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

bool readStableSensorWet(uint8_t sensorIdx) {
  constexpr uint8_t sampleCount = 5;
  uint8_t highCount = 0;
  for (uint8_t i = 0; i < sampleCount; i++) {
    if (digitalRead(SENSOR_PINS[sensorIdx]) == HIGH) {
      highCount++;
    }
    delay(2);
  }

  bool wet = highCount >= 3;
  if (SENSOR_INVERTED[sensorIdx]) {
    wet = !wet;
  }
  return wet;
}

uint8_t calcTankPercent(const bool wet[4]) {
  uint8_t filledBands = 0;
  for (int i = 3; i >= 0; i--) {
    if (wet[i]) {
      filledBands++;
    } else {
      break;
    }
  }
  return filledBands * 25;
}

void pollSensors() {
  const unsigned long nowMs = millis();
  const time_t nowEpoch = time(nullptr);

  snapshot.wetCount = 0;
  for (uint8_t i = 0; i < 4; i++) {
    bool wetNow = readStableSensorWet(i);
    if (!sensorInitialized[i] || wetNow != snapshot.wet[i]) {
      sensorChangedAtMs[i] = nowMs;
      sensorChangedAtEpoch[i] = nowEpoch;
      sensorInitialized[i] = true;
    }
    snapshot.wet[i] = wetNow;
    if (snapshot.wet[i]) {
      snapshot.wetCount++;
    }
  }
  snapshot.tankPercent = calcTankPercent(snapshot.wet);
  snapshot.sampledAtMs = millis();
}

String sensorLabel(uint8_t idx) {
  static const char* labels[4] = {"Top", "Upper-mid", "Lower-mid", "Bottom"};
  if (idx > 3) {
    return "Unknown";
  }
  return String(labels[idx]);
}

String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

void sendHtml(const String& html) {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(200, "text/html; charset=utf-8", html);
}

void redirectHome() {
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "Redirecting...");
}

bool parseManifest(const String& body, UpdateManifest& out, String& error) {
  out.versionCode = 0;
  out.versionName = "";
  out.firmwareUrl = "";

  int start = 0;
  while (start < body.length()) {
    int end = body.indexOf('\n', start);
    if (end < 0) {
      end = body.length();
    }

    String line = body.substring(start, end);
    line.trim();
    start = end + 1;

    if (line.length() == 0 || line.startsWith("#")) {
      continue;
    }

    int eq = line.indexOf('=');
    if (eq <= 0) {
      continue;
    }

    String key = line.substring(0, eq);
    String value = line.substring(eq + 1);
    key.trim();
    value.trim();

    key.toLowerCase();
    if (key == "version_code") {
      out.versionCode = value.toInt();
    } else if (key == "version_name") {
      out.versionName = value;
    } else if (key == "firmware_url") {
      out.firmwareUrl = value;
    }
  }

  if (out.versionCode <= 0) {
    error = "manifest missing valid version_code";
    return false;
  }
  if (out.firmwareUrl.length() == 0) {
    error = "manifest missing firmware_url";
    return false;
  }
  if (out.versionName.length() == 0) {
    out.versionName = String(out.versionCode);
  }
  return true;
}

bool fetchTextFromUrl(const String& url, String& body, String& error) {
  HTTPClient http;
  int code = 0;

  if (url.startsWith("https://")) {
    BearSSL::WiFiClientSecure secureClient;
    secureClient.setInsecure();
    if (!http.begin(secureClient, url)) {
      error = "cannot open URL";
      return false;
    }
    code = http.GET();
  } else {
    WiFiClient client;
    if (!http.begin(client, url)) {
      error = "cannot open URL";
      return false;
    }
    code = http.GET();
  }

  if (code != HTTP_CODE_OK) {
    error = "HTTP " + String(code);
    http.end();
    return false;
  }

  body = http.getString();
  http.end();
  return true;
}

bool fetchManifest(UpdateManifest& out, String& error) {
  String body;
  if (!fetchTextFromUrl(String(UPDATE_MANIFEST_URL), body, error)) {
    return false;
  }

  return parseManifest(body, out, error);
}

void handleRoot() {
  const bool wifiOk = (WiFi.status() == WL_CONNECTED);
  const int rssi = wifiOk ? WiFi.RSSI() : -100;
  const uint8_t signalBars = wifiOk ? rssiToBars(rssi) : 0;
  const String signalTooltip = wifiOk ? (String(rssi) + " dBm") : "offline";

  String html;
  html.reserve(7000);
  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Tank Monitor</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;background:#f4f6f8;color:#1f2937;margin:0;padding:12px;}";
  html += ".card{max-width:720px;margin:0 auto;background:#fff;border:1px solid #e5e7eb;border-radius:10px;padding:12px;}";
  html += "h1{font-size:1.2rem;margin:0;}";
  html += "p{margin:.35rem 0;}";
  html += ".topline{display:flex;justify-content:space-between;align-items:flex-start;gap:10px;margin-bottom:8px;}";
  html += ".wet{color:#0f766e;font-weight:700;}.dry{color:#b91c1c;font-weight:700;}";
  html += ".tank-wrap{margin:10px auto 6px auto;max-width:300px;}";
  html += ".tank{border:2px solid #334155;border-radius:12px 12px 6px 6px;overflow:hidden;}";
  html += ".tank-band{min-height:64px;padding:8px;border-top:1px solid #cbd5e1;display:flex;flex-direction:column;justify-content:center;}";
  html += ".tank-band:first-child{border-top:0;}";
  html += ".tank-band.wet{background:#dbeafe;}";
  html += ".tank-band.dry{background:#f8fafc;}";
  html += ".band-title{font-weight:700;font-size:.92rem;}";
  html += ".band-time{font-size:.8rem;color:#334155;margin-top:3px;word-break:break-word;}";
  html += ".actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px;}";
  html += "button{padding:10px 12px;border:none;border-radius:8px;background:#2563eb;color:#fff;font-weight:700;cursor:pointer;}";
  html += "button.alt{background:#0f766e;}";
  html += ".note{margin-top:10px;padding:9px;border-radius:8px;background:#f8fafc;border:1px solid #e5e7eb;font-size:.9rem;word-break:break-word;}";
  html += ".signal-wrap{display:inline-flex;align-items:flex-end;}";
  html += ".signal-bars{display:inline-flex;align-items:flex-end;gap:2px;height:14px;}";
  html += ".bar{display:inline-block;width:4px;background:#cbd5e1;border-radius:2px;}";
  html += ".bar.b1{height:4px;}.bar.b2{height:7px;}.bar.b3{height:10px;}.bar.b4{height:13px;}";
  html += ".bar.on{background:#16a34a;}";
  html += "@media(max-width:520px){body{padding:8px;}.card{padding:10px;}.tank-band{min-height:58px;}.actions button{width:100%;}}";
  html += "</style></head><body><div class='card'>";

  html += "<div class='topline'><h1>Tank Monitor</h1><span class='signal-wrap' aria-label='Signal strength' title='" + htmlEscape(signalTooltip) + "'><span class='signal-bars'>";
  for (uint8_t i = 0; i < 4; i++) {
    html += "<span class='bar b" + String(i + 1);
    if (wifiOk && i < signalBars) {
      html += " on";
    }
    html += "'></span>";
  }
  html += "</span></span></div>";

  html += "<div class='tank-wrap'><div class='tank'>";
  for (uint8_t i = 0; i < 4; i++) {
    const bool wet = snapshot.wet[i];
    const String statusClass = wet ? "wet" : "dry";
    const String statusText = wet ? "WET" : "DRY";
    html += "<div class='tank-band " + statusClass + "'>";
    html += "<div class='band-title'>" + sensorLabel(i) + " - <span class='" + statusClass + "'>" + statusText + "</span></div>";
    html += "<div class='band-time'>Triggered: " + htmlEscape(sensorChangeTimeToStr(i)) + "</div>";
    html += "</div>";
  }
  html += "</div></div>";

  html += "<p style='margin-top:10px'><strong>Firmware:</strong> ";
  html += String(FW_VERSION_NAME) + " (" + String(FW_VERSION_CODE) + ")</p>";

  html += "<div class='actions'>";
  html += "<form action='/check-update' method='post'><button type='submit'>Check for Update</button></form>";
  if (updateAvailable) {
    html += "<form action='/install-update' method='post'><button class='alt' type='submit'>Install Update</button></form>";
  }
  html += "</div>";

  if (!isTimeSynced()) {
    html += "<div class='note'>Clock is still syncing. Last-change values are uptime (t+seconds).</div>";
  }
  html += "<div class='note'><strong>Update status:</strong> " + htmlEscape(updateStatus) + "</div>";
  html += "<div class='note'>Manifest URL: " + htmlEscape(String(UPDATE_MANIFEST_URL)) + "</div>";
  html += "</div></body></html>";

  sendHtml(html);
}

void handleCheckUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    updateAvailable = false;
    updateStatus = "Cannot check update: WiFi is disconnected.";
    redirectHome();
    return;
  }

  UpdateManifest latest;
  String error;
  if (!fetchManifest(latest, error)) {
    updateAvailable = false;
    updateStatus = "Update check failed: " + error;
    redirectHome();
    return;
  }

  if (latest.versionCode > FW_VERSION_CODE) {
    pendingUpdate = latest;
    updateAvailable = true;
    updateStatus = "Update found: " + latest.versionName + " (" + String(latest.versionCode) + ").";
  } else {
    updateAvailable = false;
    pendingUpdate = {0, "", ""};
    updateStatus = "No new update. Device is current.";
  }

  redirectHome();
}

void handleInstallUpdate() {
  if (!updateAvailable) {
    updateStatus = "No pending update. Run check first.";
    redirectHome();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    updateStatus = "Cannot install update: WiFi is disconnected.";
    redirectHome();
    return;
  }

  String otaHtml;
  otaHtml.reserve(700);
  otaHtml += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  otaHtml += "<title>OTA Update</title><style>body{font-family:Arial,sans-serif;margin:24px;color:#0f172a;}h1{font-size:1.2rem;margin:0 0 8px;}p{margin:8px 0;}";
  otaHtml += "#count{font-weight:700;font-size:1.1rem;}</style></head><body>";
  otaHtml += "<h1>Starting OTA update</h1>";
  otaHtml += "<p>Device will reboot if successful.</p>";
  otaHtml += "<p>Returning to main page in <span id='count'>30</span>s...</p>";
  otaHtml += "<script>var s=30;var el=document.getElementById('count');setInterval(function(){s--;if(s>=0){el.textContent=s;}},1000);";
  otaHtml += "setTimeout(function(){window.location.href='/'},30000);</script></body></html>";
  server.send(200, "text/html", otaHtml);
  delay(200);

  t_httpUpdate_return result;
  if (pendingUpdate.firmwareUrl.startsWith("https://")) {
    BearSSL::WiFiClientSecure secureClient;
    secureClient.setInsecure();
    result = ESPhttpUpdate.update(secureClient, pendingUpdate.firmwareUrl);
  } else {
    WiFiClient client;
    result = ESPhttpUpdate.update(client, pendingUpdate.firmwareUrl);
  }

  if (result == HTTP_UPDATE_FAILED) {
    updateStatus = "OTA failed (" + String(ESPhttpUpdate.getLastError()) + "): " + ESPhttpUpdate.getLastErrorString();
    updateAvailable = false;
  } else if (result == HTTP_UPDATE_NO_UPDATES) {
    updateStatus = "OTA result: no updates returned by server.";
    updateAvailable = false;
  } else {
    // On success, ESP8266 reboots before this line.
    updateStatus = "OTA reported success.";
  }
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  wifiManager.setConfigPortalTimeout(180);
  wifiManager.setConnectTimeout(30);

  bool ok = wifiManager.autoConnect(AP_NAME);
  if (!ok) {
    delay(500);
    ESP.restart();
  }
}

void refreshConnectivity() {
  if (WiFi.status() == WL_CONNECTED) {
    setStatusLed(true);
    return;
  }

  if (intervalElapsed(lastBlinkMs, WIFI_BLINK_MS)) {
    blinkState = !blinkState;
    setStatusLed(blinkState);
    lastBlinkMs = millis();
  }

  if (intervalElapsed(lastReconnectMs, WIFI_RECONNECT_MS)) {
    WiFi.reconnect();
    lastReconnectMs = millis();
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  for (uint8_t i = 0; i < 4; i++) {
    pinMode(SENSOR_PINS[i], INPUT);
  }
  pinMode(STATUS_LED_PIN, OUTPUT);
  setStatusLed(false);

  connectWiFi();
  syncClock();
  pollSensors();

  updateStatus = "Device ready. Run 'Check for Update' when needed.";

  server.on("/", HTTP_GET, handleRoot);
  server.on("/check-update", HTTP_POST, handleCheckUpdate);
  server.on("/install-update", HTTP_POST, handleInstallUpdate);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.print("Web UI: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();
  refreshConnectivity();

  if (WiFi.status() == WL_CONNECTED) {
    const unsigned long syncInterval = isTimeSynced() ? CLOCK_RESYNC_MS : CLOCK_RETRY_MS;
    if (intervalElapsed(lastClockSyncMs, syncInterval)) {
      syncClock();
    }
    backfillSensorEpochTimes();
  }

  if (intervalElapsed(lastSensorPollMs, SENSOR_POLL_MS)) {
    pollSensors();
    lastSensorPollMs = millis();
  }
}
