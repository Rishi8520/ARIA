/*
  ARIA - ESP32 Telemetry Bridge
  ------------------------------
  Role: UART <-> Wi-Fi bridge + physical toggle button reader.

  Responsibilities:
    1. Read JSON telemetry lines sent by the RA8D1 over UART (Serial2):
         {"mode":"vision","model":"fast","confidence":0.93,"latency_ms":7}
    2. Keep a rolling history (for the latency/confidence graph).
    3. Serve a simple live-updating dashboard over Wi-Fi (polling, no
       external frameworks -> works from a phone browser with zero setup).
    4. Read a physical push button. On press, send "TOGGLE\n" back to the
       RA8D1 over UART so it can switch between Demo A (Vision) and
       Demo B (Motion/Vibration).

  Library dependencies (install via Arduino Library Manager):
    - ArduinoJson (by Benoit Blanchon)  -- JSON parsing

  Wiring:
    - RA8D1 TX  -> ESP32 GPIO16 (RX2)
    - RA8D1 RX  -> ESP32 GPIO17 (TX2)   (through a level shifter if RA8D1 is 5V logic)
    - GND       <-> GND                (common ground, mandatory)
    - Toggle button: one leg -> GPIO0 (or any free GPIO), other leg -> GND
      (uses internal pull-up, so button press pulls the pin LOW)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "dashboard_html.h"

// ---------------------------------------------------------------------
// CONFIG - edit these for your network
// ---------------------------------------------------------------------
const char* WIFI_SSID     = "YOURWIFINAME";
const char* WIFI_PASSWORD = "WIFIPASSWORD";

// UART pins used to talk to the RA8D1
#define RA8D1_RX_PIN 16  // ESP32 receives here (connect to RA8D1 TX)
#define RA8D1_TX_PIN 17  // ESP32 transmits here (connect to RA8D1 RX)
#define RA8D1_BAUD   115200

// Physical toggle button
#define TOGGLE_BUTTON_PIN 0
#define DEBOUNCE_MS 250

// How many past readings to keep for the rolling graph
#define HISTORY_LEN 60

// ---------------------------------------------------------------------
// STATE
// ---------------------------------------------------------------------
WebServer server(80);
HardwareSerial RA8D1Serial(2);   // UART2

struct Reading {
  String  mode;         // "vision" or "motion"
  String  model;        // "fast" | "balanced" | "accurate"
  float   confidence;   // 0.0 - 1.0
  uint32_t latency_ms;
  unsigned long ts;      // millis() when received
};

Reading latest = {"unknown", "unknown", 0.0f, 0, 0};
Reading history[HISTORY_LEN];
int historyCount = 0;   // number of valid entries
int historyHead  = 0;   // next write index (ring buffer)

String uartLineBuffer;

unsigned long lastButtonPress = 0;
int lastButtonState = HIGH;

// ---- Diagnostics: visibility into what's actually arriving on UART ----
unsigned long totalBytesReceived = 0;
unsigned long totalLinesReceived = 0;   // newline-terminated, regardless of parse success
unsigned long totalParseFailures = 0;
String lastRawLine = "";                // last complete line seen, good or bad
String lastParseError = "";

// ---------------------------------------------------------------------
// HELPERS
// ---------------------------------------------------------------------

void pushHistory(const Reading& r) {
  history[historyHead] = r;
  historyHead = (historyHead + 1) % HISTORY_LEN;
  if (historyCount < HISTORY_LEN) historyCount++;
}

// Parse one JSON line coming from the RA8D1 and update state.
// Expected shape: {"mode":"vision","model":"fast","confidence":0.93,"latency_ms":7}
void handleIncomingLine(const String& line) {
  lastRawLine = line;

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    totalParseFailures++;
    lastParseError = err.c_str();
    Serial.print("[UART] JSON parse failed (");
    Serial.print(err.c_str());
    Serial.print(") on line: \"");
    Serial.print(line);
    Serial.println("\"");
    return;
  }

  // Parsed successfully, but confirm the fields we actually need are present.
  // A line like "{}" or "{\"foo\":1}" parses as valid JSON but carries no
  // useful telemetry -- surface that distinction instead of silently
  // falling back to defaults, so a schema mismatch is visible in Serial.
  if (!doc.containsKey("mode") || !doc.containsKey("model")) {
    Serial.print("[UART] JSON parsed but missing expected keys: \"");
    Serial.print(line);
    Serial.println("\"");
  }

  Reading r;
  r.mode        = doc["mode"]        | "unknown";
  r.model       = doc["model"]       | "unknown";
  r.confidence  = doc["confidence"]  | 0.0f;
  r.latency_ms  = doc["latency_ms"]  | 0;
  r.ts          = millis();

  latest = r;
  pushHistory(r);
}

// Poll UART for newline-terminated JSON messages.
// DIAGNOSTIC MODE: logs every raw byte received (hex + printable char) so
// we can tell "no bytes arriving at all" apart from "bytes arriving but
// garbled/wrong baud/wrong pins". Remove the per-byte Serial prints once
// real telemetry is confirmed flowing -- they're verbose and will slow
// down high-rate UART traffic.
void pollUart() {
    while (RA8D1Serial.available()) {
        char c = RA8D1Serial.read();

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

            /* Safety cap: discard a malformed/oversized line. */
            if (uartLineBuffer.length() > 512) {
                uartLineBuffer = "";
            }
        }
    }
}

// Read the physical toggle button (active LOW, debounced)
void pollToggleButton() {
  int state = digitalRead(TOGGLE_BUTTON_PIN);
  unsigned long now = millis();

  if (state == LOW && lastButtonState == HIGH && (now - lastButtonPress) > DEBOUNCE_MS) {
    lastButtonPress = now;
    RA8D1Serial.println("TOGGLE");
    Serial.println("[BUTTON] Toggle sent to RA8D1");
  }
  lastButtonState = state;
}

// ---------------------------------------------------------------------
// WEB HANDLERS
// ---------------------------------------------------------------------

// Serves the latest reading as JSON: used by the dashboard's poll loop
void handleTelemetry() {
  StaticJsonDocument<512> doc;
  doc["mode"]        = latest.mode;
  doc["model"]       = latest.model;
  doc["confidence"]  = latest.confidence;
  doc["latency_ms"]  = latest.latency_ms;
  doc["age_ms"]       = millis() - latest.ts;

  JsonArray hist = doc.createNestedArray("history");
  // walk the ring buffer oldest -> newest
  int start = (historyHead - historyCount + HISTORY_LEN) % HISTORY_LEN;
  for (int i = 0; i < historyCount; i++) {
    int idx = (start + i) % HISTORY_LEN;
    JsonObject h = hist.createNestedObject();
    h["latency_ms"] = history[idx].latency_ms;
    h["confidence"] = history[idx].confidence;
    h["model"]      = history[idx].model;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// Serves the dashboard HTML page (single file, no external assets)
void handleRoot() {
  server.send(200, "text/html", DASHBOARD_HTML);
}

// Diagnostic endpoint: raw UART reception stats, independent of whether
// any line ever parsed successfully. Check this first when the dashboard
// shows "unknown" -- if bytesReceived is 0, it's a wiring/pins/baud
// problem upstream of any code here. If bytesReceived > 0 but
// linesReceived is 0, the RA8D1 is sending but never terminating with
// '\n' (or it's arriving at the wrong baud and turning into garbage that
// happens to contain no 0x0A byte). If linesReceived > 0 but
// parseFailures == linesReceived, bytes and framing are fine but the
// JSON content itself is malformed -- check lastRawLine below.
void handleDebug() {
  StaticJsonDocument<768> doc;
  doc["bytesReceived"]   = totalBytesReceived;
  doc["linesReceived"]   = totalLinesReceived;
  doc["parseFailures"]   = totalParseFailures;
  doc["lastRawLine"]     = lastRawLine;
  doc["lastParseError"]  = lastParseError;
  doc["wifiStatus"]      = (WiFi.status() == WL_CONNECTED) ? "connected" : "not connected";
  doc["wifiIP"]          = WiFi.localIP().toString();
  doc["uptimeMs"]        = millis();

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------
// SETUP / LOOP
// ---------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(TOGGLE_BUTTON_PIN, INPUT_PULLUP);

  RA8D1Serial.begin(RA8D1_BAUD, SERIAL_8N1, RA8D1_RX_PIN, RA8D1_TX_PIN);

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

