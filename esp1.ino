// ===== TRAM RADAR: HLK-LD2451 + LoRa E32 + man hinh LED P5 HUB75 =====
// Dung chung cho tram A va tram B: chi doi NODE_ID roi nap.
#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <ESP32-VirtualMatrixPanel-I2S-DMA.h>

#define NODE_ID 'A'     // <<< 'A' cho board 1, 'B' cho board 2
#define NET_ID  "KC"    // ma mang LoRa: giong het ESP3 va tram con lai, nen dat rieng

// ---------- Man hinh HUB75 ----------
#define PANEL_RES_X 64
#define PANEL_RES_Y 32
#define NUM_ROWS 1
#define NUM_COLS 1
#define PANEL_CHAIN (NUM_ROWS * NUM_COLS)

#define R1_PIN  25
#define G1_PIN  26
#define B1_PIN  27
#define R2_PIN  14
#define G2_PIN  12
#define B2_PIN  13
#define A_PIN   23
#define B_PIN   19
#define C_PIN   5
#define D_PIN   -1
#define E_PIN   -1
#define LAT_PIN 4
#define OE_PIN  15
#define CLK_PIN 16

MatrixPanel_I2S_DMA *dma_display = nullptr;
VirtualMatrixPanel  *matrix = nullptr;
uint16_t COL_BLACK, COL_RED, COL_YELLOW, COL_GREEN, COL_GREY, COL_WHITE;

// Canh bao nhan tu ESP3
const int FAST_KMH = 30;                      // >= muc nay: "di nhanh" (do)
const unsigned long LINK_TIMEOUT_MS = 20000;  // lau khong nhan lenh ESP3 -> bao mat song
const unsigned long WARN_TIMEOUT_MS = 6000;   // lau khong nhan lenh canh bao -> tat
bool warnOn = false;
int  warnSpeed = 0;
unsigned long lastW = 0, lastFrame = 0;
int scrollX = PANEL_RES_X;

// ---------- LoRa E32 ----------
const int LORA_AUX = 34;   // input only
const int LORA_RX  = 35;   // ESP32 RX <- E32 TX (input only)
const int LORA_TX  = 18;   // ESP32 TX -> E32 RX
const int LORA_M1  = 33;
const int LORA_M0  = 32;

// ---------- Radar LD2451 ----------
const int RADAR_RX = 22;   // ESP32 RX <- HLK TX
const int RADAR_TX = 17;   // ESP32 TX -> HLK RX

// Thong so phat hien
const int  MIN_SPEED_KMH = 5;
const int  MAX_DIST_M    = 50;
const int  CONFIRM_HITS  = 2;
const unsigned long HOLD_MS      = 2000;
const unsigned long HB_ACTIVE_MS = 1000;
const unsigned long HB_IDLE_MS   = 5000;

// Truyen tin cay
const unsigned long ACK_TIMEOUT_MS = 1200;
const int MAX_RETRY = 8;
const int QSIZE = 8;

// Trang thai xe
bool present = false;
int  curSpeed = 0, curDist = 0, peakSpeed = 0, hits = 0;
unsigned long lastSeen = 0, lastHb = 0, totalCount = 0;

// Hang doi ban tin can ACK
struct Msg { char text[56]; uint8_t seq; };
Msg q[QSIZE];
int qHead = 0, qCount = 0, tries = 0;
unsigned long lastSend = 0, retryDelay = ACK_TIMEOUT_MS;
uint8_t seqNo;

// Khung radar
const uint8_t HDR[4] = {0xF4, 0xF3, 0xF2, 0xF1};
uint8_t buf[96];
uint8_t idx = 0;
int dataLen = 0;

// Nhan LoRa
char rxBuf[64];
int rxIdx = 0;

// =====================================================
//                    MAN HINH
// =====================================================
void initDisplay() {
  HUB75_I2S_CFG::i2s_pins _pins = {
    R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
    A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
    LAT_PIN, OE_PIN, CLK_PIN
  };
  HUB75_I2S_CFG mxconfig(PANEL_RES_X * 2, PANEL_RES_Y / 2, PANEL_CHAIN, _pins);
  mxconfig.double_buff = true;
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  if (!dma_display->begin()) Serial.println("!!! Cap phat bo nho DMA that bai !!!");
  dma_display->setBrightness8(100);
  dma_display->clearScreen();

  matrix = new VirtualMatrixPanel(*dma_display, NUM_ROWS, NUM_COLS, PANEL_RES_X, PANEL_RES_Y);
  matrix->setPhysicalPanelScanRate(FOUR_SCAN_32PX_HIGH);
  matrix->setTextWrap(false);

  COL_BLACK  = dma_display->color565(0, 0, 0);
  COL_RED    = dma_display->color565(255, 0, 0);
  COL_YELLOW = dma_display->color565(255, 180, 0);
  COL_GREEN  = dma_display->color565(0, 220, 0);
  COL_GREY   = dma_display->color565(120, 120, 120);
  COL_WHITE  = dma_display->color565(255, 255, 255);
}

void drawCentered(const char* s, int y, int size, uint16_t col) {
  int x = (PANEL_RES_X - (int)strlen(s) * 6 * size) / 2;
  if (x < 0) x = 0;
  matrix->setTextSize(size);
  matrix->setTextColor(col);
  matrix->setCursor(x, y);
  matrix->print(s);
}

void drawScreen() {
  unsigned long now = millis();
  matrix->fillRect(0, 0, PANEL_RES_X, PANEL_RES_Y, COL_BLACK);

  bool link = (lastW != 0) && (now - lastW < LINK_TIMEOUT_MS);
  bool warn = link && warnOn && (now - lastW < WARN_TIMEOUT_MS);
  char label[12];
  snprintf(label, sizeof(label), "TRAM %c", NODE_ID);

  if (warn) {
    bool fast = warnSpeed >= FAST_KMH;
    uint16_t col = fast ? COL_RED : COL_YELLOW;

    if ((now / 400) % 2 == 0) drawCentered("CANH BAO", 0, 1, col);

    char sp[8];
    snprintf(sp, sizeof(sp), "%d", warnSpeed);
    int wDigits = strlen(sp) * 12;
    int x0 = (PANEL_RES_X - (wDigits + 2 + 24)) / 2;
    if (x0 < 0) x0 = 0;
    matrix->setTextSize(2);
    matrix->setTextColor(col);
    matrix->setCursor(x0, 8);
    matrix->print(sp);
    matrix->setTextSize(1);
    matrix->setCursor(x0 + wDigits + 2, 16);
    matrix->print("km/h");

    const char* msg = fast ? "HUONG DOI DIEN CO XE DI NHANH   "
                           : "HUONG DOI DIEN CO XE DANG DEN   ";
    int msgW = strlen(msg) * 6;
    matrix->setTextColor(COL_WHITE);
    matrix->setCursor(scrollX, 24);
    matrix->print(msg);
    scrollX--;
    if (scrollX < -msgW) scrollX = PANEL_RES_X;

  } else if (link) {
    scrollX = PANEL_RES_X;
    drawCentered("AN TOAN", 6, 1, COL_GREEN);
    drawCentered(label, 18, 1, COL_GREY);

  } else {
    scrollX = PANEL_RES_X;
    drawCentered("MAT SONG", 2, 1, COL_GREY);
    drawCentered("VOI ESP3", 12, 1, COL_GREY);
    drawCentered(label, 22, 1, COL_GREY);
  }
}

void displayTick() {
  if (millis() - lastFrame < 40) return;   // ~25 khung/giay
  lastFrame = millis();
  drawScreen();
  dma_display->flipDMABuffer();
}

// =====================================================
//                       LORA
// =====================================================
// Khung: $<NET>:<noi dung>*<CS>\n   (CS = XOR phan giua $ va *)
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

void sendFrame(const char* payload) {
  delay(random(5, 60));          // giam dung do voi tram con lai
  waitAux();
  char body[72];
  snprintf(body, sizeof(body), "%s:%s", NET_ID, payload);
  char out[96];
  int n = snprintf(out, sizeof(out), "$%s*%02X\n", body, crcOf(body, strlen(body)));
  Serial1.write((const uint8_t*)out, n);
  Serial.print("TX: ");
  Serial.print(out);
}

void enqueue(const char* payload, uint8_t seq) {
  if (qCount >= QSIZE) {
    qHead = (qHead + 1) % QSIZE;
    qCount--;
    tries = 0;
  }
  int tail = (qHead + qCount) % QSIZE;
  strncpy(q[tail].text, payload, sizeof(q[tail].text) - 1);
  q[tail].text[sizeof(q[tail].text) - 1] = 0;
  q[tail].seq = seq;
  qCount++;
}

void queueTick() {
  if (qCount == 0) return;
  unsigned long now = millis();
  if (tries > 0 && now - lastSend < retryDelay) return;
  if (tries >= MAX_RETRY) {
    Serial.println("DROP (khong co ACK)");
    qHead = (qHead + 1) % QSIZE;
    qCount--;
    tries = 0;
    return;
  }
  sendFrame(q[qHead].text);
  tries++;
  lastSend = millis();
  retryDelay = ACK_TIMEOUT_MS + random(0, 300);
}

void onAck(uint8_t seq) {
  if (qCount > 0 && q[qHead].seq == seq) {
    qHead = (qHead + 1) % QSIZE;
    qCount--;
    tries = 0;
    Serial.printf("ACK %u\n", seq);
  }
}

void sendHeartbeat() {
  char p[48];
  snprintf(p, sizeof(p), "S,%c,%d,%d,%d,%lu", NODE_ID, present ? 1 : 0,
           present ? curSpeed : 0, present ? curDist : 0, totalCount);
  sendFrame(p);
  lastHb = millis();
}

void queuePresence(bool on) {
  char p[56];
  uint8_t s = seqNo++;
  snprintf(p, sizeof(p), "P,%c,%u,%d,%d,%d,%lu", NODE_ID, s, on ? 1 : 0,
           on ? curSpeed : 0, on ? curDist : 0, totalCount);
  enqueue(p, s);
}

void queueVehicle() {
  char p[56];
  uint8_t s = seqNo++;
  snprintf(p, sizeof(p), "V,%c,%u,%d,%lu", NODE_ID, s, peakSpeed, totalCount);
  enqueue(p, s);
}

// ---------- Radar ----------
void parseTargets(const uint8_t* d, int len) {
  int bestSpeed = -1, bestDist = 0;
  if (len >= 2) {
    int count = d[0];
    if (count > 0 && len >= 2 + 5 * count) {
      for (int i = 0; i < count; i++) {
        const uint8_t* t = d + 2 + 5 * i;
        int dist = t[1];
        int dir  = t[2];     // 1 = lai gan, 0 = ra xa
        int spd  = t[3];     // km/h
        if (dir == 1 && spd >= MIN_SPEED_KMH && dist <= MAX_DIST_M && spd > bestSpeed) {
          bestSpeed = spd;
          bestDist = dist;
        }
      }
    }
  }

  if (bestSpeed < 0) {
    if (!present) hits = 0;
    return;
  }
  if (!present && ++hits < CONFIRM_HITS) return;

  bool newPass = !present;
  if (newPass) {
    present = true;
    totalCount++;
    peakSpeed = 0;
  }
  lastSeen = millis();
  curSpeed = bestSpeed;
  curDist = bestDist;
  if (bestSpeed > peakSpeed) peakSpeed = bestSpeed;
  if (newPass) queuePresence(true);
}

void feed(uint8_t b) {
  if (idx < 4) {
    if (b == HDR[idx]) buf[idx++] = b;
    else { idx = 0; if (b == HDR[0]) buf[idx++] = b; }
    return;
  }
  buf[idx++] = b;
  if (idx == 6) {
    dataLen = buf[4] | (buf[5] << 8);
    if (dataLen > 80) { idx = 0; return; }
  }
  if (idx >= 6 && idx == 6 + dataLen + 4) {
    if (buf[idx - 4] == 0xF8 && buf[idx - 3] == 0xF7 &&
        buf[idx - 2] == 0xF6 && buf[idx - 1] == 0xF5) {
      parseTargets(buf + 6, dataLen);
    }
    idx = 0;
  }
  if (idx >= sizeof(buf)) idx = 0;
}

// ---------- Nhan tu ESP3 (ACK + lenh hien thi) ----------
void handleRx(char* line) {
  if (line[0] != '$') return;
  char* star = strrchr(line, '*');
  if (!star || strlen(star) < 3) return;
  int len = star - (line + 1);
  unsigned int cs = 0;
  if (sscanf(star + 1, "%2x", &cs) != 1) return;
  if (crcOf(line + 1, len) != (uint8_t)cs) return;   // sai checksum -> bo
  *star = 0;
  char* p = line + 1;

  size_t nl = strlen(NET_ID);
  if (strncmp(p, NET_ID, nl) != 0 || p[nl] != ':') return;   // khac mang -> bo
  p += nl + 1;

  char node; unsigned int s;
  if (sscanf(p, "K,%c,%u", &node, &s) == 2) {
    if (node == NODE_ID) onAck((uint8_t)s);
    return;
  }

  int sa, sb, spA, spB;
  if (sscanf(p, "W,%d,%d,%d,%d", &sa, &sb, &spA, &spB) == 4) {
    // Tram A canh bao khi co xe tu B (signA), toc do la cua xe o B; tram B nguoc lai
    warnOn    = (NODE_ID == 'A') ? (sa == 1) : (sb == 1);
    warnSpeed = (NODE_ID == 'A') ? spB : spA;
    lastW = millis();
  }
}

void readLora() {
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\r') continue;
    if (c == '\n') {
      rxBuf[rxIdx] = 0;
      if (rxIdx > 0) handleRx(rxBuf);
      rxIdx = 0;
    } else if (rxIdx < (int)sizeof(rxBuf) - 1) {
      rxBuf[rxIdx++] = c;
    } else {
      rxIdx = 0;
    }
  }
}

// =====================================================
void setup() {
  Serial.begin(115200);
  pinMode(LORA_M0, OUTPUT);
  pinMode(LORA_M1, OUTPUT);
  pinMode(LORA_AUX, INPUT);
  digitalWrite(LORA_M0, LOW);
  digitalWrite(LORA_M1, LOW);
  delay(200);

  Serial1.begin(9600, SERIAL_8N1, LORA_RX, LORA_TX);
  Serial2.begin(115200, SERIAL_8N1, RADAR_RX, RADAR_TX);

  initDisplay();

  randomSeed(esp_random());
  seqNo = random(0, 200);

  Serial.printf("TRAM %c san sang (mang %s)\n", NODE_ID, NET_ID);
  sendHeartbeat();
}

void loop() {
  while (Serial2.available()) feed(Serial2.read());
  readLora();

  unsigned long now = millis();

  // Xe da roi vung giam sat
  if (present && now - lastSeen >= HOLD_MS) {
    present = false;
    hits = 0;
    queuePresence(false);
    queueVehicle();
  }

  queueTick();

  unsigned long interval = present ? HB_ACTIVE_MS : HB_IDLE_MS;
  if (qCount == 0 && now - lastHb >= interval) sendHeartbeat();

  displayTick();
}
