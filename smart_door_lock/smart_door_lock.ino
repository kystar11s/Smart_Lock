/*
 * 智能门锁 v9.0
 * ESP32 + RFID + 指纹 + 矩阵键盘 + OLED + 舵机 + ESP32-CAM
 * U8g2 中文显示 + NTP时间 + 云端配置同步
 * 暴力破解防护 + 黑名单管理 + OLED升级
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
#include "BluetoothSerial.h"
#include <Preferences.h>

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
#define CONFIG_POLL_MS  10000

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
bool configSyncFailed = false;

// ==================== 暴力破解防护 ====================
int failCount = 0;
unsigned long lockoutUntil = 0;
#define FAIL_THRESHOLD 5
#define LOCKOUT_MS    30000UL

// ==================== 黑名单 ====================
#define MAX_RFID_BL 10
#define MAX_FP_BL   10
byte rfidBlackList[MAX_RFID_BL][UID_LEN];
int rfidBlackCount = 0;
int fpBlackList[MAX_FP_BL];
int fpBlackCount = 0;

// ==================== 经典蓝牙 SPP ====================
BluetoothSerial SerialBT;

// ==================== Flash持久化 ====================
Preferences prefs;

// ==================== 离线日志缓存 ====================
#define MAX_OFFLINE_LOGS 20
struct OfflineLog {
  char method[8];
  char result[8];
  unsigned long timestamp;
};
OfflineLog offlineLogs[MAX_OFFLINE_LOGS];
int offlineLogCount = 0;

// ==================== 管理菜单 ====================
bool adminMode = false;
int adminPage = 0;        // 0-5: 菜单页
#define ADMIN_MENU_COUNT 6
const char* adminMenuItems[] = {
  "添加RFID卡", "删除RFID卡",
  "添加指纹",   "删除指纹",
  "修改密码",   "返回主界面"
};
// 0=等待操作, 1=等待RFID刷卡, 2=等待指纹按压, 3=输入新密码, 4=确认中
int adminSubMode = 0;
int adminSelIdx = 0;      // 删除列表中的选中索引
unsigned long starPressTs = 0;
#define LONG_PRESS_MS 800

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

// ==================== 蓝牙回调/处理 ====================

void onAccessGranted(const char* method);
void onAccessDenied(const char* method);

void handleBluetoothApp() {
  if (!SerialBT.available()) return;

  String value = SerialBT.readString();
  value.trim();
  if (value.length() == 0) return;

  Serial.printf("[BT] 收到密码: %s\n", value.c_str());

  // 锁定中拒绝
  if (millis() < lockoutUntil) {
    SerialBT.println("LOCKED");
    Serial.println("[BT] 已锁定 拒绝操作");
    return;
  }

  if (value == AUTH_PWD) {
    SerialBT.println("OK");
    Serial.println("[BT] 密码正确 开锁!");
    onAccessGranted("蓝牙");
  } else {
    SerialBT.println("FAIL");
    Serial.println("[BT] 密码错误");
    failCount++;
    if (failCount >= FAIL_THRESHOLD) {
      lockoutUntil = millis() + LOCKOUT_MS;
      Serial.println("[SECURITY] 蓝牙密码连续错误5次 已锁定30秒!");
      sendBruteForceAlert();
    }
    onAccessDenied("蓝牙");
  }
}

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
  u8g2.drawBox(0, 0, 128, 12);
  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(2, 10, "智能门锁");

  // 右上角: WiFi/BT状态 + 小锁图标
  u8g2.setFont(u8g2_font_5x7_tf);
  if (WiFi.status() == WL_CONNECTED)
    u8g2.drawStr(78, 9, "WiFi");
  else
    u8g2.drawStr(78, 9, "---");
  u8g2.drawStr(100, 9, "BT");
  if (sysState == STATE_UNLOCKED) {
    u8g2.drawRFrame(116, 2, 8, 7, 2);
  } else {
    u8g2.drawRBox(116, 2, 8, 7, 2);
  }
  u8g2.setDrawColor(1);

  // 大字时间 (居中)
  String timeStr = getTimeString();
  u8g2.setFont(u8g2_font_logisoso16_tf);
  int tw = u8g2.getUTF8Width(timeStr.c_str());
  u8g2.drawUTF8((128 - tw) / 2, 30, timeStr.c_str());

  // 日期含年份 (时间下方居中)
  String dateStr = getDateString();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  int dw = u8g2.getUTF8Width(dateStr.c_str());
  u8g2.drawUTF8((128 - dw) / 2, 42, dateStr.c_str());

  // 分隔线
  u8g2.drawHLine(0, 45, 128);

  // 锁定倒计时 or 底部三列
  if (millis() < lockoutUntil) {
    int remain = (lockoutUntil - millis()) / 1000;
    if (remain < 1) remain = 1;
    char lockBuf[20];
    sprintf(lockBuf, "已锁定 %ds", remain);
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    int lw = u8g2.getUTF8Width(lockBuf);
    u8g2.drawUTF8((128 - lw) / 2, 57, lockBuf);
  } else {
    u8g2.setFont(u8g2_font_wqy12_t_gb2312);
    u8g2.drawUTF8(8, 57, "刷卡");
    u8g2.drawUTF8(52, 57, "指纹");
    u8g2.drawUTF8(96, 57, "密码");

    u8g2.drawVLine(40, 46, 17);
    u8g2.drawVLine(84, 46, 17);
  }

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

  // 失败次数提示
  if (failCount > 0 && millis() >= lockoutUntil) {
    int remain = FAIL_THRESHOLD - failCount;
    char hint[20];
    sprintf(hint, "WARN:%dleft", remain);
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(32, 32, hint);
  }

  u8g2.drawHLine(0, 34, 128);

  // 键盘 4x4 (不超框)
  int gy = 35;
  int cw = 31;
  int ch = 7;

  for (int r = 0; r <= 4; r++)
    u8g2.drawHLine(1, gy + r * ch, 126);
  for (int c = 0; c <= 4; c++)
    u8g2.drawVLine(1 + c * cw, gy, 28);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      u8g2.setCursor(2 + c * cw + 8, gy + r * ch + 6);
      u8g2.print(keyLabels[r][c]);
    }
  }

  u8g2.sendBuffer();
}

// ===== 锁定界面 =====
void oledShowLockout() {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);

  // 标题
  u8g2.drawBox(0, 0, 128, 14);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(34, 12, "安全锁定");
  u8g2.setDrawColor(1);

  // 锁图标
  drawLockIcon(64, 22, true);

  // 倒计时
  int remain = (lockoutUntil - millis()) / 1000;
  if (remain < 1) remain = 1;
  char buf[20];
  sprintf(buf, "%d秒后解除", remain);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  int bw = u8g2.getUTF8Width(buf);
  u8g2.drawUTF8((128 - bw) / 2, 52, buf);

  // 提示
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(16, 62, "密码连续错误已锁定");

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

// ===== 管理菜单 =====
void oledShowAdminMenu() {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);
  // 标题
  u8g2.drawBox(0, 0, 128, 12);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(34, 10, "系统管理");
  u8g2.setDrawColor(1);

  // 计算滚动偏移，让选中项尽量居中显示
  int maxVisible = 4;
  int start = adminPage - 1;
  if (start < 0) start = 0;
  if (start + maxVisible > ADMIN_MENU_COUNT) start = ADMIN_MENU_COUNT - maxVisible;
  if (start < 0) start = 0;

  for (int i = 0; i < maxVisible && start + i < ADMIN_MENU_COUNT; i++) {
    int y = 24 + i * 12;
    if (start + i == adminPage) {
      u8g2.drawBox(2, y - 9, 124, 12);
      u8g2.setDrawColor(0);
    }
    u8g2.drawUTF8(8, y, adminMenuItems[start + i]);
    u8g2.setDrawColor(1);
  }

  u8g2.sendBuffer();
}

void oledShowAdminAddRfid() {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawBox(0, 0, 128, 12);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(28, 10, "添加RFID卡");
  u8g2.setDrawColor(1);
  u8g2.drawUTF8(16, 32, "请将新卡靠近");
  u8g2.drawUTF8(24, 46, "RFID模块");
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(40, 60, "D=Cancel");
  u8g2.sendBuffer();
}

void oledShowAdminAddRfidResult(bool ok, const char* uid) {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  if (ok) {
    u8g2.drawUTF8(24, 24, "添加成功!");
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(16, 42, uid);
  } else {
    u8g2.drawUTF8(16, 24, "添加失败/已存在");
  }
  u8g2.sendBuffer();
}

void oledShowAdminDelList() {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawBox(0, 0, 128, 12);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(34, 10, "删除RFID卡");
  u8g2.setDrawColor(1);

  if (rfidCount == 0) {
    u8g2.drawUTF8(24, 36, "列表为空");
  } else {
    int start = adminSelIdx;
    for (int i = 0; i < 4 && start + i < rfidCount; i++) {
      int y = 24 + i * 12;
      if (start + i == adminSelIdx) {
        u8g2.drawBox(2, y - 9, 124, 12);
        u8g2.setDrawColor(0);
      }
      char buf[20];
      sprintf(buf, "%d. %02X:%02X:%02X:%02X", start + i + 1,
        rfidList[start + i][0], rfidList[start + i][1],
        rfidList[start + i][2], rfidList[start + i][3]);
      u8g2.drawUTF8(8, y, buf);
      u8g2.setDrawColor(1);
    }
  }
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(4, 60, "#Del D=Back");
  u8g2.sendBuffer();
}

void oledShowAdminAddFp() {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawBox(0, 0, 128, 12);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(28, 10, "添加指纹");
  u8g2.setDrawColor(1);
  u8g2.drawUTF8(20, 30, "请将手指放到");
  u8g2.drawUTF8(20, 44, "指纹模块上");
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(40, 60, "D=Cancel");
  u8g2.sendBuffer();
}

void oledShowAdminDelFpList() {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawBox(0, 0, 128, 12);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(34, 10, "删除指纹");
  u8g2.setDrawColor(1);

  if (fpCount == 0) {
    u8g2.drawUTF8(24, 36, "列表为空");
  } else {
    int start = adminSelIdx;
    for (int i = 0; i < 4 && start + i < fpCount; i++) {
      int y = 24 + i * 12;
      if (start + i == adminSelIdx) {
        u8g2.drawBox(2, y - 9, 124, 12);
        u8g2.setDrawColor(0);
      }
      char buf[16];
      sprintf(buf, "指纹 ID=%d", fpList[start + i]);
      u8g2.drawUTF8(8, y, buf);
      u8g2.setDrawColor(1);
    }
  }
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(4, 60, "#Del D=Back");
  u8g2.sendBuffer();
}

void oledShowAdminChangePwd() {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.drawBox(0, 0, 128, 12);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.setDrawColor(0);
  u8g2.drawUTF8(34, 10, "修改密码");
  u8g2.setDrawColor(1);

  u8g2.drawFrame(20, 16, 88, 12);
  u8g2.setFont(u8g2_font_6x10_tf);
  for (int i = 0; i < PWD_LEN; i++) {
    u8g2.setCursor(28 + i * 20, 26);
    if (i < pwdIdx)
      u8g2.print("*");
    else
      u8g2.print("_");
  }
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(16, 44, "输入新密码(4位)");
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(4, 60, "#OK D=Back");
  u8g2.sendBuffer();
}

void oledShowAdminResult(bool ok, const char* msg) {
  u8g2.clearBuffer();
  u8g2.drawFrame(0, 0, 128, 64);
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(ok ? 40 : 24, 30, ok ? "操作成功" : msg);
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
    if (fpBufLen >= 63) { fpBufLen = 0; continue; }
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
  failCount = 0;
  lockoutUntil = 0;
  beep(1, 100);
  digitalWrite(PIN_LED, HIGH);
  lockServo.write(ANGLE_UNLOCK);
  sysState = STATE_UNLOCKED;
  unlockTs = millis();
  if (strcmp(method, "FP") == 0) fpLED_solidGreen();
  pwdInputMode = false;
  oledShowGranted();
  camReportStatus("UNLOCKED");
  camCapture(method, true);
  addOfflineLog(method, true);
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
  addOfflineLog(method, false);
  delay(1500);
  resetPwdInput();
  if (millis() < lockoutUntil) {
    oledShowLockout();
  } else if (pwdInputMode) {
    oledShowPassword();
  } else {
    oledShowMain();
  }
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
    failCount++;
    Serial.printf("[密码] 密码错误 (第%d次)\n", failCount);
    if (failCount >= FAIL_THRESHOLD) {
      lockoutUntil = millis() + LOCKOUT_MS;
      Serial.println("[SECURITY] 密码连续错误5次 已锁定30秒!");
      sendBruteForceAlert();
    }
    onAccessDenied("密码");
  }
}

void handleKeypad() {
  char key = keypad.getKey();

  // 检测长按 * (800ms)
  if (starPressTs > 0 && !adminMode && millis() - starPressTs >= LONG_PRESS_MS) {
    if (sysState == STATE_IDLE && millis() >= lockoutUntil && !pwdInputMode) {
      adminMode = true;
      adminSubMode = 3;  // 需要密码验证
      adminPage = 0;
      adminSelIdx = 0;
      resetPwdInput();
      starPressTs = 0;
      Serial.println("[ADMIN] 长按* 进入管理菜单 - 请验证密码");
      oledShowAdminChangePwd();
      return;
    }
    starPressTs = 0;
  }

  if (!key) return;

  // 按下 * 时记录时间
  if (key == '*') {
    starPressTs = millis();
    if (adminMode && (adminSubMode == 3 || adminSubMode == 6)) {
      handleAdminMenu(key);
    } else if (pwdInputMode) {
      if (pwdIdx > 0) {
        pwdIdx--;
        pwdBuf[pwdIdx] = '\0';
        oledShowPassword();
      }
    }
    return;
  }

  // 按下其他键时取消长按检测
  if (starPressTs > 0) starPressTs = 0;

  // 管理菜单模式
  if (adminMode) {
    handleAdminMenu(key);
    return;
  }

  if (sysState == STATE_UNLOCKED) return;

  // 锁定中检查
  if (millis() < lockoutUntil) {
    oledShowLockout();
    return;
  }

  // 锁定刚到期，重置失败计数
  if (failCount >= FAIL_THRESHOLD) {
    failCount = 0;
  }

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

// ==================== 管理菜单处理 ====================

void handleAdminMenu(char key) {
  // 子模式3 = 密码验证（进入菜单前）
  if (adminSubMode == 3) {
    if (!key) {
      // 密码输入中，检查是否有数字键持续按压的情况
      // 数字输入已经在下面处理
    }
    if (key >= '0' && key <= '9' && pwdIdx < PWD_LEN) {
      pwdBuf[pwdIdx++] = key;
      oledShowAdminChangePwd();
      return;
    }
    if (key == '#' || key == 'C') {
      if (pwdIdx == PWD_LEN) {
        pwdBuf[PWD_LEN] = '\0';
        if (strcmp(pwdBuf, AUTH_PWD) == 0) {
          Serial.println("[ADMIN] 密码验证通过");
          adminSubMode = 0;
          adminPage = 0;
          resetPwdInput();
          oledShowAdminMenu();
        } else {
          Serial.println("[ADMIN] 密码错误");
          adminMode = false;
          adminSubMode = 0;
          oledShowMain();
          beep(3, 100);
        }
      }
      return;
    }
    if (key == 'D' || key == '*') {
      adminMode = false;
      adminSubMode = 0;
      oledShowMain();
      return;
    }
    if (key == 'A') {
      if (pwdIdx > 0) { pwdIdx--; pwdBuf[pwdIdx] = '\0'; oledShowAdminChangePwd(); }
      return;
    }
    if (key == 'B') { resetPwdInput(); oledShowAdminChangePwd(); return; }
    if (key == '*') {
      if (pwdIdx > 0) { pwdIdx--; pwdBuf[pwdIdx] = '\0'; oledShowAdminChangePwd(); }
      return;
    }
    return;
  }

  // 子模式0 = 主菜单
  if (adminSubMode == 0) {
    if (key == '8') {
      if (adminPage < ADMIN_MENU_COUNT - 1) adminPage++;
      oledShowAdminMenu();
    } else if (key == '2') {
      if (adminPage > 0) adminPage--;
      oledShowAdminMenu();
    } else if (key == '#' || key == 'C') {
      switch (adminPage) {
        case 0: adminSubMode = 1; oledShowAdminAddRfid(); break;
        case 1: adminSubMode = 2; adminSelIdx = 0; oledShowAdminDelList(); break;
        case 2: adminSubMode = 4; oledShowAdminAddFp(); break;
        case 3: adminSubMode = 5; adminSelIdx = 0; oledShowAdminDelFpList(); break;
        case 4: adminSubMode = 6; resetPwdInput(); oledShowAdminChangePwd(); break;
        case 5: adminMode = false; oledShowMain(); break;
      }
    } else if (key == 'D') {
      adminMode = false;
      oledShowMain();
    }
    return;
  }

  // 子模式1 = 添加RFID（等待刷卡）
  if (adminSubMode == 1) {
    if (key == 'D') { adminSubMode = 0; oledShowAdminMenu(); return; }
    // 扫描RFID卡片
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      // 检查是否已存在
      bool exists = false;
      for (int c = 0; c < rfidCount; c++) {
        bool match = true;
        for (byte i = 0; i < UID_LEN; i++) {
          if (rfid.uid.uidByte[i] != rfidList[c][i]) { match = false; break; }
        }
        if (match) { exists = true; break; }
      }
      char uidBuf[20];
      sprintf(uidBuf, "%02X:%02X:%02X:%02X",
        rfid.uid.uidByte[0], rfid.uid.uidByte[1],
        rfid.uid.uidByte[2], rfid.uid.uidByte[3]);
      if (!exists && rfidCount < MAX_RFID) {
        for (byte i = 0; i < UID_LEN; i++)
          rfidList[rfidCount][i] = rfid.uid.uidByte[i];
        rfidCount++;
        saveConfigToFlash();
        syncConfigToServer();
        Serial.printf("[ADMIN] 添加RFID: %s (共%d张)\n", uidBuf, rfidCount);
        oledShowAdminAddRfidResult(true, uidBuf);
        beep(1, 100);
        delay(1500);
        oledShowAdminAddRfid();
      } else {
        Serial.printf("[ADMIN] RFID已存在或列表满: %s\n", uidBuf);
        oledShowAdminAddRfidResult(false, uidBuf);
        beep(3, 100);
        delay(1500);
        oledShowAdminAddRfid();
      }
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
    return;
  }

  // 子模式2 = 删除RFID列表
  if (adminSubMode == 2) {
    if (rfidCount == 0) {
      if (key == 'D') { adminSubMode = 0; oledShowAdminMenu(); }
      return;
    }
    if (key == '8') {
      if (adminSelIdx < rfidCount - 1) adminSelIdx++;
      oledShowAdminDelList();
    } else if (key == '2') {
      if (adminSelIdx > 0) adminSelIdx--;
      oledShowAdminDelList();
    } else if (key == '#' || key == 'C') {
      // 删除选中项
      Serial.printf("[ADMIN] 删除RFID[%d]: %02X:%02X:%02X:%02X\n", adminSelIdx,
        rfidList[adminSelIdx][0], rfidList[adminSelIdx][1],
        rfidList[adminSelIdx][2], rfidList[adminSelIdx][3]);
      for (int i = adminSelIdx; i < rfidCount - 1; i++)
        for (byte j = 0; j < UID_LEN; j++)
          rfidList[i][j] = rfidList[i + 1][j];
      rfidCount--;
      if (adminSelIdx >= rfidCount && adminSelIdx > 0) adminSelIdx--;
      saveConfigToFlash();
      syncConfigToServer();
      beep(1, 100);
      oledShowAdminDelList();
    } else if (key == 'D') {
      adminSubMode = 0;
      oledShowAdminMenu();
    }
    return;
  }

  // 子模式4 = 添加指纹
  if (adminSubMode == 4) {
    if (key == 'D') { adminSubMode = 0; oledShowAdminMenu(); return; }
    // 启动指纹录入
    if (fpOnline && fpState == FP_IDLE) {
      Serial.println("[ADMIN] 开始指纹录入...");
      uint8_t p[6] = {0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00};
      fpSend(0x31, p, 6);  // PS_AutoEnroll
      fpState = FP_LISTENING;
      fpListenStart = millis();
    }
    // 等待录入结果
    if (fpState == FP_LISTENING) {
      while (fpSerial.available()) {
        uint8_t b = fpSerial.read();
        if (fpBufLen == 0 && b != 0xEF) continue;
        if (fpBufLen >= 63) { fpBufLen = 0; continue; }
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
                if (fpCount < MAX_FP) {
                  fpList[fpCount++] = id;
                  saveConfigToFlash();
                  syncConfigToServer();
                  Serial.printf("[ADMIN] 指纹录入成功 ID=%d (共%d枚)\n", id, fpCount);
                  oledShowAdminResult(true, "");
                  beep(1, 100);
                } else {
                  Serial.println("[ADMIN] 指纹列表已满");
                  oledShowAdminResult(false, "指纹列表已满");
                }
              } else {
                Serial.printf("[ADMIN] 指纹录入失败 ack=0x%02X\n", ack);
                oledShowAdminResult(false, "录入失败 重试");
              }
              delay(1500);
              oledShowAdminAddFp();
            }
            fpBufLen = 0;
          }
        }
      }
    }
    return;
  }

  // 子模式5 = 删除指纹列表
  if (adminSubMode == 5) {
    if (fpCount == 0) {
      if (key == 'D') { adminSubMode = 0; oledShowAdminMenu(); }
      return;
    }
    if (key == '8') {
      if (adminSelIdx < fpCount - 1) adminSelIdx++;
      oledShowAdminDelFpList();
    } else if (key == '2') {
      if (adminSelIdx > 0) adminSelIdx--;
      oledShowAdminDelFpList();
    } else if (key == '#' || key == 'C') {
      uint16_t id = fpList[adminSelIdx];
      // 发送删除指令 PS_DeleteChar (0x0C)
      uint8_t p[5] = { (uint8_t)(id >> 8), (uint8_t)(id & 0xFF), 0x00, 0x01, 0x00 };
      fpSend(0x0C, p, 5);
      delay(200);
      Serial.printf("[ADMIN] 删除指纹 ID=%d\n", id);
      for (int i = adminSelIdx; i < fpCount - 1; i++)
        fpList[i] = fpList[i + 1];
      fpCount--;
      if (adminSelIdx >= fpCount && adminSelIdx > 0) adminSelIdx--;
      saveConfigToFlash();
      syncConfigToServer();
      beep(1, 100);
      oledShowAdminDelFpList();
    } else if (key == 'D') {
      adminSubMode = 0;
      oledShowAdminMenu();
    }
    return;
  }

  // 子模式6 = 修改密码
  if (adminSubMode == 6) {
    if (key >= '0' && key <= '9' && pwdIdx < PWD_LEN) {
      pwdBuf[pwdIdx++] = key;
      oledShowAdminChangePwd();
      return;
    }
    if (key == '#' || key == 'C') {
      if (pwdIdx == PWD_LEN) {
        pwdBuf[PWD_LEN] = '\0';
        strncpy(AUTH_PWD, pwdBuf, PWD_LEN + 1);
        saveConfigToFlash();
        syncConfigToServer();
        Serial.printf("[ADMIN] 密码修改为: %s\n", AUTH_PWD);
        oledShowAdminResult(true, "");
        beep(1, 100);
        delay(1500);
        adminSubMode = 0;
        oledShowAdminMenu();
      }
      return;
    }
    if (key == 'A') {
      if (pwdIdx > 0) { pwdIdx--; pwdBuf[pwdIdx] = '\0'; oledShowAdminChangePwd(); }
      return;
    }
    if (key == '*') {
      if (pwdIdx > 0) { pwdIdx--; pwdBuf[pwdIdx] = '\0'; oledShowAdminChangePwd(); }
      return;
    }
    if (key == 'B') { resetPwdInput(); oledShowAdminChangePwd(); return; }
    if (key == 'D') { adminSubMode = 0; resetPwdInput(); oledShowAdminMenu(); return; }
    return;
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
  if (isBlacklistedRfid()) {
    Serial.println("  [黑名单] 已拒绝!");
    onAccessDenied("RFID");
  } else if (isAuthCard()) {
    onAccessGranted("RFID");
  } else {
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
    if (fpBufLen >= 63) { fpBufLen = 0; continue; }
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
            if (isBlacklistedFp(id)) {
              Serial.println("  [黑名单] 指纹已拒绝!");
              onAccessDenied("指纹");
            } else {
              onAccessGranted("指纹");
            }
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
  Serial.println("  ║      智 能 门 锁 v9.0       ║");
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

String getDateString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "--/--";
  const char* weekDays[] = {"日", "一", "二", "三", "四", "五", "六"};
  char buf[20];
  sprintf(buf, "%d-%02d-%02d 周%s", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, weekDays[timeinfo.tm_wday]);
  return String(buf);
}

// ==================== 暴力破解告警 ====================

void sendBruteForceAlert() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  client.setTimeout(300);
  if (!client.connect(SERVER_HOST, SERVER_PORT)) return;
  String body = "{\"type\":\"brute_force\",\"count\":" + String(failCount) + "}";
  String req = "POST /api/alert HTTP/1.1\r\nHost: " + String(SERVER_HOST) + "\r\nContent-Type: application/json\r\nContent-Length: " + String(body.length()) + "\r\nConnection: close\r\n\r\n" + body;
  client.print(req);
  delay(100);
  client.stop();
  Serial.println("[SECURITY] 暴力破解告警已发送");
}

// ==================== 黑名单检查 ====================

bool isBlacklistedRfid() {
  if (rfidBlackCount == 0) return false;
  for (int c = 0; c < rfidBlackCount; c++) {
    bool match = true;
    for (byte i = 0; i < UID_LEN; i++) {
      if (rfid.uid.uidByte[i] != rfidBlackList[c][i]) { match = false; break; }
    }
    if (match) return true;
  }
  return false;
}

bool isBlacklistedFp(uint16_t id) {
  for (int i = 0; i < fpBlackCount; i++) {
    if (fpBlackList[i] == id) return true;
  }
  return false;
}

// ==================== 配置同步 ====================

void pollConfig() {
  if (WiFi.status() != WL_CONNECTED) return;
  yield();

  WiFiClient client;
  client.setTimeout(500);
  if (!client.connect(SERVER_HOST, SERVER_PORT)) {
    configSyncFailed = true;
    return;
  }
  configSyncFailed = false;

  String req = "GET /api/config HTTP/1.1\r\nHost: " + String(SERVER_HOST) + "\r\nConnection: close\r\n\r\n";
  client.print(req);

  unsigned long start = millis();
  String payload = "";
  while (millis() - start < 800) {
    if (client.available()) {
      payload = client.readString();
      break;
    }
    delay(1);
  }
  client.stop();

  if (payload.length() == 0) return;

  int jsonStart = payload.indexOf("\r\n\r\n");
  if (jsonStart < 0) return;
  payload = payload.substring(jsonStart + 4);

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
            payload.substring(pStart, pEnd).toCharArray(AUTH_PWD, PWD_LEN + 1);
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
              if (pos >= arr.length()) break;
              int e = arr.indexOf(",", pos);
              if (e < 0) e = arr.length();
              if (e > pos) {
                fpList[fpCount] = arr.substring(pos, e).toInt();
                fpCount++;
              }
              pos = e + 1;
            }
          }
        }

        // 黑名单解析
        rfidBlackCount = 0;
        int rbIdx = payload.indexOf("\"rfid_blacklist\"");
        if (rbIdx > 0) {
          int arrStart = payload.indexOf("[", rbIdx);
          int arrEnd = payload.indexOf("]", arrStart);
          if (arrEnd > arrStart) {
            String arr = payload.substring(arrStart + 1, arrEnd);
            int pos = 0;
            while (pos < arr.length() && rfidBlackCount < MAX_RFID_BL) {
              int s = arr.indexOf("\"", pos);
              if (s < 0) break;
              int e = arr.indexOf("\"", s + 1);
              if (e < 0) break;
              String hex = arr.substring(s + 1, e);
              for (int i = 0; i < UID_LEN; i++) {
                rfidBlackList[rfidBlackCount][i] = strtoul(hex.substring(i * 3, i * 3 + 2).c_str(), NULL, 16);
              }
              rfidBlackCount++;
              pos = e + 1;
            }
          }
        }

        fpBlackCount = 0;
        int fbIdx = payload.indexOf("\"fp_blacklist\"");
        if (fbIdx > 0) {
          int arrStart = payload.indexOf("[", fbIdx);
          int arrEnd = payload.indexOf("]", arrStart);
          if (arrEnd > arrStart) {
            String arr = payload.substring(arrStart + 1, arrEnd);
            int pos = 0;
            while (pos < arr.length() && fpBlackCount < MAX_FP_BL) {
              while (pos < arr.length() && (arr[pos] == ' ' || arr[pos] == ',')) pos++;
              if (pos >= arr.length()) break;
              int e = arr.indexOf(",", pos);
              if (e < 0) e = arr.length();
              if (e > pos) {
                fpBlackList[fpBlackCount] = arr.substring(pos, e).toInt();
                fpBlackCount++;
              }
              pos = e + 1;
            }
          }
        }

        Serial.printf("[SYNC] v%d pwd=%s rfid=%d/%d fp=%d/%d bl_rfid=%d bl_fp=%d\n",
          configVersion, AUTH_PWD, rfidCount, rfidBlackCount, fpCount, fpBlackCount, rfidBlackCount, fpBlackCount);
        saveConfigToFlash();
        uploadOfflineLogs();
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
      WiFiClient c2;
      c2.setTimeout(300);
      if (c2.connect(SERVER_HOST, SERVER_PORT)) {
        c2.print("GET /api/unlock/done HTTP/1.1\r\nHost: " + String(SERVER_HOST) + "\r\nConnection: close\r\n\r\n");
        delay(100);
        c2.stop();
      }
      remoteUnlockPending = false;
    }
  }
}

// ==================== Flash 持久化 ====================

void saveConfigToFlash() {
  prefs.begin("smartlock", false);
  prefs.putBytes("password", AUTH_PWD, PWD_LEN + 1);
  prefs.putUChar("rfidCount", rfidCount);
  prefs.putBytes("rfidList", rfidList, rfidCount * UID_LEN);
  prefs.putUChar("fpCount", fpCount);
  prefs.putBytes("fpList", fpList, fpCount * sizeof(int));
  prefs.putUChar("rfidBlCount", rfidBlackCount);
  prefs.putBytes("rfidBlackList", rfidBlackList, rfidBlackCount * UID_LEN);
  prefs.putUChar("fpBlCount", fpBlackCount);
  prefs.putBytes("fpBlackList", fpBlackList, fpBlackCount * sizeof(int));
  prefs.putUInt("configVer", configVersion);
  prefs.end();
  Serial.println("[FLASH] 配置已保存到Flash");
}

void loadConfigFromFlash() {
  prefs.begin("smartlock", true);
  if (prefs.isKey("password")) {
    prefs.getBytes("password", AUTH_PWD, PWD_LEN + 1);
    rfidCount = prefs.getUChar("rfidCount", 0);
    if (rfidCount > 0) prefs.getBytes("rfidList", rfidList, rfidCount * UID_LEN);
    fpCount = prefs.getUChar("fpCount", 0);
    if (fpCount > 0) prefs.getBytes("fpList", fpList, fpCount * sizeof(int));
    rfidBlackCount = prefs.getUChar("rfidBlCount", 0);
    if (rfidBlackCount > 0) prefs.getBytes("rfidBlackList", rfidBlackList, rfidBlackCount * UID_LEN);
    fpBlackCount = prefs.getUChar("fpBlCount", 0);
    if (fpBlackCount > 0) prefs.getBytes("fpBlackList", fpBlackList, fpBlackCount * sizeof(int));
    configVersion = prefs.getUInt("configVer", 0);
    Serial.printf("[FLASH] 从Flash加载: pwd=%s rfid=%d fp=%d bl_rfid=%d bl_fp=%d v%d\n",
      AUTH_PWD, rfidCount, fpCount, rfidBlackCount, fpBlackCount, configVersion);
  } else {
    Serial.println("[FLASH] Flash无数据，使用默认配置");
  }
  prefs.end();
}

// ==================== 同步配置到服务器 ====================

void syncConfigToServer() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient client;
  client.setTimeout(500);
  if (!client.connect(SERVER_HOST, SERVER_PORT)) return;

  // 构建JSON: 同步白名单和密码
  String body = "{\"door_password\":\"" + String(AUTH_PWD) + "\",\"rfid_whitelist\":[";
  for (int i = 0; i < rfidCount; i++) {
    if (i > 0) body += ",";
    char buf[16];
    sprintf(buf, "\"%02X:%02X:%02X:%02X\"",
      rfidList[i][0], rfidList[i][1], rfidList[i][2], rfidList[i][3]);
    body += buf;
  }
  body += "],\"fp_whitelist\":[";
  for (int i = 0; i < fpCount; i++) {
    if (i > 0) body += ",";
    body += String(fpList[i]);
  }
  body += "]}";

  String req = "POST /api/config HTTP/1.1\r\nHost: " + String(SERVER_HOST) +
               "\r\nContent-Type: application/json\r\nContent-Length: " + String(body.length()) +
               "\r\nConnection: close\r\n\r\n" + body;
  client.print(req);
  delay(200);
  client.stop();
  Serial.println("[SYNC] 配置已同步到服务器");
}

// ==================== 离线日志缓存 ====================

void addOfflineLog(const char* method, bool success) {
  if (offlineLogCount >= MAX_OFFLINE_LOGS) {
    Serial.println("[LOG] 离线日志已满，丢弃最早记录");
    for (int i = 0; i < MAX_OFFLINE_LOGS - 1; i++) {
      offlineLogs[i] = offlineLogs[i + 1];
    }
    offlineLogCount = MAX_OFFLINE_LOGS - 1;
  }
  strncpy(offlineLogs[offlineLogCount].method, method, 7);
  offlineLogs[offlineLogCount].method[7] = '\0';
  strncpy(offlineLogs[offlineLogCount].result, success ? "SUCCESS" : "FAIL", 8);
  offlineLogs[offlineLogCount].timestamp = millis() / 1000 + GMT_OFFSET;
  offlineLogCount++;
  Serial.printf("[LOG] 缓存离线记录: %s %s (共%d条)\n", method, success ? "SUCCESS" : "FAIL", offlineLogCount);
}

void uploadOfflineLogs() {
  if (offlineLogCount == 0 || WiFi.status() != WL_CONNECTED) return;
  Serial.printf("[LOG] 上传%d条离线记录...\n", offlineLogCount);
  int uploaded = 0;
  for (int i = 0; i < offlineLogCount; i++) {
    WiFiClient client;
    client.setTimeout(500);
    if (!client.connect(SERVER_HOST, SERVER_PORT)) break;
    String body = "{\"method\":\"" + String(offlineLogs[i].method) +
                  "\",\"result\":\"" + String(offlineLogs[i].result) +
                  "\",\"timestamp\":" + String(offlineLogs[i].timestamp) + "}";
    String req = "POST /api/log HTTP/1.1\r\nHost: " + String(SERVER_HOST) +
                 "\r\nContent-Type: application/json\r\nContent-Length: " + String(body.length()) +
                 "\r\nConnection: close\r\n\r\n" + body;
    client.print(req);
    delay(100);
    client.stop();
    uploaded++;
  }
  if (uploaded < offlineLogCount) {
    for (int i = 0; i < offlineLogCount - uploaded; i++) {
      offlineLogs[i] = offlineLogs[uploaded + i];
    }
  }
  offlineLogCount -= uploaded;
  Serial.printf("[LOG] 上传完成 %d条 剩余%d条\n", uploaded, offlineLogCount);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  showBanner();
  Serial.println("  初始化硬件:");
  loadConfigFromFlash();
  initOLED();
  initWiFi();
  initRFID();
  initServo();
  initKeypad();
  initFingerprint();
  initCam();

  // 经典蓝牙 SPP，匹配 MIT App Inventor 的 BluetoothClient
  Serial.print("  [..] 经典蓝牙    ");
  SerialBT.setTimeout(200);
  if (SerialBT.begin("Smart_Lock")) {
    Serial.println("OK");
  } else {
    Serial.println("FAIL");
  }

  delay(500);
  oledShowMain();
  beep(1, 200);

  Serial.println();
  Serial.println("  ┌─────────────────────────────┐");
  Serial.println("  │  [RFID]  刷卡开锁            │");
  if (fpOnline)
    Serial.println("  │  [指纹]  放手指开锁          │");
  Serial.println("  │  [密码]  按任意键开始输入     │");
  Serial.println("  │  [蓝牙]  手机App开锁         │");
  Serial.println("  │    * 删除  B 清空  # 确认    │");
  Serial.println("  │    D 返回主界面              │");
  Serial.println("  └─────────────────────────────┘");
  Serial.println();
}

// ==================== 主循环 ====================

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }

  // 管理菜单模式：处理键盘，添加RFID/指纹时持续检测
  if (adminMode) {
    handleKeypad();
    if (adminSubMode == 1) {
      handleAdminMenu(0);  // 持续RFID扫描
    }
    if (adminSubMode == 4 && fpState == FP_LISTENING) {
      handleAdminMenu(0);  // 持续指纹串口轮询
    }
    return;
  }

  if (sysState == STATE_IDLE) {
    handleKeypad();
    pollRFID();
    if (millis() - lastOledRefresh >= 1000) {
      lastOledRefresh = millis();
      if (!pwdInputMode) {
        yield();
        oledShowMain();
      } else if (millis() < lockoutUntil) {
        oledShowLockout();
      } else {
        oledShowPassword();
      }
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

  handleBluetoothApp();
  delay(10);
}


