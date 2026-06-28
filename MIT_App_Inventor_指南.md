# MIT App Inventor 蓝牙开锁 App 制作指南

## 前置准备
1. 打开 https://ai2.appinventor.mit.edu/ 登录 Google 账号
2. 点击「新建项目」，项目名 `SmartLock`

## 第一步：添加蓝牙组件

在左侧「组件面板」中依次添加：

| 组件 | 分类 | 说明 |
|------|------|------|
| BluetoothClient1 | 传感器 | 蓝牙客户端 |
| Clock1 | 传感器 | 定时器，用于检测连接状态 |

## 第二步：设计界面

### 界面布局

```
┌─────────────────────────┐
│     智能门锁            │  ← 标题 Label
├─────────────────────────┤
│                         │
│  状态：未连接           │  ← Label 连接状态
│                         │
│  [扫描设备]             │  ← 按钮 扫描
│  [断开连接]             │  ← 按钮 断开
│                         │
│  设备列表:              │  ← 列表框 显示扫描到的设备
│                         │
├─────────────────────────┤
│  密码: [____]           │  ← 文本输入框（密码）
│                         │
│  [开  锁]               │  ← 按钮 开锁
│                         │
├─────────────────────────┤
│  结果：等待操作...      │  ← Label 开锁结果
│                         │
│  ─── 开锁记录 ───       │
│  13:30:15 蓝牙 成功     │  ← 标签 显示历史
│  13:28:02 蓝牙 失败     │
└─────────────────────────┘
```

### 组件清单

| 组件名 | 类型 | 属性设置 |
|--------|------|---------|
| TitleLabel | Label | Text="智能门锁", FontSize=24, 文字居中 |
| StatusLabel | Label | Text="状态: 未连接", FontSize=16 |
| ScanButton | Button | Text="扫描设备" |
| DisconnectButton | Button | Text="断开连接" |
| DeviceListPicker | ListPicker | Visible=false (扫描后设为true) |
| PasswordBox | TextBox | NumbersOnly=true, Hint="输入4位密码", MaxLength=4 |
| UnlockButton | Button | Text="开锁", FontSize=18 |
| ResultLabel | Label | Text="结果：等待操作...", FontSize=14 |
| HistoryLabel | Label | Text="开锁记录:", FontSize=12, MultiLine=true |
| Clock1 | Clock | TimerInterval=1000, TimerEnabled=false |
| BluetoothClient1 | BluetoothClient | — |

## 第三步：编写逻辑（块编辑器）

### 1. 扫描按钮点击
```
当 ScanButton.Click
  → 调用 BluetoothClient1.Disconnect
  → 设置 DeviceListPicker.Elements = BluetoothClient1.AddressesAndNames
  → 设置 DeviceListPicker.Visible = true
```

### 2. 设备列表选择
```
当 DeviceListPicker.AfterPicking
  → 调用 BluetoothClient1.Connect(DeviceListPicker.Selection)
  → 设置 StatusLabel.Text = "状态: 连接中..."
  → 设置 Clock1.TimerEnabled = true
```

### 3. 定时器检查连接
```
当 Clock1.Timer
  如果 BluetoothClient1.IsConnected 则
    设置 StatusLabel.Text = "状态: 已连接"
    设置 StatusLabel.TextColor = green
  否则
    设置 StatusLabel.Text = "状态: 未连接"
    设置 StatusLabel.TextColor = red
    设置 Clock1.TimerEnabled = false
```

### 4. 开锁按钮点击
```
当 UnlockButton.Click
  如果 BluetoothClient1.IsConnected 则
    调用 BluetoothClient1.SendText(PasswordBox.Text)
    设置 ResultLabel.Text = "正在验证..."
    设置 ResultLabel.TextColor = gray
  否则
    设置 ResultLabel.Text = "请先连接蓝牙设备"
    设置 ResultLabel.TextColor = red
```

### 5. 接收ESP32返回结果（关键！）
App Inventor 的 BluetoothClient 没有直接的「收到数据」事件。
需要用 **Clock 轮询** 方式读取：

```
当 Clock1.Timer
  如果 BluetoothClient1.IsConnected 则
    如果 BluetoothClient1.BytesAvailableToReceive > 0 则
      设置 result = 调用 BluetoothClient1.ReceiveText(BluetoothClient1.BytesAvailableToReceive)
      如果 result = "OK" 则
        设置 ResultLabel.Text = "✅ 开锁成功!"
        设置 ResultLabel.TextColor = green
        添加记录："时间 蓝牙 成功"
      否则如果 result = "FAIL" 则
        设置 ResultLabel.Text = "❌ 密码错误"
        设置 ResultLabel.TextColor = red
        添加记录："时间 蓝牙 失败"
```

**注意：** ESP32 的 BLE Characteristic notify 数据需要通过轮询 `BytesAvailableToReceive` 来读取。

### 6. 断开按钮
```
当 DisconnectButton.Click
  调用 BluetoothClient1.Disconnect
  设置 StatusLabel.Text = "状态: 未连接"
  设置 Clock1.TimerEnabled = false
```

## 第四步：生成 APK

1. 点击菜单「打包」→「App (提供二维码)」
2. 等待编译完成，手机扫描二维码安装
3. 或选择「App (保存到电脑)」下载 APK

## 测试步骤

1. ESP32 上电，串口显示 `[BLE]` 相关日志
2. 手机打开 App → 点击「扫描设备」
3. 在列表中选择 `Smart_Lock`
4. 状态变为「已连接」
5. 输入密码（默认1234）→ 点击「开锁」
6. 观察 ESP32 串口输出和舵机动作
7. App 显示「开锁成功」或「密码错误」

## 常见问题

**Q: 扫描不到设备？**
- 确认 ESP32 已上电且 BLE 已初始化
- 手机蓝牙已开启
- 尝试关闭再重新打开蓝牙

**Q: 连接后收不到结果？**
- 检查 ESP32 串口是否有 `[BLE] 密码正确` 输出
- 确认 App 的 Clock 定时器已启用
- BLE notify 需要 Client 端先订阅（App Inventor 自动处理）

**Q: 编译报错？**
- 确保使用的是 ESP32 Dev Module 板型
- Arduino IDE 安装了 ESP32 BLE Arduino 库
