# Smart_Lock 智能门锁系统

基于 ESP32 的多功能智能门锁，支持 RFID 刷卡、指纹识别、密码键盘三种开锁方式，搭配 ESP32-CAM 自动拍照、Flask 网页管理后台、微信通知推送。

## 功能特性

### 三种开锁方式
- **RFID 刷卡** — RC522 射频模块，刷卡即时响应
- **指纹识别** — ZW101 半导体指纹模组，非阻塞轮询
- **密码输入** — 4x4 矩阵键盘，4位数字密码，按 `#` 确认

### 自动拍照记录
- 每次验证（成功/失败）自动触发 ESP32-CAM 拍照
- 照片上传到本地 Flask 服务器，Web 界面可浏览

### Web 管理后台
- **验证记录** — 查看所有开锁/失败记录，含照片、时间、方式
- **白名单管理** — 添加/删除 RFID 卡号和指纹 ID
- **通知设置** — Server酱 微信推送，可按事件类型开关（成功/失败/远程）
- **密码修改** — 在线修改开门密码，自动同步到 ESP32
- **远程开锁** — 网页一键远程开门
- **CSV 导出** — 导出验证记录为 CSV 文件

### 微信通知
- 接入 Server酱（sct.ftqq.com），开锁/失败/远程开锁事件实时推送到微信
- 支持独立控制每种事件的通知开关

### 云端配置同步
- ESP32 每 5 秒从服务器拉取最新配置（密码、白名单）
- WiFi 断线自动重连，支持网络环境变化后 IP 自动适应

### OLED 显示
- 主界面：中文标题 + 实时时钟 + 锁状态图标
- 密码界面：虚拟键盘布局 + 输入进度
- 开锁倒计时显示

## 硬件清单

| 模块 | 型号 | 说明 |
|------|------|------|
| 主控 | ESP32-WROOM-32E | 主程序运行 |
| 摄像头 | ESP32-CAM | MicroPython，UART 通信 |
| RFID | RC522 | SPI 接口 |
| 指纹 | ZW101 | 半导体指纹，带 RGB 灯 |
| 舵机 | SG90 | 控制门锁开关 |
| 键盘 | 4x4 矩阵键盘 | 密码输入 |
| OLED | SSD1306 128x64 I2C | U8g2 中文显示 |

## 接线图

| ESP32 引脚 | 连接模块 | 说明 |
|-----------|---------|------|
| GPIO 2 | LED | 状态指示灯 |
| GPIO 4 | 蜂鸣器 | 声音反馈 |
| GPIO 5 | RC522 SS | SPI 片选 |
| GPIO 12 | ESP32-CAM TX | CAM 发送 |
| GPIO 13 | 键盘 C3 | |
| GPIO 14 | 键盘 C2 | |
| GPIO 15 | SG90 舵机 | PWM 信号 |
| GPIO 16 | ZW101 RX | 指纹接收 |
| GPIO 17 | ZW101 TX | 指纹发送 |
| GPIO 18 | RC522 SCK | SPI 时钟 |
| GPIO 19 | RC522 MISO | SPI 主入 |
| GPIO 21 | OLED SDA | I2C 数据 |
| GPIO 22 | OLED SCL | I2C 时钟 |
| GPIO 23 | RC522 MOSI | SPI 主出 |
| GPIO 25-27, 32-33 | 键盘行/列 | 4x4 矩阵 |
| GPIO 35 | ESP32-CAM RX | CAM 接收 |

## 文件结构

```
Smart_Lock/
├── server.py                    # Flask 管理服务器
├── smart_door_lock/
│   ├── smart_door_lock.ino      # 主 ESP32 程序 (Arduino)
│   └── README.md                # 详细开发文档
├── 模块资料/
│   └── cam.py                   # ESP32-CAM 程序 (MicroPython)
└── uploads/                     # 照片存储目录
```

## 快速开始

### 1. 烧录主 ESP32
Arduino IDE 打开 `smart_door_lock/smart_door_lock.ino`，选择 ESP32 Dev Module，上传。

### 2. 上传 ESP32-CAM
用 Thonny 将 `模块资料/cam.py` 上传到 ESP32-CAM，重启运行。

### 3. 启动服务器
```bash
pip install flask werkzeug
python server.py
```
访问 http://127.0.0.1:8000，登录账号 `admin`，密码 `123456`。

### 4. 配置
- 打开 **通知设置** 填入 Server酱 SendKey 实现微信推送
- 打开 **白名单管理** 添加你的 RFID 卡号和指纹 ID
- 打开 **密码设置** 修改开门密码

## 技术栈

| 组件 | 技术 |
|------|------|
| 主控固件 | Arduino C++ (ESP32) |
| 摄像头固件 | MicroPython (ESP32-CAM) |
| Web 服务器 | Python Flask + SQLite |
| OLED 驱动 | U8g2 (I2C) |
| 微信推送 | Server酱 |

## 注意事项

- ESP32-CAM 需要独立 5V 供电，不能从主 ESP32 取电
- WiFi 名称、密码、服务器 IP 需在代码中配置（与局域网匹配）
- 默认开门密码为 `1234`，建议修改
- 网络环境变化（换路由器/重启）后 IP 可能改变，需同步更新代码中的服务器地址
