#include <TinyGPS++.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>

#define GREEN_LED 4
#define RED_LED   2
#define GPS_RX 19
#define GPS_TX 18

const char* API_URL = "https://gps-tracker-o2ug.onrender.com/api/locations";
const char* DEVICE_ID = "esp32-01";

const unsigned long POST_INTERVAL_MS = 5000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
unsigned long lastPost = 0;
unsigned long lastStatus = 0;
bool receivedGPSData = false;

TinyGPSPlus gps;
HardwareSerial GPS_Serial(2);
Preferences prefs;
WebServer server(80);
bool provisioningMode = false;

// --- provisioning-failure feedback (RAM only — valid for this boot cycle) ---
bool lastConnectFailed = false;
String lastAttemptedSSID = "";

// --- live status state ---
double lastLat = 0, lastLng = 0;
int lastSatCount = 0;
int lastPostCode = 0;
unsigned long lastPostTime = 0;

#define LOG_SIZE 12
String logBuffer[LOG_SIZE];
int logIndex = 0;

void addLog(String msg) {
  logBuffer[logIndex] = "[" + String(millis() / 1000) + "s] " + msg;
  logIndex = (logIndex + 1) % LOG_SIZE;
  Serial.println(msg);
}

// ---------------------------------------------------------------------
// PROVISIONING MODE — scan + form
// ---------------------------------------------------------------------

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
                 "<title>GPS Tracker Setup</title>"
                 "<style>body{font-family:sans-serif;max-width:340px;margin:40px auto;padding:0 16px}"
                 "select,input{width:100%;padding:8px;margin:6px 0 14px;box-sizing:border-box}"
                 "button{width:100%;padding:10px;background:#4C5FD5;color:#fff;border:none;border-radius:6px}"
                 ".warn{background:#fdecea;color:#a33;padding:10px;border-radius:6px;margin-bottom:14px}"
                 ".link{display:block;text-align:center;margin-top:10px;font-size:13px}</style></head><body>";

  html += "<h2>GPS Tracker WiFi Setup</h2>";

  if (lastConnectFailed) {
    html += "<div class='warn'>Couldn't connect to <b>" + lastAttemptedSSID +
            "</b>. Check the password and try again.</div>";
  }

  html += "<form action='/save' method='POST'><label>Network</label><select name='ssid'>";

  int n = WiFi.scanNetworks();
  if (n == 0) {
    html += "<option value=''>No networks found — rescan?</option>";
  } else {
    for (int i = 0; i < n; i++) {
      html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) +
              " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
  }
  html += "</select>";
  html += "<label>Password</label><input name='password' type='password'>";
  html += "<button type='submit'>Save & Connect</button></form>";
  html += "<a class='link' href='/'>Rescan networks</a>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("password", password);
  prefs.end();
  server.send(200, "text/html",
    "<body style='font-family:sans-serif;text-align:center;margin-top:60px'>"
    "<h3>Saved. Attempting to connect...</h3></body>");
  delay(1500);
  ESP.restart();
}

void startProvisioning() {
  provisioningMode = true;
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);

  WiFi.mode(WIFI_AP_STA);  // AP for the setup page, STA half free to scan
  WiFi.softAP("GPS-Tracker-Setup");
  Serial.print("PROVISIONING. Browse to: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

// ---------------------------------------------------------------------
// STATUS PAGE — active once connected as a station
// ---------------------------------------------------------------------

void handleStatus() {
  String html = "<html><head><meta http-equiv='refresh' content='5'>"
                 "<style>body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 16px}"
                 "pre{background:#f0f0f0;padding:10px;border-radius:6px;font-size:13px}"
                 "a.reset{display:inline-block;margin-top:10px;color:#a33}</style></head><body>";
  html += "<h2>GPS Tracker Status</h2>";
  html += "<p><b>Network:</b> " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")</p>";
  html += "<p><b>Last fix:</b> " + String(lastLat, 6) + ", " + String(lastLng, 6) + "</p>";
  html += "<p><b>Satellites:</b> " + String(lastSatCount) + "</p>";
  html += "<p><b>Last POST result:</b> " + String(lastPostCode) +
          " (" + String((millis() - lastPostTime) / 1000) + "s ago)</p>";
  html += "<h3>Recent log</h3><pre>";
  for (int i = 0; i < LOG_SIZE; i++) {
    int idx = (logIndex + i) % LOG_SIZE;
    if (logBuffer[idx].length() > 0) html += logBuffer[idx] + "\n";
  }
  html += "</pre>";
  html += "<a class='reset' href='/reset'>Forget WiFi & re-provision</a>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleReset() {
  server.send(200, "text/html",
    "<body style='font-family:sans-serif;text-align:center;margin-top:60px'>"
    "<h3>WiFi forgotten. Rebooting into setup mode...</h3></body>");
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  delay(1500);
  ESP.restart();
}

// ---------------------------------------------------------------------
// NORMAL MODE — station connect + GPS + POST
// ---------------------------------------------------------------------

bool connectWithSavedCredentials() {
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String password = prefs.getString("password", "");
  prefs.end();
  if (ssid.length() == 0) return false;

  lastAttemptedSSID = ssid;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      lastConnectFailed = true;
      return false;
    }
    delay(300);
  }
  addLog("WiFi connected: " + WiFi.localIP().toString());
  return true;
}

void postLocation(double lat, double lng) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, API_URL);
  http.addHeader("Content-Type", "application/json");
  String body = "{\"deviceId\":\"" + String(DEVICE_ID) +
                "\",\"lat\":" + String(lat, 6) +
                ",\"lng\":" + String(lng, 6) + "}";
  lastPostCode = http.POST(body);
  lastPostTime = millis();
  addLog("POST -> " + String(lastPostCode));
  http.end();
}

void setup() {
  Serial.begin(115200);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(RED_LED, LOW);
  GPS_Serial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  delay(1000);

  if (!connectWithSavedCredentials()) {
    startProvisioning();
    return;
  }

  server.on("/", handleStatus);
  server.on("/reset", handleReset);
  server.begin();
  Serial.println("Status page live at http://" + WiFi.localIP().toString() + "/");
}

void loop() {
  server.handleClient();
  if (provisioningMode) return;

  while (GPS_Serial.available()) {
    char c = GPS_Serial.read();
    gps.encode(c);
    receivedGPSData = true;
  }

  if (gps.location.isValid()) {
    digitalWrite(RED_LED, HIGH);
    lastLat = gps.location.lat();
    lastLng = gps.location.lng();
    lastSatCount = gps.satellites.value();

    if (millis() - lastStatus >= 3000) {
      lastStatus = millis();
      addLog("GPS FIXED lat=" + String(lastLat, 6) + " lng=" + String(lastLng, 6) +
             " sats=" + String(lastSatCount));
    }
    if (millis() - lastPost >= POST_INTERVAL_MS) {
      lastPost = millis();
      postLocation(lastLat, lastLng);
    }
  } else {
    digitalWrite(RED_LED, LOW);
    lastSatCount = gps.satellites.value();
    if (millis() - lastStatus >= 5000) {
      lastStatus = millis();
      addLog(receivedGPSData ? "Waiting for fix... sats=" + String(lastSatCount) : "NO GPS DATA. Check wiring.");
    }
  }
}