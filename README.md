# Smart_Lock 智能门锁系统 v9.0

基于 ESP32 的多功能智能门锁，支持 RFID 刷卡、指纹识别、密码键盘、蓝牙手机 四种开锁方式。

## 功能

- 四种开锁: RFID + 指纹 + 密码 + 蓝牙App
- ESP32-CAM 自动拍照记录每次验证（成功/失败均拍照）
- SQLite 数据库: 门禁记录、白名单、黑名单、设备状态、系统配置全部结构化存储
- Web管理后台: 验证记录、白名单管理、黑名单管理、密码修改、远程开锁、微信通知、CSV导出
- 云端配置同步: 密码/白名单/黑名单实时同步到ESP32
- 暴力破解防护: 密码连续错5次锁定30秒 + 微信告警，锁定到期自动恢复
- 黑名单管理: 丢失的卡/指纹可主动封禁，刷卡/放手指直接拒绝并拍照
- OLED升级: 中央大字时间 + 年月日 + WiFi/BT状态 + 锁定倒计时 + 失败次数提示

## 硬件清单

| 模块 | 型号 | 说明 |
|------|------|------|
| 主控 | ESP32-WROOM-32E | Arduino C++ |
| 摄像头 | ESP32-CAM | MicroPython，UART通信 |
| RFID | RC522 | SPI接口读卡 |
| 指纹 | ZW101 半导体指纹模组 | 带RGB三色灯 |
| 舵机 | SG90 | 控制门锁开关 |
| 键盘 | 4x4 矩阵键盘 | 密码输入 |
| OLED | SSD1306 128x64 I2C | U8g2中文显示 |
| 蓝牙 | 经典蓝牙SPP | 手机App开锁 |

## 快速开始

详细使用说明见 [说明书.md](说明书.md)

1. Arduino IDE 烧录 `smart_door_lock/smart_door_lock.ino`（分区选 Huge APP 3MB）
2. Thonny 上传 `模块资料/cam.py` 到 ESP32-CAM
3. 运行 `python server.py`，访问 http://127.0.0.1:8000（admin/123456）
4. 安装 `SmartLockPro_md3_unlock_only.apk` 到安卓手机使用蓝牙开锁

## 技术栈

| 组件 | 技术 |
|------|------|
| 主控固件 | Arduino C++ (ESP32 BLE Arduino) |
| 摄像头固件 | MicroPython (ESP32-CAM) |
| Web 服务器 | Python Flask + SQLite |
| OLED 驱动 | U8g2 (I2C) |
| 微信推送 | Server酱 |
| 蓝牙 App | MIT App Inventor / Android |
