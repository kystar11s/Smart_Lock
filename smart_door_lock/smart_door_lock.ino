/*
 * 智能门锁 v8.0
 * ESP32 + RFID + 指纹 + 矩阵键盘 + OLED + 舵机 + ESP32-CAM
 * U8g2 中文显示 + NTP时间 + 云端配置同步
 */

#include <SPI.h>
#include <MFRC522.h>
#include <Servo.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Keypad.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

// ==================== WiFi ====================
#define WIFI_SSID     "ky"
#define WIFI_PASS     "1472583692"
#define SERVER_HOST   "10.132.190.95"
#define SERVER_PORT   8000
#define NTP_SERVER    "ntp.aliyun.com"
#define GMT_OFFSET    28800
#define DAY_OFFSET    0

// ==================== 引脚 ====================
#define PIN_SS        5
#define PIN_RST       -1
#define PIN_SERVO     15
#define PIN_LED       2
#define PIN_BUZZER    4
#define PIN_FP_TX     17
#define PIN_FP_RX     16
#define PIN_SDA       21
#define PIN_SCL       22
#define PIN_CAM_TX    12
#define PIN_CAM_RX    35

// ==================== 参数 ====================
#define ANGLE_LOCK      85
#define ANGLE_UNLOCK    180
#define AUTO_LOCK_MS    5000
#define FP_POLL_MS      3000
#define FP_TIMEOUT_MS   10000
#define UID_LEN         4
#define PWD_LEN         4
#define CONFIG_POLL_MS  3000

byte AUTH_CARD[UID_LEN] = {0x0C, 0xD5, 0xD4, 0xE7};
char AUTH_PWD[PWD_LEN + 1] = "1234";
int configVersion = 0;
unsigned long lastConfigPoll = 0;

#define MAX_RFID 10
#define MAX_FP   10
byte rfidList[MAX_RFID][UID_LEN];
int rfidCount = 0;
int fpList[MAX_FP];
int fpCount = 0;
bool remoteUnlockPending = false;

// ==================== OLED (U8g2) ====================
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);

// ==================== 矩阵键盘 ====================
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {26, 25, 33, 32};
byte colPins[COLS] = {27, 14, 13, 3};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// 按键显示标签 (OLED上显示的文字)
const char* keyLabels[4][4] = {
  {"1","2","3","DEL"},
  {"4","5","6","CLR"},
  {"7","8","9","OK"},
  {"*","0","#","BACK"}
};

// ==================== 对象 ====================
MFRC522 rfid(PIN_SS, PIN_RST);
Servo lockServo;
HardwareSerial fpSerial(2);
HardwareSerial camSerial(1);

// ==================== 状态 ====================
enum SysState { STATE_IDLE, STATE_UNLOCKED };
SysState sysState = STATE_IDLE;
unsigned long unlockTs = 0;

enum FpState { FP_IDLE, FP_LISTENING };
FpState fpState = FP_IDLE;
unsigned long fpPollTs = 0;
unsigned long fpListenStart = 0;
bool fpOnline = false;
bool camOnline = false;

char pwdBuf[PWD_LEN + 1];
int pwdIdx = 0;
bool pwdInputMode = false;

uint8_t fpBuf[64];
uint8_t fpBufLen = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastOledRefresh = 0;

// ==================== 工具 ====================

uint16_t checksum(uint8_t *d, int n) {
  uint16_t s = 0;
  while (n--) s += *d++;
  return s;
}

void printHex(uint8_t *data, int len) {
  for (int i = 0; i < len; i++) {
    if (data[i] < 0x10) Serial.print("0");
    Serial.print(data[i], HEX);
    if (i < len - 1) Serial.print(":");
  }
}

// ==================== OLED 绘制 ====================

// 锁图标
void drawLockIcon(int cx, int cy, bool closed) {
  if (closed) {
    u8g2.drawRBox(cx - 9, cy, 18, 14, 3);
    u8g2.drawRFrame(cx - 6, cy - 8, 12, 10, 3);
    u8g2.drawBox(cx - 4, cy - 6, 8, 4);
    u8g2.drawDisc(cx, cy + 6, 2);
    u8g2.drawBox(cx, cy + 6, 1, 3);
  } else {
    // 开锁: 锁身空心 + 锁梁打开
    u8g2.drawRFrame(cx - 9, cy, 18, 14, 3);
    u8g2.drawRFrame(cx - 6, cy - 8, 12, 10, 3);
    u8g2.drawBox(cx - 4, cy - 6, 8, 4);
  }
}

// ===== 主界面 =====
void oledShowMain() {
  u8g2.clearBuffer();

  // 外框
  u8g2.drawFrame(0, 0, 128, 64);

  // 标题栏 (反色)
  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setDrawColor(0);

  // 标题 居中偏左
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(20, 12, "智能门锁");

  // 时间 右对齐 (8字符 × 6px = 48px, 从x=80开始)
  String timeStr = getTimeString();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(80, 12, timeStr.c_str());
  u8g2.setDrawColor(1);

  // 锁图标
  drawLockIcon(64, 28, true);

  // 分隔线
  u8g2.drawHLine(0, 48, 128);

  // 底部三列
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(16, 62, "刷卡");
  u8g2.drawUTF8(56, 62, "触摸");
  u8g2.drawUTF8(96, 62, "按键");

  u8g2.drawVLine(42, 49, 15);
  u8g2.drawVLine(86, 49, 15);

  u8g2.sendBuffer();
}

// ===== 密码界面 =====
void oledShowPassword() {
  u8g2.clearBuffer();

  // 外框
  u8g2.drawFrame(0, 0, 128, 64);

  // 顶部栏
  u8g2.drawBox(0, 0, 128, 10);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(4, 9, "返回");
  u8g2.drawUTF8(70, 9, "输入密码");
  u8g2.setDrawColor(1);

  // 密码显示 (居中不超框)
  u8g2.drawFrame(20, 12, 88, 12);
  u8g2.setFont(u8g2_font_6x10_tf);
  for (int i = 0; i < PWD_LEN; i++) {
    u8g2.setCursor(28 + i * 20, 22);
    if (i < pwdIdx)
      u8g2.print("*");
    else
      u8g2.print("_");
  }

  u8g2.drawHLine(0, 26, 128);

  // 键盘 (不超框)
  int gy = 27;
  int cw = 31;
  int ch = 9;

  for (int r = 0; r <= 4; r++)
    u8g2.drawHLine(1, gy + r * ch, 126);
  for (int c = 0; c <= 4; c++)
    u8g2.drawVLine(1 + c * cw, gy, 36);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      u8g2.setCursor(2 + c * cw + 8, gy + r * ch + 7);
      u8g2.print(keyLabels[r][c]);
    }
  }

  u8g2.sendBuffer();
}

// ===== 成功 =====
void oledShowGranted() {
  u8g2.clearBuffer();

  u8g2.drawFrame(0, 0, 128, 64);

  // 解锁成功 居中
  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(40, 12, "解锁成功");
  u8g2.setDrawColor(1);

  // 锁图标 居中
  drawLockIcon(64, 26, false);

  // 已开锁 居中
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(46, 44, "已开锁");

  // 分隔线 居中
  u8g2.drawHLine(24, 50, 80);

  // 倒计时 居中
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(24, 58, "5秒后自动关锁");

  u8g2.sendBuffer();
}

// ===== 失败 =====
void oledShowDenied() {
  u8g2.clearBuffer();

  u8g2.drawFrame(0, 0, 128, 64);

  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(34, 12, "验证失败");
  u8g2.setDrawColor(1);

  // X 居中
  u8g2.drawCircle(64, 30, 12);
  u8g2.drawLine(56, 22, 72, 38);
  u8g2.drawLine(72, 22, 56, 38);

  // 拒绝 居中
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(40, 52, "拒绝访问");

  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(28, 62, "密码错误");

  u8g2.sendBuffer();
}

// ===== 倒计时 =====
void oledShowCountdown() {
  u8g2.clearBuffer();

  u8g2.drawFrame(0, 0, 128, 64);

  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(40, 12, "解锁成功");
  u8g2.setDrawColor(1);

  drawLockIcon(64, 26, false);

  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(46, 44, "已开锁");

  int remain = (AUTO_LOCK_MS - (millis() - unlockTs)) / 1000;
  if (remain < 0) remain = 0;
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawHLine(24, 50, 80);
  char buf[16];
  sprintf(buf, "%d秒后关锁", remain);
  u8g2.drawUTF8(28, 58, buf);

  u8g2.sendBuffer();
}

// ==================== 蜂鸣器 ====================

void beep(int count, int ms) {
  for (int i = 0; i < count; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(ms);
    digitalWrite(PIN_BUZZER, LOW);
    if (i < count - 1) delay(ms);
  }
}

// ==================== 指纹串口 ====================

void fpSend(uint8_t cmd, uint8_t *params, uint8_t plen) {
  uint8_t pkt[32];
  int i = 0;
  pkt[i++] = 0xEF; pkt[i++] = 0x01;
  pkt[i++] = 0xFF; pkt[i++] = 0xFF; pkt[i++] = 0xFF; pkt[i++] = 0xFF;
  pkt[i++] = 0x01;
  uint16_t L = plen + 3;
  pkt[i++] = L >> 8; pkt[i++] = L & 0xFF;
  pkt[i++] = cmd;
  for (int j = 0; j < plen; j++) pkt[i++] = params[j];
  uint16_t cs = checksum(&pkt[6], i - 6);
  pkt[i++] = cs >> 8; pkt[i++] = cs & 0xFF;
  fpSerial.write(pkt, i);
}

bool fpReadPacket(unsigned long timeoutMs) {
  fpBufLen = 0;
  unsigned long t = millis();
  while (millis() - t < timeoutMs) {
    if (!fpSerial.available()) continue;
    uint8_t b = fpSerial.read();
    if (fpBufLen == 0 && b != 0xEF) continue;
    fpBuf[fpBufLen++] = b;
    if (fpBufLen >= 9) {
      uint16_t L = (fpBuf[7] << 8) | fpBuf[8];
      if (fpBufLen >= 9 + L) return true;
    }
  }
  return false;
}

uint8_t fpAck()   { return fpBufLen >= 10 ? fpBuf[9]  : 0xFF; }
uint8_t fpStep()  { return fpBufLen >= 10 ? fpBuf[10] : 0x00; }
uint8_t fpIdH()   { return fpBufLen >= 11 ? fpBuf[11] : 0x00; }
uint8_t fpIdL()   { return fpBufLen >= 12 ? fpBuf[12] : 0x00; }
uint8_t fpScore() { return fpBufLen >= 13 ? fpBuf[13] : 0x00; }

// ==================== 指纹LED ====================

void fpLED_breathBlue() {
  uint8_t p[5] = {0x01, 0x01, 0x01, 0x00, 0x08};
  fpSend(0x3C, p, 5);
}

void fpLED_solidGreen() {
  uint8_t p[5] = {0x03, 0x02, 0x02, 0x00, 0x08};
  fpSend(0x3C, p, 5);
}

void fpLED_blinkRed() {
  uint8_t p[5] = {0x02, 0x04, 0x04, 0x00, 0x08};
  fpSend(0x3C, p, 5);
}

// ==================== ESP32-CAM ====================

void initCam() {
  Serial.print("  [..] ESP32-CAM  ");
  camSerial.begin(115200, SERIAL_8N1, PIN_CAM_RX, PIN_CAM_TX);
  delay(2000);
  for (int i = 0; i < 3; i++) {
    camSerial.println("PING");
    unsigned long t = millis();
    while (millis() - t < 1000) {
      if (camSerial.available()) {
        String resp = camSerial.readStringUntil('\n');
        resp.trim();
        if (resp == "PONG") {
          camOnline = true;
          Serial.println("OK");
          return;
        }
      }
    }
    delay(500);
  }
  Serial.println("离线");
}

void camCapture(const char *method, bool success) {
  if (!camOnline) return;
  String cmd = "CAPTURE|";
  cmd += method;
  cmd += "|";
  cmd += success ? "SUCCESS" : "FAIL";
  Serial.println("[CAM] 拍照中...");
  camSerial.println(cmd);
  unsigned long t = millis();
  while (millis() - t < 3000) {
    if (camSerial.available()) {
      String resp = camSerial.readStringUntil('\n');
      resp.trim();
      if (resp == "OK") {
        Serial.println("[CAM] 拍照成功");
      } else {
        Serial.println("[CAM] 拍照失败");
      }
      return;
    }
  }
  Serial.println("[CAM] 超时");
}

void camReportStatus(const char *status) {
  if (!camOnline) return;
  String cmd = "STATUS|";
  cmd += status;
  camSerial.println(cmd);
}

// ==================== 业务逻辑 ====================

void onAccessGranted(const char *method) {
  if (sysState == STATE_UNLOCKED) return;
  Serial.printf("  >> %s 验证通过! 开锁!\n", method);
  beep(1, 200);
  digitalWrite(PIN_LED, HIGH);
  lockServo.write(ANGLE_UNLOCK);
  sysState = STATE_UNLOCKED;
  unlockTs = millis();
  if (strcmp(method, "FP") == 0) fpLED_solidGreen();
  pwdInputMode = false;
  oledShowGranted();
  camReportStatus("UNLOCKED");
  camCapture(method, true);
  delay(1500);
}

void onAccessDenied(const char *method) {
  Serial.printf("  >> %s 验证失败!\n", method);
  beep(3, 100);
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_LED, HIGH); delay(150);
    digitalWrite(PIN_LED, LOW);  delay(150);
  }
  if (strcmp(method, "FP") == 0) fpLED_blinkRed();
  oledShowDenied();
  camCapture(method, false);
  delay(1500);
  resetPwdInput();
  if (pwdInputMode) oledShowPassword();
  else oledShowMain();
}

void autoLock() {
  Serial.println("  >> 自动关锁");
  lockServo.write(ANGLE_LOCK);
  digitalWrite(PIN_LED, LOW);
  sysState = STATE_IDLE;
  fpLED_breathBlue();
  pwdInputMode = false;
  camReportStatus("LOCKED");
  oledShowMain();
}

// ==================== 密码键盘 ====================

void resetPwdInput() {
  memset(pwdBuf, 0, sizeof(pwdBuf));
  pwdIdx = 0;
}

void checkPassword() {
  pwdBuf[PWD_LEN] = '\0';
  if (strcmp(pwdBuf, AUTH_PWD) == 0) {
    Serial.println("[密码] 验证通过");
    onAccessGranted("密码");
  } else {
    Serial.println("[密码] 密码错误");
    onAccessDenied("密码");
  }
}

void handleKeypad() {
  char key = keypad.getKey();
  if (!key) return;
  if (sysState == STATE_UNLOCKED) return;

  if (!pwdInputMode) {
    pwdInputMode = true;
    resetPwdInput();
    oledShowPassword();
    return;
  }

  // # = 确认提交
  if (key == '#') {
    if (pwdIdx == PWD_LEN) checkPassword();
    return;
  }

  // * = 删除最后一位
  if (key == '*') {
    if (pwdIdx > 0) {
      pwdIdx--;
      pwdBuf[pwdIdx] = '\0';
      oledShowPassword();
    }
    return;
  }

  // D = 返回主界面
  if (key == 'D') {
    resetPwdInput();
    pwdInputMode = false;
    oledShowMain();
    return;
  }

  // A = 删除 (同 *)
  if (key == 'A') {
    if (pwdIdx > 0) {
      pwdIdx--;
      pwdBuf[pwdIdx] = '\0';
      oledShowPassword();
    }
    return;
  }

  // B = 清空全部
  if (key == 'B') {
    resetPwdInput();
    oledShowPassword();
    return;
  }

  // C = 确认 (同 #)
  if (key == 'C') {
    if (pwdIdx == PWD_LEN) checkPassword();
    return;
  }

  // 数字输入
  if (key >= '0' && key <= '9' && pwdIdx < PWD_LEN) {
    pwdBuf[pwdIdx++] = key;
    oledShowPassword();
  }
}

// ==================== RFID ====================

bool isAuthCard() {
  if (rfidCount == 0) return false;
  for (int c = 0; c < rfidCount; c++) {
    bool match = true;
    for (byte i = 0; i < UID_LEN; i++) {
      if (rfid.uid.uidByte[i] != rfidList[c][i]) { match = false; break; }
    }
    if (match) return true;
  }
  return false;
}

void pollRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;
  Serial.print("[RFID] 卡片: ");
  printHex(rfid.uid.uidByte, rfid.uid.size);
  Serial.println();
  if (isAuthCard()) onAccessGranted("RFID");
  else {
    Serial.println("  未知卡片");
    onAccessDenied("RFID");
  }
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ==================== 指纹 ====================

void fpStartIdentify() {
  fpBufLen = 0;
  fpState = FP_LISTENING;
  fpListenStart = millis();
  uint8_t p[6] = {0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00};
  fpSend(0x32, p, 6);
}

void fpPoll() {
  if (fpState != FP_LISTENING) return;
  if (millis() - fpListenStart > FP_TIMEOUT_MS) {
    fpState = FP_IDLE;
    return;
  }
  while (fpSerial.available()) {
    uint8_t b = fpSerial.read();
    if (fpBufLen == 0 && b != 0xEF) continue;
    fpBuf[fpBufLen++] = b;
    if (fpBufLen >= 9) {
      uint16_t L = (fpBuf[7] << 8) | fpBuf[8];
      if (fpBufLen >= 9 + L) {
        uint8_t step = fpStep();
        uint8_t ack  = fpAck();
        if (step == 0x05) {
          fpState = FP_IDLE;
          if (ack == 0x00) {
            uint16_t id = (fpIdH() << 8) | fpIdL();
            Serial.printf("[指纹] 识别成功 ID=%d\n", id);
            onAccessGranted("指纹");
          } else if (ack == 0x09) {
            Serial.println("[指纹] 识别错误 未匹配");
            onAccessDenied("指纹");
          }
        }
        fpBufLen = 0;
      }
    }
  }
}

// ==================== 初始化 ====================

void showBanner() {
  Serial.println();
  Serial.println("  ╔══════════════════════════════╗");
  Serial.println("  ║      智 能 门 锁 v8.0       ║");
  Serial.println("  ║  RFID+指纹+键盘+OLED+舵机   ║");
  Serial.println("  ╚══════════════════════════════╝");
  Serial.println();
}

void initOLED() {
  Serial.print("  [..] OLED 128x64  ");
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.sendBuffer();
  Serial.println("OK");
}

void initRFID() {
  Serial.print("  [..] RFID RC522   ");
  SPI.begin();
  rfid.PCD_Init();
  delay(100);
  Serial.println("OK");
}

void initServo() {
  Serial.print("  [..] SG90 舵机    ");
  lockServo.attach(PIN_SERVO);
  lockServo.write(ANGLE_LOCK);
  delay(500);
  Serial.println("OK");
}

void initKeypad() {
  Serial.print("  [..] 4x4 键盘     ");
  for (int i = 0; i < ROWS; i++) {
    pinMode(rowPins[i], OUTPUT);
    digitalWrite(rowPins[i], HIGH);
  }
  for (int i = 0; i < COLS; i++) {
    pinMode(colPins[i], INPUT_PULLUP);
  }
  Serial.println("OK");
}

void initFingerprint() {
  Serial.print("  [..] ZW101 指纹   ");
  fpSerial.begin(57600, SERIAL_8N1, PIN_FP_RX, PIN_FP_TX);
  delay(1000);
  while (fpSerial.available()) fpSerial.read();
  delay(200);
  fpSend(0x35, NULL, 0);
  delay(200);
  if (fpReadPacket(1000) && fpAck() == 0x00) {
    fpOnline = true;
    fpLED_breathBlue();
    Serial.println("OK");
  } else {
    Serial.println("离线");
  }
}

// ==================== WiFi & NTP ====================

void initWiFi() {
  Serial.print("  [..] WiFi         ");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 5000) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("OK");
    configTime(GMT_OFFSET, DAY_OFFSET, NTP_SERVER);
  } else {
    Serial.println("离线");
  }
}

String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "--:--:--";
  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(buf);
}

// ==================== 配置同步 ====================

void pollConfig() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = "http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "/api/config";
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();

    int vIdx = payload.indexOf("\"version\":");
    if (vIdx > 0) {
      int vStart = vIdx + 11;
      while (payload[vStart] == ' ') vStart++;
      int vEnd = payload.indexOf(",", vStart);
      if (vEnd < 0) vEnd = payload.indexOf("}", vStart);
      if (vEnd > vStart) {
        int newVer = payload.substring(vStart, vEnd).toInt();
        if (newVer > configVersion) {
          configVersion = newVer;

          int pIdx = payload.indexOf("\"door_password\":");
          if (pIdx > 0) {
            int pStart = payload.indexOf("\"", pIdx + 16) + 1;
            int pEnd = payload.indexOf("\"", pStart);
            if (pEnd > pStart) {
              String newPwd = payload.substring(pStart, pEnd);
              newPwd.toCharArray(AUTH_PWD, PWD_LEN + 1);
            }
          }

          rfidCount = 0;
          int rIdx = payload.indexOf("\"rfid_whitelist\"");
          if (rIdx > 0) {
            int arrStart = payload.indexOf("[", rIdx);
            int arrEnd = payload.indexOf("]", arrStart);
            if (arrEnd > arrStart) {
              String arr = payload.substring(arrStart + 1, arrEnd);
              int pos = 0;
              while (pos < arr.length() && rfidCount < MAX_RFID) {
                int s = arr.indexOf("\"", pos);
                if (s < 0) break;
                int e = arr.indexOf("\"", s + 1);
                if (e < 0) break;
                String hex = arr.substring(s + 1, e);
                for (int i = 0; i < UID_LEN; i++) {
                  rfidList[rfidCount][i] = strtoul(hex.substring(i * 3, i * 3 + 2).c_str(), NULL, 16);
                }
                rfidCount++;
                pos = e + 1;
              }
            }
          }

          fpCount = 0;
          int fIdx = payload.indexOf("\"fp_whitelist\"");
          if (fIdx > 0) {
            int arrStart = payload.indexOf("[", fIdx);
            int arrEnd = payload.indexOf("]", arrStart);
            if (arrEnd > arrStart) {
              String arr = payload.substring(arrStart + 1, arrEnd);
              int pos = 0;
              while (pos < arr.length() && fpCount < MAX_FP) {
                while (pos < arr.length() && (arr[pos] == ' ' || arr[pos] == ',')) pos++;
                int e = arr.indexOf(",", pos);
                if (e < 0) e = arr.indexOf("]", pos);
                if (e > pos) {
                  fpList[fpCount] = arr.substring(pos, e).toInt();
                  fpCount++;
                }
                pos = e + 1;
              }
            }
          }

          Serial.printf("[SYNC] v%d pwd=%s rfid=%d fp=%d\n", configVersion, AUTH_PWD, rfidCount, fpCount);
        }
      }
    }

    int uIdx = payload.indexOf("\"unlock_request\":");
    if (uIdx > 0) {
      int uStart = uIdx + 17;
      while (payload[uStart] == ' ') uStart++;
      if (payload[uStart] == '1' && !remoteUnlockPending) {
        remoteUnlockPending = true;
        Serial.println("[REMOTE] 远程开锁请求");
        onAccessGranted("远程");
        HTTPClient http2;
        http2.begin("http://" + String(SERVER_HOST) + ":" + String(SERVER_PORT) + "/api/unlock/done");
        http2.setTimeout(2000);
        http2.GET();
        http2.end();
        remoteUnlockPending = false;
      }
    }
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  showBanner();
  Serial.println("  初始化硬件:");
  initOLED();
  initWiFi();
  initRFID();
  initServo();
  initKeypad();
  initFingerprint();
  initCam();

  delay(500);
  oledShowMain();
  beep(1, 200);

  Serial.println();
  Serial.println("  ┌─────────────────────────────┐");
  Serial.println("  │  [RFID]  刷卡开锁            │");
  if (fpOnline)
    Serial.println("  │  [指纹]  放手指开锁          │");
  Serial.println("  │  [密码]  按任意键开始输入     │");
  Serial.println("  │    * 删除  B 清空  # 确认    │");
  Serial.println("  │    D 返回主界面              │");
  Serial.println("  └─────────────────────────────┘");
  Serial.println();
}

// ==================== 主循环 ====================

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(500);
    return;
  }

  if (sysState == STATE_IDLE) {
    handleKeypad();
    pollRFID();
    if (millis() - lastOledRefresh >= 1000) {
      lastOledRefresh = millis();
      if (!pwdInputMode) oledShowMain();
    }
  }

  if (sysState == STATE_IDLE && fpOnline) {
    if (fpState == FP_IDLE && millis() - fpPollTs >= FP_POLL_MS) {
      fpPollTs = millis();
      fpStartIdentify();
    }
    fpPoll();
  }

  if (sysState == STATE_IDLE && millis() - lastConfigPoll >= CONFIG_POLL_MS) {
    lastConfigPoll = millis();
    pollConfig();
  }

  if (sysState == STATE_UNLOCKED) {
    if (millis() - unlockTs >= AUTO_LOCK_MS) {
      autoLock();
    } else {
      if (millis() - lastDisplayUpdate >= 500) {
        lastDisplayUpdate = millis();
        oledShowCountdown();
      }
    }
  }

  delay(10);
}
