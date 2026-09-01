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

// --- Server ---
const char* API_URL = "https://gps-tracker-o2ug.onrender.com/api/locations";
const char* DEVICE_ID = "esp32-01";

// --- Timing ---
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

// ---------------------------------------------------------------------
// PROVISIONING MODE — device becomes its own WiFi network + web form
// ---------------------------------------------------------------------

const char* setupPage = R"rawliteral(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1">
<title>GPS Tracker Setup</title>
<style>body{font-family:sans-serif;max-width:340px;margin:40px auto;padding:0 16px}
input{width:100%;padding:8px;margin:6px 0 14px;box-sizing:border-box}
button{width:100%;padding:10px;background:#4C5FD5;color:#fff;border:none;border-radius:6px}</style>
</head><body>
<h2>GPS Tracker WiFi Setup</h2>
<form action="/save" method="POST">
  <label>Network name (SSID)</label>
  <input name="ssid" required>
  <label>Password</label>
  <input name="password" type="password">
  <button type="submit">Save & Connect</button>
</form>
</body></html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", setupPage);
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
    "<h3>Saved. Rebooting into normal mode...</h3></body>");

  delay(1500);
  ESP.restart();
}

void startProvisioning() {
  provisioningMode = true;
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);  // solid red = "needs setup"

  WiFi.mode(WIFI_AP);
  WiFi.softAP("GPS-Tracker-Setup");  // open network, no password, for easy first setup

  Serial.println("== PROVISIONING MODE ==");
  Serial.print("Connect to WiFi network: GPS-Tracker-Setup, then browse to ");
  Serial.println(WiFi.softAPIP());  // normally 192.168.4.1

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

// ---------------------------------------------------------------------
// NORMAL MODE — station connect + GPS + POST
// ---------------------------------------------------------------------

bool connectWithSavedCredentials() {
  prefs.begin("wifi", true);  // read-only
  String ssid = prefs.getString("ssid", "");
  String password = prefs.getString("password", "");
  prefs.end();

  if (ssid.length() == 0) {
    Serial.println("No saved WiFi credentials.");
    return false;
  }

  Serial.print("Connecting to saved network: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("WiFi connect timed out.");
      return false;
    }
    delay(300);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
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

  int httpCode = http.POST(body);
  Serial.print("POST -> ");
  Serial.println(httpCode);

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

  Serial.println("================================");
  Serial.println("ESP32 + NEO-6M GPS TRACKER");
  Serial.println("================================");

  if (!connectWithSavedCredentials()) {
    startProvisioning();
    return;  // don't fall through to GPS/normal loop this boot
  }

  Serial.println("Waiting for GPS data...");
}

void loop() {
  if (provisioningMode) {
    server.handleClient();  // must be serviced constantly, same principle as webSocket.loop()
    return;
  }

  while (GPS_Serial.available()) {
    char c = GPS_Serial.read();
    gps.encode(c);
    receivedGPSData = true;
    Serial.write(c);
  }

  if (gps.location.isValid()) {
    digitalWrite(RED_LED, HIGH);

    if (millis() - lastStatus >= 3000) {
      lastStatus = millis();
      Serial.print("FIX  lat=");
      Serial.print(gps.location.lat(), 6);
      Serial.print(" lng=");
      Serial.println(gps.location.lng(), 6);
    }

    if (millis() - lastPost >= POST_INTERVAL_MS) {
      lastPost = millis();
      postLocation(gps.location.lat(), gps.location.lng());
    }
  } else {
    digitalWrite(RED_LED, LOW);
  }

  if (millis() - lastStatus >= 5000) {
    lastStatus = millis();
    Serial.println(receivedGPSData ? "Waiting for satellite FIX..." : "NO GPS DATA. Check wiring.");
  }
}