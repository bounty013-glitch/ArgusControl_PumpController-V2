#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <PubSubClient.h>

const char* WIFI_SSID = "ArgusMotorTest";
const char* WIFI_PASS = "ArgusPump123";
const char* DEVICE_NAME = "ArgusPumpHMI-001";

const char* MQTT_SERVER = "192.168.4.1";
const int MQTT_PORT = 1883;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

bool mqttConnected = false;

#define MQTT_TOPIC_ROOT "argus/peristaltic"
#define CMD_RUN_TOPIC MQTT_TOPIC_ROOT "/cmd/run"
#define CMD_SPEED_PCT_TOPIC MQTT_TOPIC_ROOT "/cmd/speed_pct"
#define CMD_STOP_TOPIC MQTT_TOPIC_ROOT "/cmd/stop"

#define STATUS_RUN_TOPIC      MQTT_TOPIC_ROOT "/status/run"
#define STATUS_RPM_TOPIC      MQTT_TOPIC_ROOT "/status/rpm"
#define STATUS_LOCKED_TOPIC   MQTT_TOPIC_ROOT "/status/locked"
#define STATUS_ESTOP_TOPIC    MQTT_TOPIC_ROOT "/status/e_stop"
#define STATUS_ONLINE_TOPIC   MQTT_TOPIC_ROOT "/status/online"

#define TOUCH_MISO 39
#define TOUCH_MOSI 32
#define TOUCH_SCLK 25
#define TOUCH_CS   33
#define TOUCH_IRQ  36

#define MAX_SPEED_PCT 100
#define MIN_SPEED_PCT 0

TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
SPIClass touchSPI = SPIClass(VSPI);

String pumpStatus = "READY";
bool pumpRunning = false;
int targetSpeedPct = 50;
bool heartbeatOn = false;
bool wifiBlinkOn = false;
bool mqttBlinkOn = false;
float actualRpm = 0.0;
bool pumpOnline = false;
bool pumpLocked = false;
bool pumpEStop = false;

int mapTouchX(int rawX) {
  int x = map(rawX, 500, 3550, 0, 320);
  return constrain(x, 0, 319);
}

int mapTouchY(int rawY) {
  int y = map(rawY, 750, 3420, 0, 240);
  return constrain(y, 0, 239);
}

void drawHeartbeat(bool on) {
  uint16_t color = on ? TFT_GREEN : TFT_DARKGREEN;

  tft.fillCircle(295, 17, 8, color);
  tft.drawCircle(295, 17, 8, TFT_WHITE);
}

void drawDashboard() {
  tft.fillScreen(TFT_BLACK);

  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("ARGUS PUMP", 55, 8);
  drawHeartbeat(heartbeatOn);

  tft.drawRect(10, 35, 300, 85, TFT_WHITE);

  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("COMMAND %", 20, 45);

  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("ACT RPM", 20, 102);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(String(actualRpm, 1), 205, 102);

  tft.setTextSize(4);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(String(targetSpeedPct), 125, 72);

  tft.fillRoundRect(20, 75, 55, 35, 6, TFT_DARKGREY);
  tft.drawRoundRect(20, 75, 55, 35, 6, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.setTextSize(3);
  tft.drawString("-", 43, 80);

  tft.fillRoundRect(245, 75, 55, 35, 6, TFT_DARKGREY);
  tft.drawRoundRect(245, 75, 55, 35, 6, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString("+", 263, 80);

  tft.drawRect(10, 130, 300, 35, TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("STATUS", 20, 140);

  tft.setTextColor(pumpRunning ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
  tft.drawString(pumpStatus, 175, 140);

  tft.fillRoundRect(10, 178, 140, 50, 8, TFT_DARKGREEN);
  tft.drawRoundRect(10, 178, 140, 50, 8, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("START", 45, 195);

  tft.fillRoundRect(170, 178, 140, 50, 8, TFT_MAROON);
  tft.drawRoundRect(170, 178, 140, 50, 8, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.drawString("STOP", 215, 195);
}

void connectWiFi() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("ARGUS PUMP", 55, 20);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Connecting WiFi...", 30, 80);

  WiFi.setHostname(DEVICE_NAME);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 10) {
    delay(300);
    Serial.print(".");
    tries++;
  }

  tft.fillRect(0, 70, 320, 80, TFT_BLACK);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("WiFi CONNECTED", 35, 80);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(WiFi.localIP().toString(), 55, 115);

    // delay(1500);
  } else {
    Serial.println();
    Serial.println("WiFi failed");

    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("WiFi FAILED", 65, 80);

    // delay(2000);
  }
}

void drawWiFiStatus(bool on) {
  uint16_t color;

  if (WiFi.status() == WL_CONNECTED) {
    color = on ? TFT_BLUE : TFT_NAVY;
  } else {
    color = TFT_RED;
  }

  tft.fillCircle(270, 17, 8, color);
  tft.drawCircle(270, 17, 8, TFT_WHITE);
}

void drawMQTTStatus(bool on) {
  uint16_t color;

  if (mqttClient.connected()) {
    color = on ? TFT_MAGENTA : TFT_PURPLE;
  } else {
    color = TFT_RED;
  }

  tft.fillCircle(245, 17, 8, color);
  tft.drawCircle(245, 17, 8, TFT_WHITE);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[64];

  if (length >= sizeof(msg)) {
    length = sizeof(msg) - 1;
  }

  memcpy(msg, payload, length);
  msg[length] = '\0';

  Serial.print("MQTT IN ");
  Serial.print(topic);
  Serial.print(" = ");
  Serial.println(msg);

  if (strcmp(topic, STATUS_RUN_TOPIC) == 0) {
    pumpRunning = (strcmp(msg, "1") == 0 || strcmp(msg, "true") == 0);
    pumpStatus = pumpRunning ? "RUNNING" : "STOPPED";
    drawStatusField();
    return;
  }

  if (strcmp(topic, STATUS_RPM_TOPIC) == 0) {
    actualRpm = atof(msg);
    drawActualRpm();
    return;
  }

  if (strcmp(topic, STATUS_ONLINE_TOPIC) == 0) {
    pumpOnline = (strcmp(msg, "1") == 0 || strcmp(msg, "true") == 0 || strcmp(msg, "online") == 0);
    drawMQTTStatus(mqttBlinkOn);
    return;
  }

  if (strcmp(topic, STATUS_LOCKED_TOPIC) == 0) {
    pumpLocked = (strcmp(msg, "1") == 0 || strcmp(msg, "true") == 0);
    drawStatusField();
    return;
  }

  if (strcmp(topic, STATUS_ESTOP_TOPIC) == 0) {
    pumpEStop = (strcmp(msg, "1") == 0 || strcmp(msg, "true") == 0);
    pumpStatus = pumpEStop ? "E-STOP" : pumpStatus;
    drawStatusField();
    return;
  }
}

void connectMQTT() {

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);

  String clientId =
      String(DEVICE_NAME) + "-" +
      String((uint32_t)ESP.getEfuseMac(), HEX);

  Serial.print("Connecting MQTT... ");

  if (mqttClient.connect(clientId.c_str())) {

    mqttConnected = true;

    Serial.println("Connected");

    mqttClient.publish(
      "argus/pump_hmi/001/status",
      "online",
      true
    );

    mqttClient.subscribe(STATUS_RUN_TOPIC);
    mqttClient.subscribe(STATUS_RPM_TOPIC);
    mqttClient.subscribe(STATUS_ONLINE_TOPIC);
    mqttClient.subscribe(STATUS_LOCKED_TOPIC);
    mqttClient.subscribe(STATUS_ESTOP_TOPIC);

    Serial.println("Subscribed to pump status topics");

  } else {

    mqttConnected = false;

    Serial.print("Failed. rc=");
    Serial.println(mqttClient.state());
  }

  drawMQTTStatus(mqttBlinkOn);
}

void publishTargetSpeedPct() {
  if (!mqttClient.connected()) {
    Serial.println("MQTT not connected, speed not published");
    return;
  }

  char payload[12];
  snprintf(payload, sizeof(payload), "%d", targetSpeedPct);

  mqttClient.publish(CMD_SPEED_PCT_TOPIC, payload);

  Serial.print("Published speed percent: ");
  Serial.println(payload);
}

void publishRunCommand(bool run) {
  if (!mqttClient.connected()) return;

  mqttClient.publish(CMD_RUN_TOPIC, run ? "1" : "0");

  Serial.print("Published run command: ");
  Serial.println(run ? "1" : "0");
}

void drawCommandPct() {
  tft.fillRect(115, 65, 90, 45, TFT_BLACK);
  tft.setTextSize(4);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(String(targetSpeedPct), 125, 72);
}

void drawActualRpm() {
  tft.fillRect(200, 98, 95, 22, TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(String(actualRpm, 1), 205, 102);
}

void drawStatusField() {
  tft.fillRect(170, 135, 130, 25, TFT_BLACK);

  if (pumpEStop) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("E-STOP", 175, 140);
  } else if (pumpLocked) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawString("LOCKED", 175, 140);
  } else {
    tft.setTextColor(pumpRunning ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
    tft.drawString(pumpStatus, 175, 140);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  tft.init();
  tft.setRotation(1);

  touchSPI.begin(TOUCH_SCLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  drawWiFiStatus(wifiBlinkOn);

  connectWiFi();

  mqttClient.setCallback(mqttCallback);
  connectMQTT();

  Serial.println("Argus Pump HMI ready");

  drawDashboard();
}

void loop() {
  static unsigned long lastTouch = 0;

  if (ts.touched() && millis() - lastTouch > 300) {
    lastTouch = millis();

    TS_Point p = ts.getPoint();
    int x = mapTouchX(p.x);
    int y = mapTouchY(p.y);

    Serial.print("Touch X=");
    Serial.print(x);
    Serial.print(" Y=");
    Serial.println(y);

  if (x >= 20 && x <= 75 && y >= 65 && y <= 125) {
    targetSpeedPct = constrain(targetSpeedPct - 1, MIN_SPEED_PCT, MAX_SPEED_PCT);
    publishTargetSpeedPct();
    drawCommandPct();
}

    if (x >= 245 && x <= 300 && y >= 65 && y <= 125) {
      targetSpeedPct = constrain(targetSpeedPct + 1, MIN_SPEED_PCT, MAX_SPEED_PCT);
      publishTargetSpeedPct();
      drawCommandPct();
    }

    if (x >= 10 && x <= 150 && y >= 150 && y <= 239) {
      pumpRunning = true;
      pumpStatus = "RUNNING";
      publishRunCommand(true);
      drawStatusField();
    }

    if (x >= 170 && x <= 310 && y >= 150 && y <= 239) {
      pumpRunning = false;
      pumpStatus = "STOPPED";
      publishRunCommand(false);
      drawStatusField();
    }
  }

  static unsigned long lastHeartbeat = 0;
  static unsigned long lastWiFiBlink = 0;
  static unsigned long lastMQTTBlink = 0;
  static unsigned long lastWiFiCheck = 0;
  static unsigned long lastMQTTCheck = 0;
  static unsigned long lastMQTTHeartbeat = 0;

  if (millis() - lastHeartbeat > 500) {
    lastHeartbeat = millis();
    heartbeatOn = !heartbeatOn;
    drawHeartbeat(heartbeatOn);
  }

  if (millis() - lastWiFiBlink > 700) {
    lastWiFiBlink = millis();
    wifiBlinkOn = !wifiBlinkOn;
    drawWiFiStatus(wifiBlinkOn);
  }

  if (millis() - lastMQTTBlink > 900) {
    lastMQTTBlink = millis();
    mqttBlinkOn = !mqttBlinkOn;
    drawMQTTStatus(mqttBlinkOn);
  }

  if (millis() - lastWiFiCheck > 5000) {
    lastWiFiCheck = millis();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected. Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  }

  if (millis() - lastMQTTCheck > 5000) {
    lastMQTTCheck = millis();

    if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
      connectMQTT();
    }

    mqttConnected = mqttClient.connected();
  }

if (mqttClient.connected() && millis() - lastMQTTHeartbeat > 1000) {
  lastMQTTHeartbeat = millis();

  mqttClient.publish("argus/pump/001/hmi/heartbeat", "1");
}

  mqttClient.loop();
}