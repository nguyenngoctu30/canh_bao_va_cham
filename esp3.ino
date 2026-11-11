// ===== ESP3 - BO DIEU KHIEN: nhan LoRa, bien bao, MQTT, phat lenh hien thi =====
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

// WiFi
const char* WIFI_SSID = "677 5G";
const char* WIFI_PASS = "10101010";

// MQTT (broker mien phi, cong 1883)
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;
const char* TOPIC_STATE = "iot677/khuccua01/state";
const char* TOPIC_EVENT = "iot677/khuccua01/event";

// LoRa E32
const int LORA_AUX = 27;
const int LORA_RX  = 26;   // ESP32 RX <- E32 TX
const int LORA_TX  = 25;   // ESP32 TX -> E32 RX
const int LORA_M1  = 33;
const int LORA_M0  = 32;

// Bien canh bao (kich muc HIGH, qua relay/transistor)
const int SIGN_A_PIN = 18;  // bien dat o huong A: sang khi co xe tu B
const int SIGN_B_PIN = 19;  // bien dat o huong B: sang khi co xe tu A

// Thoi gian
const unsigned long NODE_TIMEOUT_MS  = 6000;
const unsigned long CLEAR_CONFIRM_MS = 3000;
const unsigned long PUBLISH_MS       = 2000;
const unsigned long W_ACTIVE_MS      = 2000;   // chu ky phat lenh hien thi khi co canh bao
const unsigned long W_IDLE_MS        = 5000;   // chu ky phat lenh hien thi khi roi

enum State { IDLE, A_ONLY, B_ONLY, BOTH };
const char* STATE_NAME[] = {"IDLE", "A_ONLY", "B_ONLY", "BOTH"};

struct Node {
  bool present = false;
  int speed = 0, dist = 0;
  unsigned long total = 0;
  unsigned long lastRx = 0;
  int lastSeq = -1;
  bool online() const { return lastRx != 0 && millis() - lastRx < NODE_TIMEOUT_MS; }
  bool active() const { return online() && present; }
};

Node nodeA, nodeB;
State applied = IDLE;
unsigned long lastActiveMs = 0;
bool dirty = true;
unsigned long lastPub = 0, lastMqttTry = 0, lastWifiTry = 0;

// Toc do giu lai de hien tren man hinh (khong ve 0 ngay khi xe vua roi di)
int dispSpdA = 0, dispSpdB = 0;
unsigned long lastW = 0, lastWBurst = 0;
int wBurst = 0;

WiFiClient wifi;
PubSubClient mqtt(wifi);

char lineBuf[96];
int lineIdx = 0;

uint8_t crcOf(const char* s, int len) {
  uint8_t c = 0;
  for (int i = 0; i < len; i++) c ^= (uint8_t)s[i];
  return c;
}

void waitAux() {
  unsigned long t = millis();
  while (digitalRead(LORA_AUX) == LOW && millis() - t < 500) delay(1);
  delay(2);
}

void sendAck(char node, unsigned int seq) {
  char p[24];
  snprintf(p, sizeof(p), "K,%c,%u", node, seq);
  char out[40];
  int n = snprintf(out, sizeof(out), "$%s*%02X\n", p, crcOf(p, strlen(p)));
  waitAux();
  Serial1.write((const uint8_t*)out, n);
}

// Lenh hien thi gui cho ESP1/ESP2: W,signA,signB,tocDoA,tocDoB
void sendDisplay() {
  bool sa = (applied == B_ONLY || applied == BOTH);
  bool sb = (applied == A_ONLY || applied == BOTH);
  char p[40];
  snprintf(p, sizeof(p), "W,%d,%d,%d,%d", sa ? 1 : 0, sb ? 1 : 0, dispSpdA, dispSpdB);
  char out[56];
  int n = snprintf(out, sizeof(out), "$%s*%02X\n", p, crcOf(p, strlen(p)));
  waitAux();
  Serial1.write((const uint8_t*)out, n);
  lastW = millis();
  Serial.print("TX: ");
  Serial.print(out);
}

void displayTick() {
  unsigned long now = millis();
  // Vua doi trang thai: gui nhanh vai lan de chac chan toi noi
  if (wBurst > 0 && now - lastWBurst >= 400) {
    wBurst--;
    lastWBurst = now;
    sendDisplay();
    return;
  }
  unsigned long interval = (applied == IDLE) ? W_IDLE_MS : W_ACTIVE_MS;
  if (now - lastW >= interval) sendDisplay();
}

void publishState() {
  if (!mqtt.connected()) return;
  char p[400];
  bool sa = (applied == B_ONLY || applied == BOTH);
  bool sb = (applied == A_ONLY || applied == BOTH);
  snprintf(p, sizeof(p),
    "{\"state\":\"%s\",\"signA\":%d,\"signB\":%d,"
    "\"a\":{\"on\":%d,\"p\":%d,\"spd\":%d,\"dist\":%d,\"cnt\":%lu},"
    "\"b\":{\"on\":%d,\"p\":%d,\"spd\":%d,\"dist\":%d,\"cnt\":%lu},"
    "\"up\":%lu}",
    STATE_NAME[applied], sa, sb,
    nodeA.online(), nodeA.active(), nodeA.speed, nodeA.dist, nodeA.total,
    nodeB.online(), nodeB.active(), nodeB.speed, nodeB.dist, nodeB.total,
    millis() / 1000);
  mqtt.publish(TOPIC_STATE, p, true);
  lastPub = millis();
}

void publishEvent(char n, int spd, unsigned long cnt) {
  if (!mqtt.connected()) return;
  char p[100];
  snprintf(p, sizeof(p), "{\"n\":\"%c\",\"spd\":%d,\"cnt\":%lu}", n, spd, cnt);
  mqtt.publish(TOPIC_EVENT, p);
}

void applyOutputs() {
  bool signA = (applied == B_ONLY || applied == BOTH);
  bool signB = (applied == A_ONLY || applied == BOTH);
  digitalWrite(SIGN_A_PIN, signA ? HIGH : LOW);
  digitalWrite(SIGN_B_PIN, signB ? HIGH : LOW);
}

void updateState() {
  bool a = nodeA.active(), b = nodeB.active();
  if (a && nodeA.speed > 0) dispSpdA = nodeA.speed;
  if (b && nodeB.speed > 0) dispSpdB = nodeB.speed;

  State raw = (a && b) ? BOTH : (a ? A_ONLY : (b ? B_ONLY : IDLE));
  unsigned long now = millis();
  State next = applied;

  if (raw != IDLE) {
    lastActiveMs = now;
    next = raw;
  } else if (applied != IDLE && now - lastActiveMs >= CLEAR_CONFIRM_MS) {
    next = IDLE;
  }

  if (next != applied) {
    applied = next;
    applyOutputs();
    if (applied == IDLE) { dispSpdA = 0; dispSpdB = 0; }
    dirty = true;
    wBurst = 3;          // gui lenh hien thi ngay, lap lai 3 lan
    lastWBurst = 0;
    Serial.printf("STATE -> %s\n", STATE_NAME[applied]);
  }
}

void handlePayload(char* p) {
  Serial.print("RX: ");
  Serial.println(p);
  char node;
  unsigned int seq;
  int pr, spd, dist, peak;
  unsigned long total;

  if (p[0] == 'S') {
    if (sscanf(p, "S,%c,%d,%d,%d,%lu", &node, &pr, &spd, &dist, &total) != 5) return;
    Node* n = (node == 'A') ? &nodeA : (node == 'B') ? &nodeB : nullptr;
    if (!n) return;
    if (n->present != (pr == 1)) dirty = true;
    n->present = (pr == 1);
    n->speed = spd;
    n->dist = dist;
    n->total = total;
    n->lastRx = millis();

  } else if (p[0] == 'P') {
    if (sscanf(p, "P,%c,%u,%d,%d,%d,%lu", &node, &seq, &pr, &spd, &dist, &total) != 6) return;
    Node* n = (node == 'A') ? &nodeA : (node == 'B') ? &nodeB : nullptr;
    if (!n) return;
    sendAck(node, seq);
    n->lastRx = millis();
    if (n->lastSeq == (int)seq) return;
    n->lastSeq = seq;
    n->present = (pr == 1);
    n->speed = spd;
    n->dist = dist;
    n->total = total;
    dirty = true;

  } else if (p[0] == 'V') {
    if (sscanf(p, "V,%c,%u,%d,%lu", &node, &seq, &peak, &total) != 4) return;
    Node* n = (node == 'A') ? &nodeA : (node == 'B') ? &nodeB : nullptr;
    if (!n) return;
    sendAck(node, seq);
    n->lastRx = millis();
    if (n->lastSeq == (int)seq) return;
    n->lastSeq = seq;
    n->total = total;
    publishEvent(node, peak, total);
  }
}

void handleLine(char* line) {
  if (line[0] != '$') return;
  char* star = strrchr(line, '*');
  if (!star || strlen(star) < 3) return;
  int len = star - (line + 1);
  unsigned int cs = 0;
  if (sscanf(star + 1, "%2x", &cs) != 1) return;
  if (crcOf(line + 1, len) != (uint8_t)cs) {
    Serial.println("RX loi checksum, bo");
    return;
  }
  *star = 0;
  handlePayload(line + 1);
}

void readLora() {
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineIdx] = 0;
      if (lineIdx > 0) handleLine(lineBuf);
      lineIdx = 0;
    } else if (lineIdx < (int)sizeof(lineBuf) - 1) {
      lineBuf[lineIdx++] = c;
    } else {
      lineIdx = 0;
    }
  }
}

void keepConnections() {
  unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiTry > 10000) {
      lastWifiTry = now;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }
  if (!mqtt.connected()) {
    if (now - lastMqttTry > 5000) {
      lastMqttTry = now;
      String cid = "esp3-curve-" + String((uint32_t)ESP.getEfuseMac(), HEX);
      if (mqtt.connect(cid.c_str())) {
        Serial.println("MQTT connected");
        dirty = true;
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(SIGN_A_PIN, OUTPUT);
  pinMode(SIGN_B_PIN, OUTPUT);
  digitalWrite(SIGN_A_PIN, LOW);
  digitalWrite(SIGN_B_PIN, LOW);

  pinMode(LORA_M0, OUTPUT);
  pinMode(LORA_M1, OUTPUT);
  pinMode(LORA_AUX, INPUT);
  digitalWrite(LORA_M0, LOW);
  digitalWrite(LORA_M1, LOW);
  delay(200);

  Serial1.setRxBufferSize(1024);
  Serial1.begin(9600, SERIAL_8N1, LORA_RX, LORA_TX);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(2);

  Serial.println("ESP3 - Bo dieu khien san sang");
}

void loop() {
  readLora();
  updateState();
  displayTick();
  keepConnections();
  if (mqtt.connected()) mqtt.loop();

  if (dirty || millis() - lastPub >= PUBLISH_MS) {
    publishState();
    dirty = false;
  }
}
