// ===== ESP3 - BO DIEU KHIEN: nhan LoRa, bien bao, MQTT, phat lenh hien thi =====
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

// ---------- Cau hinh (KHONG dua mat khau that len GitHub) ----------
const char* WIFI_SSID = "677 5G";
const char* WIFI_PASS = "10101010";

const char* MQTT_HOST    = "broker.hivemq.com";
const int   MQTT_PORT    = 1883;
const char* TOPIC_STATE  = "iot677/khuccua01/state";    // doi giong web (BASE) va nen dat rieng
const char* TOPIC_EVENT  = "iot677/khuccua01/event";
const char* TOPIC_STATUS = "iot677/khuccua01/status";   // online/offline (Last Will)

#define NET_ID "KC"    // ma mang LoRa: giong het hai tram, nen dat rieng

// ---------- LoRa E32 ----------
const int LORA_AUX = 27;
const int LORA_RX  = 26;   // ESP32 RX <- E32 TX
const int LORA_TX  = 25;   // ESP32 TX -> E32 RX
const int LORA_M1  = 33;
const int LORA_M0  = 32;

// ---------- Bien canh bao (kich muc HIGH, qua relay/transistor) ----------
const int SIGN_A_PIN = 18;  // bien o huong A: sang khi co xe tu B
const int SIGN_B_PIN = 19;  // bien o huong B: sang khi co xe tu A

// ---------- Thoi gian ----------
const unsigned long NODE_TIMEOUT_MS  = 6000;   // mat tin hieu tram -> coi nhu khong co xe
const unsigned long CLEAR_CONFIRM_MS = 3000;
const unsigned long PUBLISH_MS       = 2000;
const unsigned long W_ACTIVE_MS      = 2000;
const unsigned long W_IDLE_MS        = 5000;

enum State { IDLE, A_ONLY, B_ONLY, BOTH };
const char* STATE_NAME[] = {"IDLE", "A_ONLY", "B_ONLY", "BOTH"};

struct Node {
  char id = '?';
  bool present = false, wasOnline = false;
  int speed = 0, dist = 0;
  unsigned long total = 0, lastRx = 0;
  int lastSeq = -1;
  bool online() const { return lastRx != 0 && millis() - lastRx < NODE_TIMEOUT_MS; }
  bool active() const { return online() && present; }
  int  ago() const { return lastRx ? (int)((millis() - lastRx) / 1000) : -1; }
};

Node nodeA, nodeB;
State applied = IDLE;
unsigned long lastActiveMs = 0;
bool dirty = true;
unsigned long lastPub = 0, lastMqttTry = 0, lastWifiTry = 0, lastDiag = 0;
unsigned long foreignCnt = 0, crcErrCnt = 0;   // goi mang khac / goi loi checksum

int dispSpdA = 0, dispSpdB = 0;                // giu toc do de hien tren man hinh
unsigned long lastW = 0, lastWBurst = 0;
int wBurst = 0;

WiFiClient wifi;
PubSubClient mqtt(wifi);
char lineBuf[96];
int lineIdx = 0;

Node* pick(char c) { return c == 'A' ? &nodeA : (c == 'B' ? &nodeB : nullptr); }

// =====================================================
//                       LORA
// =====================================================
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

// Khung: $<NET>:<noi dung>*<CS>\n
void sendFrame(const char* payload) {
  char body[56];
  snprintf(body, sizeof(body), "%s:%s", NET_ID, payload);
  char out[72];
  int n = snprintf(out, sizeof(out), "$%s*%02X\n", body, crcOf(body, strlen(body)));
  waitAux();
  Serial1.write((const uint8_t*)out, n);
}

void sendAck(char node, unsigned int seq) {
  char p[24];
  snprintf(p, sizeof(p), "K,%c,%u", node, seq);
  sendFrame(p);
}

// Lenh hien thi cho hai tram: W,signA,signB,tocDoA,tocDoB
void sendDisplay() {
  bool sa = (applied == B_ONLY || applied == BOTH);
  bool sb = (applied == A_ONLY || applied == BOTH);
  char p[32];
  snprintf(p, sizeof(p), "W,%d,%d,%d,%d", sa ? 1 : 0, sb ? 1 : 0, dispSpdA, dispSpdB);
  sendFrame(p);
  lastW = millis();
  Serial.printf("TX W: %s\n", p);
}

void displayTick() {
  unsigned long now = millis();
  if (wBurst > 0 && now - lastWBurst >= 400) {   // vua doi trang thai: gui nhanh vai lan
    wBurst--;
    lastWBurst = now;
    sendDisplay();
    return;
  }
  unsigned long interval = (applied == IDLE) ? W_IDLE_MS : W_ACTIVE_MS;
  if (now - lastW >= interval) sendDisplay();
}

// =====================================================
//                       MQTT
// =====================================================
void publishState() {
  if (!mqtt.connected()) return;
  char p[380];
  bool sa = (applied == B_ONLY || applied == BOTH);
  bool sb = (applied == A_ONLY || applied == BOTH);
  snprintf(p, sizeof(p),
    "{\"state\":\"%s\",\"signA\":%d,\"signB\":%d,"
    "\"a\":{\"on\":%d,\"p\":%d,\"spd\":%d,\"dist\":%d,\"cnt\":%lu,\"ago\":%d},"
    "\"b\":{\"on\":%d,\"p\":%d,\"spd\":%d,\"dist\":%d,\"cnt\":%lu,\"ago\":%d},"
    "\"x\":%lu,\"e\":%lu,\"up\":%lu}",
    STATE_NAME[applied], sa ? 1 : 0, sb ? 1 : 0,
    nodeA.online() ? 1 : 0, nodeA.active() ? 1 : 0, nodeA.speed, nodeA.dist, nodeA.total, nodeA.ago(),
    nodeB.online() ? 1 : 0, nodeB.active() ? 1 : 0, nodeB.speed, nodeB.dist, nodeB.total, nodeB.ago(),
    foreignCnt, crcErrCnt, millis() / 1000);
  mqtt.publish(TOPIC_STATE, p);          // khong retained: web tu bao cu neu thieu ban tin moi
  lastPub = millis();
}

void publishEvent(char n, int spd, unsigned long cnt) {
  if (!mqtt.connected()) return;
  char p[100];
  snprintf(p, sizeof(p), "{\"n\":\"%c\",\"spd\":%d,\"cnt\":%lu}", n, spd, cnt);
  mqtt.publish(TOPIC_EVENT, p);
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
  if (!mqtt.connected() && now - lastMqttTry > 5000) {
    lastMqttTry = now;
    String cid = "esp3-curve-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    // Last Will: broker tu dat "offline" neu ESP3 mat ket noi
    if (mqtt.connect(cid.c_str(), TOPIC_STATUS, 0, true, "offline")) {
      mqtt.publish(TOPIC_STATUS, "online", true);
      Serial.println("MQTT connected");
      dirty = true;
    }
  }
}

// =====================================================
//                   LOGIC CANH BAO
// =====================================================
void applyOutputs() {
  digitalWrite(SIGN_A_PIN, (applied == B_ONLY || applied == BOTH) ? HIGH : LOW);
  digitalWrite(SIGN_B_PIN, (applied == A_ONLY || applied == BOTH) ? HIGH : LOW);
}

void watchNode(Node& n) {
  bool on = n.online();
  if (on == n.wasOnline) return;
  n.wasOnline = on;
  if (!on) n.present = false;
  Serial.printf("TRAM %c %s\n", n.id, on ? "ONLINE" : "OFFLINE (mat tin hieu)");
  dirty = true;
}

void updateState() {
  watchNode(nodeA);
  watchNode(nodeB);

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

// =====================================================
//                    XU LY BAN TIN
// =====================================================
void handlePayload(char* p) {
  Serial.print("RX: ");
  Serial.println(p);
  char node;
  unsigned int seq;
  int pr, spd, dist, peak;
  unsigned long total;

  if (p[0] == 'S') {
    if (sscanf(p, "S,%c,%d,%d,%d,%lu", &node, &pr, &spd, &dist, &total) != 5) return;
    Node* n = pick(node);
    if (!n) return;
    if (n->present != (pr == 1)) dirty = true;
    n->present = (pr == 1);
    n->speed = spd;
    n->dist = dist;
    n->total = total;
    n->lastRx = millis();

  } else if (p[0] == 'P') {
    if (sscanf(p, "P,%c,%u,%d,%d,%d,%lu", &node, &seq, &pr, &spd, &dist, &total) != 6) return;
    Node* n = pick(node);
    if (!n) return;
    sendAck(node, seq);
    n->lastRx = millis();
    if (n->lastSeq == (int)seq) return;       // ban tin lap -> chi ACK
    n->lastSeq = seq;
    n->present = (pr == 1);
    n->speed = spd;
    n->dist = dist;
    n->total = total;
    dirty = true;

  } else if (p[0] == 'V') {
    if (sscanf(p, "V,%c,%u,%d,%lu", &node, &seq, &peak, &total) != 4) return;
    Node* n = pick(node);
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
    crcErrCnt++;
    Serial.println("RX loi checksum, bo");
    return;
  }
  *star = 0;
  char* p = line + 1;

  size_t nl = strlen(NET_ID);
  if (strncmp(p, NET_ID, nl) != 0 || p[nl] != ':') {   // thiet bi LoRa khac hoac code cu
    foreignCnt++;
    return;
  }
  handlePayload(p + nl + 1);
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

// =====================================================
void setup() {
  Serial.begin(115200);
  nodeA.id = 'A';
  nodeB.id = 'B';

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

  Serial.printf("ESP3 san sang (mang %s)\n", NET_ID);
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

  // Bao khi nghe thay goi cua thiet bi/mang khac (nguyen nhan "tram van online" du da rut)
  if (millis() - lastDiag >= 10000) {
    lastDiag = millis();
    if (foreignCnt > 0) Serial.printf("CANH BAO: %lu goi khac mang (NET_ID khac hoac code cu)\n", foreignCnt);
  }
}
