/*
--------------------------------------------------------------------------------
-- Smart Anti-Dengue Plant Care System — ESP32 Telegram gateway (MVP)
-- Structure adapted from the AS4 sample (c) Low Tjun Lym, NUS — educational use only.
--------------------------------------------------------------------------------
-- Role of this board
--   - Reads the DHT11 humidity sensor (polled) and sends it to the MCXC444 over UART
--   - Receives status / events from the MCXC444 over UART
--   - Over Wi-Fi + HTTPS, uses the Telegram Bot API (REST) to:
--       send a daily time-left report
--       send instant alerts when the plate is full, expired or emptied
--       receive commands: /status /report /open /close /auto /mute /reset
--
-- Libraries (Library Manager): Arduino_JSON (by Arduino), DHT sensor library (by Adafruit),
--                              Adafruit Unified Sensor
-- Secrets  : fill in secrets.h (see the notes in that file; never commit real values)
-- Wiring : MCXC444 TX -> GPIO16 (RX2), MCXC444 RX <- GPIO17 (TX2), common GND
--          DHT11 module data -> GPIO4 (module has its own pull-up), VCC 3V3
--
-- UART protocol (9600 8N1, one message per line):
--   MCXC444 -> ESP32 : $STAT,<time_left_s>,<state>,<level>,<light>,<humidity>,<soil_dry>,<watering>
--                      (only the first 5 fields are parsed)
--                      $EVT,FULL | $EVT,CLEARED | $EVT,EXPIRED
--   ESP32 -> MCXC444 : $HUM,<%RH>
--                      $CMD,LID_OPEN | LID_CLOSE | LID_AUTO | MUTE | RESET
--                      (LID_* names kept from an earlier design; they now control the sprinkler)
--
-- Tasks / RTOS objects
--   tUartBridge : UART receive + DHT11 polling
--   tTelegram   : daily report, event alerts, command polling
--   statusMutex : guards the status variables
--   uartMutex   : one writer on Serial1 at a time
--   evtQueue    : MCXC444 events -> tTelegram
--------------------------------------------------------------------------------
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <DHT.h>

#include "secrets.h"         // WIFI_SSID, WIFI_PASSWORD, BOT_TOKEN, CHAT_ID

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Telegram bot: create with @BotFather, get your chat id from @userinfobot
String botToken = BOT_TOKEN;
String chatId   = CHAT_ID;

// Pins
#define DHT_PIN     4
#define DHT_TYPE    DHT11   // blue 3-pin module; use DHT22 for the white one
#define MCX_RX_PIN  16
#define MCX_TX_PIN  17
#define MCX_BAUD    9600

// Timers (short values for testing / demo)
unsigned long humidityDelay = 30000;   // send humidity to MCXC444 every 30 s
unsigned long reportDelay   = 300000;  // "daily" report every 5 min for demo (86400000 = 24 h)
unsigned long telegramDelay = 3000;    // check for new commands every 3 s

// Latest status from the MCXC444 (protected by statusMutex)
long timeLeft = -1;     // simulated seconds left, -1 = no data yet
int  binState = 0;      // 0 = OK, 1 = full, 2 = expired
int  waterLevel = 0;
int  lightLevel = 0;
int  humidity = -1;     // from DHT11

// Events from the MCXC444
#define EVT_FULL     1
#define EVT_CLEARED  2
#define EVT_EXPIRED  3

SemaphoreHandle_t statusMutex;
SemaphoreHandle_t uartMutex;
QueueHandle_t     evtQueue;

DHT dht(DHT_PIN, DHT_TYPE);

/* ------------------------------------------------------------------ */
/* HTTP helpers (same pattern as the sample, but HTTPS for Telegram)    */
/* ------------------------------------------------------------------ */
String httpGETRequest(const char* serverName) {
  WiFiClientSecure client;
  client.setInsecure();               // MVP: skip certificate check
  HTTPClient http;

  http.begin(client, serverName);
  int httpResponseCode = http.GET();

  String payload = "{}";
  if (httpResponseCode > 0) {
    payload = http.getString();
  } else {
    Serial.print("GET error code: ");
    Serial.println(httpResponseCode);
  }
  http.end();
  return payload;
}

String httpPOSTRequest(const char* serverName, String body) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  http.begin(client, serverName);
  http.addHeader("Content-Type", "application/json");
  int httpResponseCode = http.POST(body);

  String payload = "{}";
  if (httpResponseCode > 0) {
    payload = http.getString();
  } else {
    Serial.print("POST error code: ");
    Serial.println(httpResponseCode);
  }
  http.end();
  return payload;
}

/* ------------------------------------------------------------------ */
/* Telegram helpers                                                     */
/* ------------------------------------------------------------------ */
void sendTelegram(String text) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected");
    return;
  }
  JSONVar msg;
  msg["chat_id"] = chatId;
  msg["text"] = text;

  String serverPath = "https://api.telegram.org/bot" + botToken + "/sendMessage";
  httpPOSTRequest(serverPath.c_str(), JSON.stringify(msg));
  Serial.println("[tg] sent: " + text);
}

void sendToMcx(String line) {
  xSemaphoreTake(uartMutex, portMAX_DELAY);
  Serial1.print(line);
  Serial1.print('\n');
  xSemaphoreGive(uartMutex);
  Serial.println("[tx] " + line);
}

String formatTimeLeft(long s) {
  if (s < 0) return "unknown";
  long days = s / 86400;
  long hours = (s % 86400) / 3600;
  return String(days) + " d " + String(hours) + " h";
}

String stateName(int st) {
  if (st == 1) return "FULL";
  if (st == 2) return "EXPIRED";
  return "OK";
}

void sendStatus() {
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  String text = "Time left: " + formatTimeLeft(timeLeft) +
                "\nPlate: " + stateName(binState) +
                "\nHumidity: " + (humidity >= 0 ? String(humidity) + "%" : String("n/a"));
  xSemaphoreGive(statusMutex);
  sendTelegram(text);
}

void sendDailyReport() {
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  long t = timeLeft;
  int st = binState;
  xSemaphoreGive(statusMutex);
  if (t < 0) return;                  // no data from MCXC444 yet

  String text = "Daily report: " + formatTimeLeft(t) + " left before mosquitoes can breed.";
  if (st == 1) text += "\nPlate is FULL - please empty it.";
  if (st == 2) text += "\nTime is up - empty the plate now.";
  sendTelegram(text);
}

void handleCommand(String cmd) {
  if (cmd == "/status")      sendStatus();
  else if (cmd == "/report") sendDailyReport();
  else if (cmd == "/open")  { sendToMcx("$CMD,LID_OPEN");  sendTelegram("Sprinkler on (stops by itself after 10 s)."); }
  else if (cmd == "/close") { sendToMcx("$CMD,LID_CLOSE"); sendTelegram("Sprinkler held off. Send /auto to resume automatic watering."); }
  else if (cmd == "/auto")  { sendToMcx("$CMD,LID_AUTO");  sendTelegram("Sprinkler back on automatic watering."); }
  else if (cmd == "/mute")  { sendToMcx("$CMD,MUTE");      sendTelegram("Buzzer muted until the plate is emptied."); }
  else if (cmd == "/reset") { sendToMcx("$CMD,RESET");     sendTelegram("Timer reset to 6 days."); }
  else sendTelegram("Commands: /status /report /open /close /auto /mute /reset");
}

// Ask Telegram for new messages and run any commands from our chat
void checkTelegramCommands(long &offset) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected");
    return;
  }
  String serverPath = "https://api.telegram.org/bot" + botToken +
                      "/getUpdates?timeout=0&offset=" + String(offset);
  String jsonBuffer = httpGETRequest(serverPath.c_str());
  JSONVar myObject = JSON.parse(jsonBuffer);

  if (JSON.typeof(myObject) == "undefined") {
    Serial.println("Parsing input failed!");
    return;
  }

  JSONVar results = myObject["result"];
  for (int i = 0; i < results.length(); i++) {
    offset = (long)(double)results[i]["update_id"] + 1;   // mark as read

    if (JSON.typeof(results[i]["message"]["text"]) != "string") continue;
    String fromChat = String((long long)(double)results[i]["message"]["chat"]["id"]);
    if (fromChat != chatId) {                            // only the owner can control it
      Serial.println("[tg] ignored message from chat " + fromChat + " (CHAT_ID mismatch)");
      continue;
    }

    String cmd = (const char*)results[i]["message"]["text"];
    cmd.trim();
    int at = cmd.indexOf('@');                           // "/status@botname" -> "/status"
    if (at > 0) cmd = cmd.substring(0, at);
    Serial.println("[tg] command: " + cmd);
    handleCommand(cmd);
  }
}

/* ------------------------------------------------------------------ */
/* Task: tUartBridge — UART receive + DHT11 polling                     */
/* ------------------------------------------------------------------ */
void handleMcxLine(String line) {
  Serial.println("[rx] " + line);

  if (line.startsWith("$STAT,")) {
    long t; int st, lvl, lux, hum;
    if (sscanf(line.c_str(), "$STAT,%ld,%d,%d,%d,%d", &t, &st, &lvl, &lux, &hum) >= 4) {
      xSemaphoreTake(statusMutex, portMAX_DELAY);
      timeLeft = t;
      binState = st;
      waterLevel = lvl;
      lightLevel = lux;
      xSemaphoreGive(statusMutex);
    }
  } else if (line == "$EVT,FULL") {
    int ev = EVT_FULL;    xQueueSend(evtQueue, &ev, 0);
  } else if (line == "$EVT,CLEARED") {
    int ev = EVT_CLEARED; xQueueSend(evtQueue, &ev, 0);
  } else if (line == "$EVT,EXPIRED") {
    int ev = EVT_EXPIRED; xQueueSend(evtQueue, &ev, 0);
  }
}

void tUartBridge(void *pvParameters) {
  String buffer = "";
  unsigned long lastHumidity = 0;
  bool firstRead = true;

  for (;;) {
    // 1. Read UART lines from the MCXC444
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (c == '\r') continue;
      if (c == '\n') {
        if (buffer.length() > 0) handleMcxLine(buffer);
        buffer = "";
      } else if (buffer.length() < 80) {
        buffer += c;
      } else {
        buffer = "";                  // line too long, discard
      }
    }

    // 2. Poll the DHT11 and forward humidity to the MCXC444
    if (firstRead || (millis() - lastHumidity) > humidityDelay) {
      firstRead = false;
      lastHumidity = millis();
      float h = dht.readHumidity();
      if (isnan(h) || h < 0 || h > 100) {
        Serial.println("Humidity read failed or out of range (check DHT_TYPE / wiring)");
      } else {
        int hi = (int)(h + 0.5);
        xSemaphoreTake(statusMutex, portMAX_DELAY);
        humidity = hi;
        xSemaphoreGive(statusMutex);
        Serial.print("Humidity: ");
        Serial.println(hi);
        sendToMcx("$HUM," + String(hi));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

/* ------------------------------------------------------------------ */
/* Task: tTelegram — alerts, daily report, commands                     */
/* ------------------------------------------------------------------ */
void tTelegram(void *pvParameters) {
  long offset = 0;
  unsigned long lastReport = 0;
  bool firstReportSent = false;

  // Sent from here, not setup(): HTTPS needs this task's larger stack
  sendTelegram("Plant care system online. Send /status any time.");

  for (;;) {
    // 1. Instant alerts from MCXC444 events
    int ev;
    while (xQueueReceive(evtQueue, &ev, 0) == pdTRUE) {
      if (ev == EVT_FULL)    sendTelegram("ALERT: Plate is FULL - please empty it.");
      if (ev == EVT_EXPIRED) sendTelegram("ALERT: 6-day window reached. Empty the plate now to stop mosquito breeding.");
      if (ev == EVT_CLEARED) sendTelegram("Plate emptied. Countdown reset to 6 days.");
    }

    // 2. Daily time-left report (first one as soon as the MCXC444 reports in)
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    bool haveData = (timeLeft >= 0);
    xSemaphoreGive(statusMutex);
    if ((!firstReportSent && haveData) || (firstReportSent && (millis() - lastReport) > reportDelay)) {
      sendDailyReport();
      lastReport = millis();
      firstReportSent = true;
    }

    // 3. Commands from the user
    checkTelegramCommands(offset);

    // Debug: smallest free stack seen so far (bytes); keep this well above 0
    static UBaseType_t minFree = UINT32_MAX;
    UBaseType_t freeNow = uxTaskGetStackHighWaterMark(NULL);
    if (freeNow < minFree) {
      minFree = freeNow;
      Serial.printf("[tg] stack free (min): %u bytes\n", (unsigned)minFree);
    }

    vTaskDelay(pdMS_TO_TICKS(telegramDelay));
  }
}

/* ------------------------------------------------------------------ */
void setup() {
  Serial.begin(115200);
  chatId.replace(" ", "");   // tolerate stray spaces pasted into CHAT_ID
  botToken.trim();
  Serial1.begin(MCX_BAUD, SERIAL_8N1, MCX_RX_PIN, MCX_TX_PIN);
  dht.begin();

  Serial.print("Connecting to SSID:");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(true);
  Serial.println("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.print("\nConnected to WiFi network with IP Address: ");
  Serial.println(WiFi.localIP());

  statusMutex = xSemaphoreCreateMutex();
  uartMutex   = xSemaphoreCreateMutex();
  evtQueue    = xQueueCreate(8, sizeof(int));

  // Dual-core ESP32: UART on core 1, HTTPS + Wi-Fi on core 0.
  // Single-core chips (ESP32-S2/C3): both on core 0; tUartBridge's higher
  // priority still lets it preempt tTelegram whenever UART data arrives.
  const BaseType_t uartCore = (portNUM_PROCESSORS > 1) ? 1 : 0;
  xTaskCreatePinnedToCore(tUartBridge, "tUartBridge", 4096,  NULL, 3, NULL, uartCore);
  // HTTPS + JSON parsing: measured ~3.6 KB peak use; 16 KB leaves ample headroom.
  xTaskCreatePinnedToCore(tTelegram,   "tTelegram",   16384, NULL, 2, NULL, 0);
}

void loop() {
  vTaskDelete(NULL);   // all work is done in the two tasks
}
