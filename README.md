# Smart_Lock 智能门锁系统

基于 ESP32 的多功能智能门锁，支持 RFID 刷卡、指纹识别、密码键盘、蓝牙手机 四种开锁方式。

## 功能

- 四种开锁: RFID + 指纹 + 密码 + 蓝牙App
- ESP32-CAM 自动拍照记录每次验证
- Web管理后台: 验证记录、白名单管理、黑名单管理、密码修改、远程开锁、微信通知、CSV导出
- 云端配置同步: 密码/白名单/黑名单实时同步到ESP32
- 暴力破解防护: 密码连续错5次锁定30秒 + 微信告警
- 黑名单管理: 丢失的卡/指纹可主动封禁
- OLED升级: 中央大字时间 + 日期 + WiFi/BT状态 + 锁定倒计时

## 快速开始

详细使用说明见 [说明书.md](说明书.md)

1. Arduino IDE 烧录 `smart_door_lock/smart_door_lock.ino`（分区选 Huge APP 3MB）
2. Thonny 上传 `模块资料/cam.py` 到 ESP32-CAM
3. 运行 `python server.py`，访问 http://127.0.0.1:8000
4. 安装 `SmartLockPro_md3_unlock_only.apk` 到安卓手机使用蓝牙开锁
