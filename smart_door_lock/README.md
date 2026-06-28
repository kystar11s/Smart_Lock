# 智能门锁 开发文档

## 项目概述

基于 ESP32 的智能门锁系统，支持 RFID 刷卡 + 指纹识别 + 密码键盘 三种开锁方式，OLED 显示状态，舵机控制门锁开关，蜂鸣器和 LED 提供声光反馈。集成 ESP32-CAM 模块，每次验证尝试时自动拍照并上传到本地服务器。

## 硬件清单

| 模块 | 型号 | 数量 | 说明 |
|------|------|------|------|
| 主控 | ESP32-WROOM-32E | 1 | 开发板 |
| 摄像头 | ESP32-CAM (PSRAM版) | 1 | 拍照模块 |
| RFID | RC522 | 1 | 刷卡模块 |
| 指纹 | ZW101 半导体指纹模组 | 1 | 带 RGB 三色灯 |
| 舵机 | SG90 | 1 | 控制门锁开关 |
| 键盘 | 4x4 矩阵键盘 | 1 | 密码输入 |
| OLED | 0.96寸 SSD1306 I2C | 1 | 状态显示 |
| LED | 绿色 LED | 1 | 状态指示灯 |
| 蜂鸣器 | 有源蜂鸣器 | 1 | 声音反馈 |
| 电阻 | 220Ω | 1 | LED 限流 |

## 硬件接线

### RFID RC522

| RC522 引脚 | ESP32 引脚 | 说明 |
|------------|-----------|------|
| SDA (SS) | GPIO 5 | SPI 片选（模块上标SDA） |
| SCK | GPIO 18 | SPI 时钟 |
| MOSI | GPIO 23 | SPI 主出从入 |
| MISO | GPIO 19 | SPI 主入从出 |
| RST | 不接（-1） | 模块内部上电复位，无需连接 |
| 3.3V | 3.3V | 供电 |
| GND | GND | 共地 |

### 舵机 SG90

| 舵机线颜色 | 功能 | ESP32 引脚 |
|-----------|------|-----------|
| 红 | VCC | 5V（或外部 5V 电源） |
| 棕/黑 | GND | GND |
| 橙/黄 | 信号 | GPIO 15 |

### ZW101 指纹模组

| ZW101 引脚 | ESP32 引脚 | 说明 |
|-----------|-----------|------|
| TX | GPIO 16 | 模组发 → ESP32 收 (RX2) |
| RX | GPIO 17 | ESP32 发 (TX2) → 模组收 |
| VCC | 3.3V | 供电 |
| V_SENSOR | 3.3V | 常供电，手指唤醒用 |
| GND | GND | 共地 |

### OLED 显示屏 (I2C)

| OLED 引脚 | ESP32 引脚 | 说明 |
|----------|-----------|------|
| SDA | GPIO 21 | I2C 数据 |
| SCL | GPIO 22 | I2C 时钟 |
| VCC | 3.3V | 供电 |
| GND | GND | 共地 |

### 4x4 矩阵键盘

| 键盘引脚 | ESP32 引脚 | 说明 |
|---------|-----------|------|
| R1 (行1) | GPIO 26 | 行扫描 |
| R2 (行2) | GPIO 25 | 行扫描 |
| R3 (行3) | GPIO 33 | 行扫描 |
| R4 (行4) | GPIO 32 | 行扫描 |
| C1 (列1) | GPIO 27 | 列检测 |
| C2 (列2) | GPIO 14 | 列检测 |
| C3 (列3) | GPIO 13 | 列检测 |
| C4 (列4) | GPIO 3 | 列检测（1根线跨到右侧） |

### LED 指示灯

| LED 引脚 | ESP32 引脚 |
|---------|-----------|
| 正极（长脚） | GPIO 2（经 220Ω 电阻） |
| 负极（短脚） | GND |

### 蜂鸣器

| 蜂鸣器引脚 | ESP32 引脚 |
|-----------|-----------|
| 正极 | GPIO 4 |
| 负极 | GND |

### ESP32-CAM (UART 通信)

| ESP32-CAM 引脚 | 主ESP32 引脚 | 说明 |
|----------------|-------------|------|
| TX (GPIO 1) | GPIO 35 | CAM 发 → 主ESP32 收 |
| RX (GPIO 3) | GPIO 12 | 主ESP32 发 → CAM 收 |
| 5V | 5V | 供电（建议外部5V电源） |
| GND | GND | 共地 |

**注意**: ESP32-CAM 需要独立的 5V 电源供电，不能直接从主ESP32取电（电流不足）。

### 引脚汇总

| ESP32 引脚 | 用途 | 所在侧 |
|-----------|------|--------|
| GPIO 2 | LED 指示灯 | 右侧 |
| GPIO 3 | 键盘 列4 | 右侧 |
| GPIO 4 | 蜂鸣器 | 右侧 |
| GPIO 5 | RFID SS | 右侧 |
| GPIO 12 | ESP32-CAM TX | 右侧 |
| GPIO 13 | 键盘 列3 | 左侧 |
| GPIO 14 | 键盘 列2 | 左侧 |
| GPIO 15 | 舵机信号 | 右侧 |
| GPIO 16 | 指纹 RX2 | 右侧 |
| GPIO 17 | 指纹 TX2 | 右侧 |
| GPIO 18 | RFID SCK | 右侧 |
| GPIO 19 | RFID MISO | 右侧 |
| GPIO 21 | OLED SDA | 右侧 |
| GPIO 22 | OLED SCL | 右侧 |
| GPIO 23 | RFID MOSI | 右侧 |
| GPIO 25 | 键盘 行2 | 左侧 |
| GPIO 26 | 键盘 行1 | 左侧 |
| GPIO 27 | 键盘 列1 | 左侧 |
| GPIO 32 | 键盘 行4 | 左侧 |
| GPIO 33 | 键盘 行3 | 左侧 |
| GPIO 35 | ESP32-CAM RX | 右侧 |
| 3.3V | RFID / 指纹 / OLED | |
| 5V | 舵机 / ESP32-CAM | |
| GND | 全部共地 | |

## 软件架构

### 开发环境

- **主ESP32**: Arduino IDE 2.x, esp32 3.3.10
- **ESP32-CAM**: Thonny + MicroPython 固件
- **本地服务器**: Python 3 + Flask
- **波特率**: 115200（调试串口）/ 57600（指纹模组）/ 115200（ESP32-CAM UART）

### 依赖库

| 库 | 版本 | 用途 |
|----|------|------|
| SPI | 内置 | RFID 通信 |
| MFRC522 | 1.4.12 | RFID 读卡 |
| Servo | 1.3.0 | 舵机控制（需修改适配 ESP32 3.3.10） |
| HardwareSerial | 内置 | 指纹模组串口通信 |
| Wire | 内置 | I2C 通信 |
| Adafruit_GFX | latest | OLED 图形库 |
| Adafruit_SSD1306 | latest | OLED 驱动 |
| Keypad | latest | 矩阵键盘 |

### Servo 库适配

ESP32 board package 3.3.10 的 LEDC API 有变更，需修改 Servo 库：

**文件**: `libraries/Servo/src/esp32/ServoTimers.h`
```cpp
#define LEDC_MAX_BIT_WIDTH  SOC_LEDC_TIMER_BIT_WIDTH
```

**文件**: `libraries/Servo/src/esp32/Servo.cpp`
```cpp
// 构造函数
ledcAttach(pin, (1000000 / REFRESH_INTERVAL), LEDC_MAX_BIT_WIDTH);
// 析构函数
ledcDetach(pin);
// 写入
ledcWrite(pin, value);
// 读取
ledcRead(pin);
```

### 状态机架构

```
┌──────────┐
│   IDLE   │◄──────────────────┐
└────┬─────┘                   │
     │ RFID/指纹/密码验证通过    │ 5秒定时到
     ▼                         │
┌──────────┐                   │
│ UNLOCKED │───────────────────┘
└──────────┘
```

- **IDLE**: 等待刷卡、指纹或密码，三种方式互不阻塞
- **UNLOCKED**: 门已开，等待定时自动关锁，忽略重复验证

### 开锁方式

| 方式 | 触发 | 说明 |
|------|------|------|
| RFID 刷卡 | 刷有效卡片 | 即时响应 |
| 指纹识别 | 放手指到模组 | 每 3 秒轮询，非阻塞 |
| 密码输入 | 按 `*` 进入，输入 6 位，按 `#` 确认 | 键盘操作 |

### OLED 显示内容

| 状态 | 显示 |
|------|------|
| 锁定 | `LOCKED` + 三种开锁方式提示 |
| 已开锁 | `UNLOCKED` + 倒计时 |
| 密码输入 | 显示 `*` 和 `_` 占位符 |
| 验证通过 | `ACCESS GRANTED` + 方式 |
| 验证失败 | `ACCESS DENIED` + 重试提示 |

### 指纹通信协议

ZW101 使用 UART TTL 3.3V 通信，波特率 57600，8N1。

**包格式**:
```
[包头 2B] [设备地址 4B] [包标识 1B] [包长度 2B] [命令/参数] [校验和 2B]
  EF 01     FF FF FF FF     01          LL          CC ...      SS
```

**主要指令**:
| 指令 | 代码 | 功能 |
|------|------|------|
| PS_AutoIdentify | 0x32 | 自动识别指纹（1:N） |
| PS_HandShake | 0x35 | 握手检测 |
| PS_ControlBLN | 0x3C | RGB 灯控制 |

**校验和**: 从包标识到校验和之前所有字节之和。

### 串口输出示例

```
  ╔══════════════════════════════╗
  ║      智 能 门 锁 v4.2       ║
  ║  RFID+指纹+键盘+OLED+舵机   ║
  ╚══════════════════════════════╝

  初始化硬件:
  [..] OLED 128x64  OK
  [..] RFID RC522    OK
  [..] 舵机 SG90    OK
  [..] 4x4 键盘     OK
  [..] ZW101 指纹   OK

[RFID] 卡片: 0C:D5:D4:E7
  >> RFID 验证通过! 开锁!
  >> 自动关锁

[指纹] 识别成功 ID=1 得分=6
  >> 指纹 验证通过! 开锁!
  >> 自动关锁

[密码] 进入密码输入模式
[密码] 输入: ***
[密码] 输入: ******
[密码] 验证通过
  >> 密码 验证通过! 开锁!
  >> 自动关锁
```

## 已知问题与注意事项

1. **ESP32 GPIO 34-39**: 仅支持输入，不能用于键盘行引脚（需要输出）
2. **指纹模组供电**: ZW101 支持 3.3V 供电（VCC 和 V_SENSOR 均接 3.3V）
3. **Servo 库需修改**: ESP32 board package 3.3.10 的 LEDC API 有变更
4. **GPIO 15 舵机**: 启动时会输出 PWM 信号，舵机可能轻微抖动，初始化后正常
5. **GPIO 3 键盘列4**: 与 UART0 RX 共用，不影响功能（仅用 Serial.print 发送调试信息）
6. **GPIO 4 蜂鸣器**: 空闲引脚，无启动冲突，可直接烧录
7. **GPIO 2 LED**: 与板载 LED 共用，内外 LED 同时亮
8. **RFID RST**: 不需要连接，模块内部有上电复位电路，代码中设为 -1
9. **密码默认值**: `1234`，可在代码中修改 `AUTH_PWD`
10. **自动关锁时间**: 默认 5 秒，可通过 `AUTO_LOCK_MS` 修改
11. **ESP32-CAM 供电**: 必须使用独立 5V 电源，不能从主ESP32取电
12. **GPIO 12**: Strapping 引脚，启动时需为低电平，启动后可正常用于 UART
13. **GPIO 35**: 纯输入引脚，非常适合 UART RX

## ESP32-CAM 与门禁记录

### 系统功能

- 每次验证尝试（RFID/指纹/密码）自动拍照
- 记录验证方式、结果（成功/失败）、时间
- Web 界面显示照片和验证信息
- 支持按时间排序查看历史记录

### UART 通信协议

```
主ESP32 → CAM: "CAPTURE|方式|结果"
  方式: RFID / 指纹 / 密码
  结果: SUCCESS / FAIL

CAM → 主ESP32: "OK\n" 或 "FAIL\n"
```

### 烧录 MicroPython 固件

1. 下载 ESP32-CAM MicroPython 固件（需支持 PSRAM）
2. 使用 esptool 烧录：
   ```bash
   esptool.py --chip esp32 --port COMx write_flash -z 0x1000 firmware.bin
   ```

### 上传 cam.py

使用 Thonny IDE 连接 ESP32-CAM，将 `cam.py` 上传到设备根目录。

### 启动本地服务器

在 PC 上运行：
```bash
cd Smart_Lock
python server.py
```

服务器启动后访问 http://localhost:8000 查看照片管理界面。

### 工作流程

1. 主ESP32 检测到验证尝试（RFID/指纹/密码）
2. 通过 UART 发送 `CAPTURE` 命令到 ESP32-CAM
3. ESP32-CAM 拍照并上传到 Flask 服务器
4. 照片保存在 `uploads/` 目录，可在 Web 界面查看

## 文件结构

```
Smart_Lock/
├── smart_door_lock/
│   ├── smart_door_lock.ino    # 主程序 (v7.0)
│   └── README.md              # 本文档
├── cam.py                     # ESP32-CAM 拍照模块 (MicroPython)
├── server.py                  # 门禁管理服务器 (Flask)
├── uploads/                   # 照片存储目录
├── access_logs.json           # 门禁验证日志
├── servo_test/
│   └── servo_test.ino         # 舵机单独测试
├── uart_test/
│   └── uart_test.ino          # 串口通信测试
├── 指纹模组产品用户手册_V1.5.1.pdf
├── ZW101半导体指纹处理模块规格书V1.2.pdf
└── README.md
```

## 版本历史

| 版本 | 日期 | 内容 |
|------|------|------|
| v1.0 | - | 基础 RFID + 舵机 |
| v2.0 | - | 加入指纹模组，非阻塞状态机 |
| v3.0 | - | 优化架构，美化串口输出，修复指纹通信 |
| v4.0 | - | 加入 4x4 矩阵键盘 + OLED 显示，密码开锁 |
| v4.2 | - | 优化引脚分配，按模块分组面包板走线，消除绕线 |
| v6.7 | - | U8g2 中文显示，OLED 界面优化 |
| v7.0 | - | 集成 ESP32-CAM，验证时自动拍照上传 |
| v7.1 | - | 门禁记录功能：记录验证方式、结果、时间 |
