/*
  ARIA - ESP32 Dual-Sensor Telemetry Bridge
  ------------------------------------------
  Receives newline-terminated JSON telemetry from the EK-RA8D1 over UART
  and keeps ADS1263 and ICM-20948 data in completely separate live streams.

  Accepted RA8D1 packets:

    ADS1263:
      {"mode":"ads1263","model":"fast","confidence":0.105,
       "latency_ms":0,"voltage":-0.112}

    ICM-20948:
      {"mode":"icm20948","model":"balanced","confidence":0.742,
       "latency_ms":0,"voltage":0.000}

    Backward-compatible ICM mode:
      {"mode":"motion", ...}

  The dashboard receives:
    {
      "ads": { latest ADS data + ADS-only history },
      "imu": { latest IMU data + IMU-only history }
    }

  Required Arduino libraries:
    - ArduinoJson
    - WiFi
    - WebServer
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "dashboard_html.h"

// ---------------------------------------------------------------------
// CONFIG
// ---------------------------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_HERE";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

#define RA8D1_RX_PIN 16
#define RA8D1_TX_PIN 17
#define RA8D1_BAUD   115200

#define TOGGLE_BUTTON_PIN 0
#define DEBOUNCE_MS 250

#define HISTORY_LEN 60

// ---------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------
WebServer server(80);
HardwareSerial RA8D1Serial(2);

struct Reading {
  String mode;
  String model;
  float confidence;
  uint32_t latency_us;
  float latency_ms;
  float voltage;
  unsigned long ts;
};

struct StreamState {
  Reading latest;
  Reading history[HISTORY_LEN];
  int historyCount;
  int historyHead;
  bool hasData;
};

StreamState adsState = {
  {"ads1263", "unknown", 0.0f, 0U, 0.0f, 0.0f, 0UL},
  {},
  0,
  0,
  false
};

StreamState imuState = {
  {"icm20948", "unknown", 0.0f, 0U, 0.0f, 0.0f, 0UL},
  {},
  0,
  0,
  false
};

String uartLineBuffer;

unsigned long lastButtonPress = 0;
int lastButtonState = HIGH;

// Diagnostics
unsigned long totalBytesReceived = 0;
unsigned long totalLinesReceived = 0;
unsigned long totalParseFailures = 0;
unsigned long totalAdsPackets = 0;
unsigned long totalImuPackets = 0;
unsigned long totalUnknownPackets = 0;

String lastRawLine = "";
String lastParseError = "";
String lastPacketRoute = "none";

// ---------------------------------------------------------------------
// HELPERS
// ---------------------------------------------------------------------

static bool isAdsMode(const String& mode) {
  return mode.equalsIgnoreCase("ads1263");
}

static bool isImuMode(const String& mode) {
  return mode.equalsIgnoreCase("icm20948") ||
         mode.equalsIgnoreCase("motion");
}

static void pushHistory(StreamState& state, const Reading& r) {
  state.history[state.historyHead] = r;
  state.historyHead = (state.historyHead + 1) % HISTORY_LEN;

  if (state.historyCount < HISTORY_LEN) {
    state.historyCount++;
  }
}

static void updateStream(StreamState& state, const Reading& r) {
  state.latest = r;
  state.hasData = true;
  pushHistory(state, r);
}

void handleIncomingLine(const String& line) {
  lastRawLine = line;

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, line);

  if (err) {
    totalParseFailures++;
    lastParseError = err.c_str();

    Serial.print("[UART] JSON parse failed (");
    Serial.print(err.c_str());
    Serial.print("): ");
    Serial.println(line);
    return;
  }

  Reading r;
  r.mode       = doc["mode"]       | "unknown";
  r.model      = doc["model"]      | "unknown";
  r.confidence = doc["confidence"] | 0.0f;

  if (doc.containsKey("latency_us")) {
    r.latency_us = doc["latency_us"] | 0U;
    r.latency_ms = ((float)r.latency_us) / 1000.0f;
  }
  else {
    r.latency_ms = doc["latency_ms"] | 0.0f;
    r.latency_us = (uint32_t)(r.latency_ms * 1000.0f + 0.5f);
  }

  r.voltage = doc["voltage"] | 0.0f;
  r.ts         = millis();

  if (isAdsMode(r.mode)) {
    r.mode = "ads1263";
    updateStream(adsState, r);
    totalAdsPackets++;
    lastPacketRoute = "ADS1263";
  }
  else if (isImuMode(r.mode)) {
    // Normalize old "motion" packets so the web API always exposes
    // an explicit ICM-20948 sensor name.
    r.mode = "icm20948";
    updateStream(imuState, r);
    totalImuPackets++;
    lastPacketRoute = "ICM-20948";
  }
  else {
    totalUnknownPackets++;
    lastPacketRoute = "UNKNOWN";

    Serial.print("[UART] Ignoring unknown mode: ");
    Serial.println(r.mode);
  }
}

void pollUart() {
  while (RA8D1Serial.available()) {
    char c = (char)RA8D1Serial.read();
    totalBytesReceived++;

    if (c == '\n') {
      uartLineBuffer.trim();

      if (uartLineBuffer.length() > 0) {
        totalLinesReceived++;
        handleIncomingLine(uartLineBuffer);
      }

      uartLineBuffer = "";
    }
    else if (c != '\r') {
      uartLineBuffer += c;

      if (uartLineBuffer.length() > 512) {
        uartLineBuffer = "";
      }
    }
  }
}

void pollToggleButton() {
  int state = digitalRead(TOGGLE_BUTTON_PIN);
  unsigned long now = millis();

  if (state == LOW &&
      lastButtonState == HIGH &&
      (now - lastButtonPress) > DEBOUNCE_MS) {
    lastButtonPress = now;
    RA8D1Serial.println("TOGGLE");
    Serial.println("[BUTTON] TOGGLE sent to RA8D1");
  }

  lastButtonState = state;
}

// ---------------------------------------------------------------------
// JSON BUILDERS
// ---------------------------------------------------------------------

static void addHistoryToJson(JsonArray hist, const StreamState& state) {
  int start =
      (state.historyHead - state.historyCount + HISTORY_LEN) %
      HISTORY_LEN;

  for (int i = 0; i < state.historyCount; i++) {
    int idx = (start + i) % HISTORY_LEN;
    const Reading& r = state.history[idx];

    JsonObject h = hist.createNestedObject();
    h["model"] = r.model;
    h["confidence"] = r.confidence;
    h["latency_us"] = r.latency_us;
    h["latency_ms"] = r.latency_ms;
    h["voltage"] = r.voltage;
  }
}

static void addStreamToJson(
    JsonObject obj,
    const StreamState& state,
    const char* sensorName,
    const char* normalizedMode) {

  obj["sensor"] = sensorName;
  obj["mode"] = normalizedMode;
  obj["available"] = state.hasData;

  if (!state.hasData) {
    obj["model"] = "unknown";
    obj["confidence"] = 0.0f;
    obj["latency_us"] = 0U;
    obj["latency_ms"] = 0.0f;
    obj["voltage"] = 0.0f;
    obj["age_ms"] = 0U;
    obj.createNestedArray("history");
    return;
  }

  obj["model"] = state.latest.model;
  obj["confidence"] = state.latest.confidence;
  obj["latency_us"] = state.latest.latency_us;
  obj["latency_ms"] = state.latest.latency_ms;
  obj["voltage"] = state.latest.voltage;
  obj["age_ms"] = millis() - state.latest.ts;

  JsonArray hist = obj.createNestedArray("history");
  addHistoryToJson(hist, state);
}

// ---------------------------------------------------------------------
// WEB HANDLERS
// ---------------------------------------------------------------------

void handleTelemetry() {
  // Two histories of up to 60 readings each need more room than the old
  // single-stream document. Use heap-backed ArduinoJson storage on ESP32.
  DynamicJsonDocument doc(24576);

  JsonObject ads = doc.createNestedObject("ads");
  addStreamToJson(ads, adsState, "ADS1263", "ads1263");

  JsonObject imu = doc.createNestedObject("imu");
  addStreamToJson(imu, imuState, "ICM-20948", "icm20948");

  doc["uptime_ms"] = millis();

  String out;
  serializeJson(doc, out);

  server.sendHeader(
      "Cache-Control",
      "no-store, no-cache, must-revalidate");

  server.send(200, "application/json", out);
}

void handleRoot() {
  server.sendHeader(
      "Cache-Control",
      "no-store, no-cache, must-revalidate");

  server.send(200, "text/html", DASHBOARD_HTML);
}

void handleDebug() {
  DynamicJsonDocument doc(4096);

  doc["bytesReceived"] = totalBytesReceived;
  doc["linesReceived"] = totalLinesReceived;
  doc["parseFailures"] = totalParseFailures;

  doc["adsPackets"] = totalAdsPackets;
  doc["imuPackets"] = totalImuPackets;
  doc["unknownPackets"] = totalUnknownPackets;

  doc["lastPacketRoute"] = lastPacketRoute;
  doc["lastRawLine"] = lastRawLine;
  doc["lastParseError"] = lastParseError;

  doc["wifiStatus"] =
      (WiFi.status() == WL_CONNECTED)
          ? "connected"
          : "not connected";

  doc["wifiIP"] = WiFi.localIP().toString();
  doc["uptimeMs"] = millis();

  JsonObject ads = doc.createNestedObject("ads");
  ads["available"] = adsState.hasData;
  ads["model"] = adsState.latest.model;
  ads["confidence"] = adsState.latest.confidence;
  ads["latency_us"] = adsState.latest.latency_us;
  ads["latency_ms"] = adsState.latest.latency_ms;
  ads["voltage"] = adsState.latest.voltage;
  ads["age_ms"] =
      adsState.hasData
          ? (millis() - adsState.latest.ts)
          : 0U;

  JsonObject imu = doc.createNestedObject("imu");
  imu["available"] = imuState.hasData;
  imu["model"] = imuState.latest.model;
  imu["confidence"] = imuState.latest.confidence;
  imu["latency_us"] = imuState.latest.latency_us;
  imu["latency_ms"] = imuState.latest.latency_ms;
  imu["age_ms"] =
      imuState.hasData
          ? (millis() - imuState.latest.ts)
          : 0U;

  String out;
  serializeJson(doc, out);

  server.sendHeader(
      "Cache-Control",
      "no-store, no-cache, must-revalidate");

  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------
// SETUP / LOOP
// ---------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(TOGGLE_BUTTON_PIN, INPUT_PULLUP);

  RA8D1Serial.begin(
      RA8D1_BAUD,
      SERIAL_8N1,
      RA8D1_RX_PIN,
      RA8D1_TX_PIN);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Connected. Dashboard at: http://");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/telemetry", handleTelemetry);
  server.on("/debug", handleDebug);
  server.begin();
}

void loop() {
  pollUart();
  pollToggleButton();
  server.handleClient();
}