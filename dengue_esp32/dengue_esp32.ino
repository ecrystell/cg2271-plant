/* Smart plant care ESP32 gateway, based on the AS4 HTTP/JSON pattern.
 * Helpers adapted from the AS4 sample (c) Low Tjun Lym, NUS; educational use.
 * Arduino libraries: Arduino_JSON; Adafruit DHT sensor + Unified Sensor.
 * UART framing/rules match lab4/source/plant_rules.h (keep the copies identical).
 * Alerts remain pending until Telegram confirms success; UART events are ACKed
 * afterwards. This is retry-based delivery, not exactly-once storage across power loss.
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <DHT.h>
#include <time.h>
#include "plant_rules.h"
#include "telegram_ca.h"
#include "gateway_types.h"

const char* ssid = "";
const char* password = "";
String botToken = "";
String chatId = "";
#define DHT_PIN 4
#define MCX_RX_PIN 16
#define MCX_TX_PIN 17
#define MCX_BAUD 9600
#define STATUS_TTL_MS 15000UL
#define COMMAND_TTL_MS 10000UL
#define COMMAND_RETRY_MS 2000UL
unsigned long humidityDelay = 30000UL;
unsigned long reportDelay = 86400000UL; /* Daily interval; use 300000 only for a demo. */
unsigned long telegramDelay = 3000UL;

PlantStatus latest = {};
PendingCommand pending = {};
SemaphoreHandle_t statusMutex, uartMutex;
QueueHandle_t evtQueue, cmdQueue, noticeQueue;
uint32_t controllerGeneration = 1, queuedEvent = 0, deliveredEvent = 0, nextCommandId;
DHT dht(DHT_PIN, DHT22);

PlantStatus statusCopy() {
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  PlantStatus copy = latest;
  xSemaphoreGive(statusMutex);
  return copy;
}
bool statusFresh(const PlantStatus &s) {
  return plant_fresh(s.valid, millis(), s.updated, STATUS_TTL_MS);
}
void queueNotice(const String &text) {
  PlantNotice notice = {};
  text.toCharArray(notice.text, sizeof(notice.text));
  if (xQueueSend(noticeQueue, &notice, 0) != pdTRUE) Serial.println("Reply queue full");
}
void sendToMcx(const String &line) {
  xSemaphoreTake(uartMutex, portMAX_DELAY);
  Serial1.print(line);
  Serial1.print('\n');
  xSemaphoreGive(uartMutex);
}
String httpRequest(const String &url, const String *body) {
  if (WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000) return "{}";
  WiFiClientSecure client;
  client.setCACert(TELEGRAM_ROOT_CA);
  client.setHandshakeTimeout(8);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(7000);
  if (!http.begin(client, url)) return "{}";
  int code;
  if (body) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(*body);
  } else code = http.GET();
  String response = "{}";
  if (code == 200 && http.getSize() <= 32768) {
    String received = http.getString();
    if (received.length() <= 32768) response = received;
  }
  http.end();
  return response;
}
bool sendTelegram(const String &text) {
  if (botToken.length() == 0 || chatId.length() == 0) return false;
  JSONVar message;
  message["chat_id"] = chatId;
  message["text"] = text;
  String body = JSON.stringify(message);
  JSONVar response = JSON.parse(httpRequest("https://api.telegram.org/bot" + botToken + "/sendMessage", &body));
  return JSON.typeof(response["ok"]) == "boolean" && (bool)response["ok"];
}
String formatTimeLeft(uint32_t seconds) {
  return String(seconds / 86400UL) + " d " + String((seconds % 86400UL) / 3600UL) + " h";
}
String statusText() {
  PlantStatus s = statusCopy();
  if (!statusFresh(s)) return "Controller status unavailable or stale; physical valve state is unknown.";
  String text = "Maintenance timer: " + formatTimeLeft(s.timeLeft);
  text += "\nPlate: " + String(s.state == 1 ? "FULL" : s.state == 2 ? "REMINDER DUE" : "normal");
  text += "\nWater level ADC: " + String(s.level);
  text += "\nLight ADC: " + String(s.light);
  text += "\nHumidity: " + (s.humidity <= 100 ? String(s.humidity) + "%" : String("unknown/stale"));
  text += "\nSoil: " + String(s.soilDry ? "dry" : "wet");
  text += "\nWatering: " + String(s.watering ? "ON" : "OFF");
  text += "\nMode: " + String(s.mode == 0 ? "automatic" : s.mode == 1 ? "manual burst" : "manual off");
  if (!s.sensorsOk) text += "\nSensor fault: watering blocked.";
  return text;
}
void requestCommand(const char *verb) {
  if (!statusFresh(statusCopy())) { queueNotice("Controller offline; command not queued."); return; }
  PlantCommand command = {};
  strncpy(command.verb, verb, sizeof(command.verb) - 1);
  command.queued = millis();
  if (xQueueSend(cmdQueue, &command, 0) != pdTRUE) queueNotice("Command queue busy; try again.");
  else queueNotice("Command queued; waiting for controller acknowledgment.");
}
void handleCommand(String command) {
  command.trim();
  int space = command.indexOf(' ');
  if (space >= 0) command = command.substring(0, space);
  int at = command.indexOf('@');
  if (at > 0) command = command.substring(0, at);
  if (command == "/status") queueNotice(statusText());
  else if (command == "/report") queueNotice("Plant care report\n" + statusText());
  else if (command == "/open" || command == "/water") requestCommand("WATER_ON");
  else if (command == "/close" || command == "/stop") requestCommand("WATER_OFF");
  else if (command == "/auto") requestCommand("AUTO");
  else if (command == "/mute") requestCommand("MUTE");
  else if (command == "/unmute") requestCommand("UNMUTE");
  else if (command == "/reset") requestCommand("RESET");
  else queueNotice("/status /report /water /stop /auto /mute /unmute /reset\n/open and /close are aliases. Reset requires an empty plate.");
}
void checkTelegramCommands(long &offset, bool &initialSync) {
  if (botToken.length() == 0 || chatId.length() == 0) return;
  /* Discard historical commands at gateway startup; never replay an old /water. */
  String query = initialSync ? "?timeout=0&limit=1&offset=-1" : "?timeout=0&limit=5&offset=" + String(offset);
  JSONVar response = JSON.parse(httpRequest("https://api.telegram.org/bot" + botToken + "/getUpdates" + query, nullptr));
  if (JSON.typeof(response["ok"]) != "boolean" || !(bool)response["ok"] ||
      JSON.typeof(response["result"]) != "array") return;
  JSONVar updates = response["result"];
  for (int i = 0; i < updates.length(); i++) {
    if (JSON.typeof(updates[i]["update_id"]) != "number") continue;
    long id = (long)(double)updates[i]["update_id"];
    if (!initialSync && id < offset) continue;
    offset = id + 1;
    if (initialSync || JSON.typeof(updates[i]["message"]["text"]) != "string" ||
        JSON.typeof(updates[i]["message"]["chat"]["id"]) != "number" ||
        JSON.typeof(updates[i]["message"]["date"]) != "number") continue;
    String fromChat = String((long long)(double)updates[i]["message"]["chat"]["id"]);
    time_t sent = (time_t)(double)updates[i]["message"]["date"];
    time_t now = time(nullptr);
    if (fromChat == chatId && sent <= now && now - sent < 120)
      handleCommand((const char *)updates[i]["message"]["text"]);
  }
  initialSync = false;
}
void handleMcxLine(char *line) {
  char *field[10];
  unsigned count = plant_fields(line, field, 10);
  if (count == 1 && !strcmp(field[0], "$BOOT")) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    controllerGeneration++;
    latest.valid = false;
    queuedEvent = deliveredEvent = 0;
    xSemaphoreGive(statusMutex);
    pending.active = false;
    xQueueReset(cmdQueue);
    queueNotice("Controller restarted. Pending commands cancelled; awaiting fresh status.");
    return;
  }
  if (count == 10 && !strcmp(field[0], "$STAT")) {
    uint32_t value[9];
    for (unsigned i = 0; i < 9; i++) if (!plant_uint(field[i + 1], &value[i])) return;
    if (value[0] > 6UL * 86400UL || value[1] > 2 || value[2] > 4095 || value[3] > 4095 ||
        (value[4] > 100 && value[4] != 255) || value[5] > 1 || value[6] > 1 || value[7] > 2 || value[8] > 1) return;
    PlantStatus s = {};
    s.timeLeft = value[0]; s.state = value[1]; s.level = value[2]; s.light = value[3];
    s.humidity = value[4]; s.soilDry = value[5]; s.watering = value[6];
    s.mode = value[7]; s.sensorsOk = value[8]; s.updated = millis(); s.valid = true;
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    latest = s;
    xSemaphoreGive(statusMutex);
    return;
  }
  uint32_t id;
  if (count != 3 || !plant_uint(field[1], &id) || id == 0) return;
  if (!strcmp(field[0], "$ACK")) {
    if (pending.active && pending.id == id) {
      queueNotice(String("Controller ") + pending.verb + ": " + field[2] +
                  (!strcmp(field[2], "OK") ? " (accepted)." : " (not applied)."));
      pending.active = false;
    }
    return;
  }
  if (strcmp(field[0], "$EVT")) return;
  uint8_t type;
  if (!strcmp(field[2], "FULL")) type = 1;
  else if (!strcmp(field[2], "CLEARED")) type = 2;
  else if (!strcmp(field[2], "EXPIRED")) type = 3;
  else if (!strcmp(field[2], "RESET")) type = 4;
  else return;
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  uint32_t delivered = deliveredEvent, queued = queuedEvent, generation = controllerGeneration;
  xSemaphoreGive(statusMutex);
  if (id == delivered) { sendToMcx("$EACK," + String(id)); return; }
  if (id == queued) return;
  PlantEvent event = {id, generation, type};
  if (xQueueSend(evtQueue, &event, 0) == pdTRUE) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    queuedEvent = id;
    xSemaphoreGive(statusMutex);
  } /* If full, do not ACK: MCXC444 will retry. */
}
void tUartBridge(void *argument) {
  (void)argument;
  plant_line_t line = {};
  uint32_t lastHumidity = millis() - humidityDelay;
  for (;;) {
    for (unsigned n = 0; n < 128 && Serial1.available(); n++)
      if (plant_line_byte(&line, (char)Serial1.read())) handleMcxLine(line.text);
    uint32_t now = millis();
    if (now - lastHumidity >= humidityDelay) {
      lastHumidity = now;
      float h = dht.readHumidity();
      if (isnan(h) || h < 0 || h > 100) sendToMcx("$HUM,NA");
      else sendToMcx("$HUM," + String((unsigned)(h + 0.5f)));
    }
    if (!pending.active) {
      PlantCommand command;
      if (xQueueReceive(cmdQueue, &command, 0) == pdTRUE) {
        if (now - command.queued > COMMAND_TTL_MS) queueNotice("Command expired before sending; request it again.");
        else {
          nextCommandId++;
          if (!nextCommandId) nextCommandId++;
          pending.id = nextCommandId;
          strncpy(pending.verb, command.verb, sizeof(pending.verb));
          pending.attempts = 0;
          pending.sent = now - COMMAND_RETRY_MS;
          pending.active = true;
        }
      }
    }
    if (pending.active && now - pending.sent >= COMMAND_RETRY_MS) {
      if (pending.attempts >= 3) {
        queueNotice("Command acknowledgment timed out; action is unconfirmed. Check /status.");
        pending.active = false;
      } else {
        sendToMcx("$CMD," + String(pending.id) + "," + pending.verb);
        pending.attempts++;
        pending.sent = now;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
String eventText(uint8_t type) {
  if (type == 1) return "ALERT: The plant plate reached full level. Empty and clean it; watering is blocked while full.";
  if (type == 2) return "Plant plate emptying detected. Maintenance reminder reset to six days.";
  if (type == 3) return "REMINDER: The experimental maintenance timer expired. Empty and clean the plant plate.";
  return "Manual timer reset accepted while the plate was empty.";
}
void tTelegram(void *argument) {
  (void)argument;
  long offset = 0;
  bool initialSync = true, reportSent = false, onlineSent = false;
  uint32_t lastReport = 0, lastAttempt = millis() - 60000UL, lastPoll = millis() - telegramDelay;
  uint32_t lastWifiAttempt = millis();
  for (;;) {
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (ssid[0] && now - lastWifiAttempt >= 30000UL) {
        WiFi.disconnect(); WiFi.begin(ssid, password); lastWifiAttempt = now;
      }
      vTaskDelay(pdMS_TO_TICKS(250));
      continue; /* UART/DHT task keeps running without Wi-Fi. */
    }
    if (time(nullptr) < 1700000000) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }
    if (!onlineSent) onlineSent = sendTelegram("Plant care online. Use /status, /water, /stop, /auto. Watering is locally controlled.");
    PlantEvent event;
    if (xQueuePeek(evtQueue, &event, 0) == pdTRUE) {
      xSemaphoreTake(statusMutex, portMAX_DELAY);
      uint32_t generation = controllerGeneration;
      xSemaphoreGive(statusMutex);
      if (event.generation != generation) xQueueReceive(evtQueue, &event, 0);
      else if (sendTelegram(eventText(event.type))) {
        xQueueReceive(evtQueue, &event, 0);
        xSemaphoreTake(statusMutex, portMAX_DELAY);
        bool sameBoot = event.generation == controllerGeneration;
        if (sameBoot) { deliveredEvent = event.id; queuedEvent = 0; }
        xSemaphoreGive(statusMutex);
        if (sameBoot) sendToMcx("$EACK," + String(event.id));
      }
    }
    PlantNotice notice;
    if (xQueuePeek(noticeQueue, &notice, 0) == pdTRUE && sendTelegram(notice.text))
      xQueueReceive(noticeQueue, &notice, 0);
    now = millis();
    if ((!reportSent || now - lastReport >= reportDelay) &&
        now - lastAttempt >= 60000UL && statusFresh(statusCopy())) {
      lastAttempt = now;
      if (sendTelegram("Daily plant care report\n" + statusText())) { reportSent = true; lastReport = now; }
    }
    now = millis();
    if (now - lastPoll >= telegramDelay) { lastPoll = now; checkTelegramCommands(offset, initialSync); }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}
void setup() {
  Serial.begin(115200);
  Serial1.setRxBufferSize(512);
  Serial1.begin(MCX_BAUD, SERIAL_8N1, MCX_RX_PIN, MCX_TX_PIN);
  dht.begin();
  statusMutex = xSemaphoreCreateMutex();
  uartMutex = xSemaphoreCreateMutex();
  evtQueue = xQueueCreate(8, sizeof(PlantEvent));
  cmdQueue = xQueueCreate(4, sizeof(PlantCommand));
  noticeQueue = xQueueCreate(12, sizeof(PlantNotice));
  configASSERT(statusMutex && uartMutex && evtQueue && cmdQueue && noticeQueue);
  nextCommandId = esp_random();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (ssid[0]) WiFi.begin(ssid, password);
  configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com");
  BaseType_t bridgeCreated = xTaskCreatePinnedToCore(tUartBridge, "tUartBridge", 4096, nullptr, 3, nullptr, 1);
  BaseType_t telegramCreated = xTaskCreatePinnedToCore(tTelegram, "tTelegram", 12288, nullptr, 2, nullptr, 0);
  configASSERT(bridgeCreated == pdPASS && telegramCreated == pdPASS);
}
void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
