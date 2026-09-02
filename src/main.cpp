#include <TinyGPS++.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>

#define GREEN_LED 4
#define RED_LED   2

// GPS UART pins — matched to the existing hardware wiring
#define GPS_RX 16
#define GPS_TX 17

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
double lastLat = 0;
double lastLng = 0;

int lastSatCount = 0;
int lastPostCode = 0;

unsigned long lastPostTime = 0;

#define LOG_SIZE 12

String logBuffer[LOG_SIZE];
int logIndex = 0;


void addLog(String msg) {
  logBuffer[logIndex] =
      "[" + String(millis() / 1000) + "s] " + msg;

  logIndex = (logIndex + 1) % LOG_SIZE;

  Serial.println(msg);
}


// Escapes text for safe placement inside HTML content AND inside a
// double-quoted HTML attribute.
String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");

  return s;
}


// ---------------------------------------------------------------------
// PROVISIONING MODE
// ---------------------------------------------------------------------

void handleRoot() {

  String html =
      "<!DOCTYPE html><html><head>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>GPS Tracker Setup</title>"

      "<style>"
      "body{font-family:sans-serif;max-width:340px;margin:40px auto;padding:0 16px}"
      "input{width:100%;padding:8px;margin:6px 0 14px;box-sizing:border-box}"
      "button.submit{width:100%;padding:10px;background:#4C5FD5;color:#fff;border:none;border-radius:6px}"
      ".warn{background:#fdecea;color:#a33;padding:10px;border-radius:6px;margin-bottom:14px}"
      ".scan-box{margin-top:24px;padding-top:16px;border-top:1px solid #ddd}"
      ".net-btn{display:block;width:100%;text-align:left;padding:8px;margin:4px 0;"
      "background:#f2f2f2;border:1px solid #ddd;border-radius:6px;font-size:13px;cursor:pointer}"
      ".link{display:block;text-align:center;margin-top:10px;font-size:13px}"
      "</style>"

      "</head><body>";

  html += "<h2>GPS Tracker WiFi Setup</h2>";

  if (lastConnectFailed) {

    html +=
        "<div class='warn'>Couldn't connect to <b>" +
        htmlEscape(lastAttemptedSSID) +
        "</b>. Check the password and try again.</div>";
  }


  // Primary path: manually enter SSID
  html += "<form action='/save' method='POST'>";

  html += "<label>Network name (SSID)</label>";

  html +=
      "<input id='ssid' name='ssid' required autocomplete='off'>";

  html += "<label>Password</label>";

  html +=
      "<input name='password' type='password'>";

  html +=
      "<button class='submit' type='submit'>"
      "Save & Connect"
      "</button>";

  html += "</form>";


  // WiFi scan results
  html +=
      "<div class='scan-box'>"
      "<p><b>Detected networks</b> "
      "(tap to fill in the field above):</p>";


  int n = WiFi.scanNetworks();

  Serial.print("scanNetworks returned: ");
  Serial.println(n);


  if (n < 0) {

    html +=
        "<p style='color:#888;font-size:13px'>"
        "Scan error (code " +
        String(n) +
        "). You can still type your network name above manually."
        "</p>";

  } else if (n == 0) {

    html +=
        "<p style='color:#888;font-size:13px'>"
        "No networks detected. You can still type yours above manually."
        "</p>";

  } else {

    for (int i = 0; i < n; i++) {

      String safeSsid =
          htmlEscape(WiFi.SSID(i));

      html +=
          "<button type='button' "
          "class='net-btn' "
          "data-ssid=\"" +
          safeSsid +
          "\">" +
          safeSsid +
          " (" +
          String(WiFi.RSSI(i)) +
          " dBm)"
          "</button>";
    }
  }


  html += "</div>";

  html +=
      "<a class='link' href='/'>Rescan</a>";


  html +=
      "<script>"
      "document.querySelectorAll('.net-btn').forEach(function(btn){"
      "btn.addEventListener('click', function(){"
      "document.getElementById('ssid').value = "
      "btn.getAttribute('data-ssid');"
      "});"
      "});"
      "</script>";


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


  server.send(
      200,
      "text/html",
      "<body style='font-family:sans-serif;text-align:center;margin-top:60px'>"
      "<h3>Saved. Attempting to connect...</h3>"
      "</body>"
  );


  delay(1500);

  ESP.restart();
}


void startProvisioning() {

  provisioningMode = true;

  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH);


  WiFi.mode(WIFI_AP_STA);

  WiFi.softAP("GPS-Tracker-Setup");

  delay(500);


  Serial.print("PROVISIONING. Browse to: ");

  Serial.println(
      WiFi.softAPIP()
  );


  server.on("/", handleRoot);

  server.on(
      "/save",
      HTTP_POST,
      handleSave
  );

  server.begin();
}


// ---------------------------------------------------------------------
// STATUS PAGE
// ---------------------------------------------------------------------

void handleStatus() {

  String html =
      "<html><head>"
      "<meta http-equiv='refresh' content='5'>"

      "<style>"
      "body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 16px}"
      "pre{background:#f0f0f0;padding:10px;border-radius:6px;font-size:13px}"
      "a.reset{display:inline-block;margin-top:10px;color:#a33}"
      "</style>"

      "</head><body>";


  html += "<h2>GPS Tracker Status</h2>";


  html +=
      "<p><b>Network:</b> " +
      htmlEscape(WiFi.SSID()) +
      " (" +
      WiFi.localIP().toString() +
      ")</p>";


  html +=
      "<p><b>Last fix:</b> " +
      String(lastLat, 6) +
      ", " +
      String(lastLng, 6) +
      "</p>";


  html +=
      "<p><b>Satellites:</b> " +
      String(lastSatCount) +
      "</p>";


  html +=
      "<p><b>Last POST result:</b> " +
      String(lastPostCode) +
      " (" +
      String((millis() - lastPostTime) / 1000) +
      "s ago)</p>";


  html += "<h3>Recent log</h3><pre>";


  for (int i = 0; i < LOG_SIZE; i++) {

    int idx =
        (logIndex + i) % LOG_SIZE;

    if (logBuffer[idx].length() > 0) {

      html +=
          htmlEscape(logBuffer[idx]) +
          "\n";
    }
  }


  html += "</pre>";


  html +=
      "<a class='reset' href='/reset'>"
      "Forget WiFi & re-provision"
      "</a>";


  html += "</body></html>";


  server.send(
      200,
      "text/html",
      html
  );
}


void handleReset() {

  server.send(
      200,
      "text/html",
      "<body style='font-family:sans-serif;text-align:center;margin-top:60px'>"
      "<h3>WiFi forgotten. Rebooting into setup mode...</h3>"
      "</body>"
  );


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

  String ssid =
      prefs.getString("ssid", "");

  String password =
      prefs.getString("password", "");

  prefs.end();


  if (ssid.length() == 0) {
    return false;
  }


  lastAttemptedSSID = ssid;


  WiFi.mode(WIFI_STA);

  WiFi.begin(
      ssid.c_str(),
      password.c_str()
  );


  unsigned long start =
      millis();


  while (WiFi.status() != WL_CONNECTED) {

    if (millis() - start >
        WIFI_CONNECT_TIMEOUT_MS) {

      lastConnectFailed = true;

      return false;
    }

    delay(300);
  }


  addLog(
      "WiFi connected: " +
      WiFi.localIP().toString()
  );


  return true;
}


// ---------------------------------------------------------------------
// SEND GPS LOCATION TO API
// ---------------------------------------------------------------------

void postLocation(
    double lat,
    double lng
) {

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }


  WiFiClientSecure client;

  client.setInsecure();


  HTTPClient http;

  http.begin(
      client,
      API_URL
  );


  http.addHeader(
      "Content-Type",
      "application/json"
  );


  String body =
      "{\"deviceId\":\"" +
      String(DEVICE_ID) +
      "\",\"lat\":" +
      String(lat, 6) +
      ",\"lng\":" +
      String(lng, 6) +
      "}";


  lastPostCode =
      http.POST(body);


  lastPostTime =
      millis();


  addLog(
      "POST -> " +
      String(lastPostCode)
  );


  http.end();
}


// ---------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------

void setup() {

  Serial.begin(115200);


  pinMode(
      GREEN_LED,
      OUTPUT
  );

  pinMode(
      RED_LED,
      OUTPUT
  );


  digitalWrite(
      GREEN_LED,
      HIGH
  );

  digitalWrite(
      RED_LED,
      LOW
  );


  // GPS UART
  //
  // GPIO16 = ESP32 RX
  // GPIO17 = ESP32 TX
  //
  // GPS TX should therefore be connected to GPIO16.
  // GPS RX should therefore be connected to GPIO17.
  //
  GPS_Serial.begin(
      9600,
      SERIAL_8N1,
      GPS_RX,
      GPS_TX
  );


  delay(1000);


  if (!connectWithSavedCredentials()) {

    startProvisioning();

    return;
  }


  server.on(
      "/",
      handleStatus
  );

  server.on(
      "/reset",
      handleReset
  );


  server.begin();


  Serial.println(
      "Status page live at http://" +
      WiFi.localIP().toString() +
      "/"
  );
}


// ---------------------------------------------------------------------
// MAIN LOOP
// ---------------------------------------------------------------------

void loop() {

  server.handleClient();


  if (provisioningMode) {
    return;
  }


  // Read GPS UART
  while (GPS_Serial.available()) {

    char c =
        GPS_Serial.read();

    gps.encode(c);

    receivedGPSData = true;
  }


  // GPS has a valid location
  if (gps.location.isValid()) {

    digitalWrite(
        RED_LED,
        HIGH
    );


    lastLat =
        gps.location.lat();

    lastLng =
        gps.location.lng();

    lastSatCount =
        gps.satellites.value();


    if (millis() - lastStatus >= 3000) {

      lastStatus =
          millis();


      addLog(
          "GPS FIXED lat=" +
          String(lastLat, 6) +
          " lng=" +
          String(lastLng, 6) +
          " sats=" +
          String(lastSatCount)
      );
    }


    if (millis() - lastPost >=
        POST_INTERVAL_MS) {

      lastPost =
          millis();

      postLocation(
          lastLat,
          lastLng
      );
    }


  } else {

    digitalWrite(
        RED_LED,
        LOW
    );


    lastSatCount =
        gps.satellites.value();


    if (millis() - lastStatus >= 5000) {

      lastStatus =
          millis();


      addLog(
          receivedGPSData
              ? "Waiting for fix... sats=" +
                String(lastSatCount)
              : "NO GPS DATA. Check wiring."
      );
    }
  }
}

