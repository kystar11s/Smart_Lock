# cam.py - ESP32-CAM 拍照模块 (PSRAM版)
import machine
import time
import network
import socket
from machine import Pin

# 配置
WIFI_SSID = "ky"
WIFI_PASSWORD = "1472583692"
UART_BAUDRATE = 115200
UPLOAD_HOST = "10.132.190.95"
UPLOAD_PORT = 8000

# 状态
wifi_connected = False
heartbeat_counter = 0

def check_psram():
    try:
        import esp
        psram_size = esp.PSRAM_SIZE
        print("[INFO] PSRAM:", psram_size)
        return psram_size > 0
    except:
        print("[INFO] 无PSRAM")
        return False

def init_camera():
    print("[CAM] 初始化...")
    try:
        import camera
    except ImportError:
        print("[ERR] camera模块不可用")
        return False

    has_psram = check_psram()

    try:
        camera.deinit()
        time.sleep(0.1)
    except:
        pass

    try:
        if has_psram:
            camera.init(0, format=camera.JPEG, fb_location=camera.PSRAM)
        else:
            camera.init(0, format=camera.JPEG)
        print("[CAM] OK")
    except Exception as e:
        print("[ERR] camera初始化失败:", e)
        return False

    try:
        camera.flip(0)
        camera.mirror(1)
        camera.framesize(camera.FRAME_QVGA)
        camera.quality(10)
    except:
        pass

    return True

def connect_wifi():
    global wifi_connected
    print("[WiFi] 连接中...")

    for attempt in range(3):
        try:
            wlan = network.WLAN(network.STA_IF)
            wlan.active(False)
            time.sleep(0.3)
            wlan.active(True)

            if wlan.isconnected():
                wlan.disconnect()
                time.sleep(0.3)

            wlan.connect(WIFI_SSID, WIFI_PASSWORD)

            for i in range(20):
                if wlan.isconnected():
                    ip = wlan.ifconfig()[0]
                    print("[WiFi] OK IP:", ip)
                    wifi_connected = True
                    return True
                time.sleep(1)

            print("[WiFi] 超时")
        except OSError as e:
            print("[WiFi] 错误:", e)
            time.sleep(1)

    print("[WiFi] 失败")
    wifi_connected = False
    return False

def check_wifi():
    global wifi_connected
    try:
        wlan = network.WLAN(network.STA_IF)
        if wlan.isconnected():
            wifi_connected = True
            return True
        else:
            wifi_connected = False
            return False
    except:
        wifi_connected = False
        return False

def http_post(host, port, path, data, headers):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(8)
        s.connect((host, port))

        request_line = "POST {} HTTP/1.1\r\n".format(path)
        header_lines = ""
        for key, value in headers.items():
            header_lines += "{}: {}\r\n".format(key, value)

        s.send(request_line.encode())
        s.send(header_lines.encode())
        s.send(b"\r\n")
        s.send(data)

        response = b""
        while True:
            try:
                chunk = s.recv(1024)
                if not chunk:
                    break
                response += chunk
            except:
                break

        s.close()

        if response:
            status_line = response.decode().split("\r\n")[0]
            status_code = int(status_line.split()[1])
            return status_code
        return 0
    except Exception as e:
        print("[HTTP] 错误:", e)
        return -1

def capture_and_upload(method="UNKNOWN", result="UNKNOWN"):
    try:
        import camera
    except:
        print("[CAM] camera不可用")
        return False

    print("[CAM] 拍照...")
    try:
        buf = camera.capture()
    except Exception as e:
        print("[CAM] 拍照失败:", e)
        return False

    if not buf:
        print("[CAM] 拍照失败")
        return False

    print("[CAM] 照片:", len(buf), "bytes")

    boundary = "----Boundary12345"
    body = b"--" + boundary.encode() + b"\r\n"
    body += b'Content-Disposition: form-data; name="file"; filename="photo.jpg"\r\n'
    body += b"Content-Type: image/jpeg\r\n\r\n"
    body += buf
    body += b"\r\n--" + boundary.encode() + b"\r\n"
    body += b'Content-Disposition: form-data; name="method"\r\n\r\n'
    body += method.encode() + b"\r\n"
    body += b"--" + boundary.encode() + b"\r\n"
    body += b'Content-Disposition: form-data; name="result"\r\n\r\n'
    body += result.encode() + b"\r\n"
    body += b"--" + boundary.encode() + b"--\r\n"

    headers = {
        "Host": "{}:{}".format(UPLOAD_HOST, UPLOAD_PORT),
        "Content-Type": "multipart/form-data; boundary=" + boundary,
        "Content-Length": len(body),
        "Connection": "close"
    }

    print("[UPLOAD] 上传中...")
    status = http_post(UPLOAD_HOST, UPLOAD_PORT, "/upload", body, headers)
    print("[UPLOAD] 状态:", status)
    return status == 200

def send_heartbeat():
    try:
        import json
        data = json.dumps({"cam_online": 1}).encode()
        headers = {
            "Host": "{}:{}".format(UPLOAD_HOST, UPLOAD_PORT),
            "Content-Type": "application/json",
            "Content-Length": len(data),
            "Connection": "close"
        }
        status = http_post(UPLOAD_HOST, UPLOAD_PORT, "/api/status", data, headers)
        print("[HB] 状态:", status)
        return status == 200
    except Exception as e:
        print("[HB] 失败:", e)
        return False

def main():
    global heartbeat_counter

    print("=" * 40)
    print("  ESP32-CAM 拍照模块")
    print("=" * 40)

    # 初始化摄像头
    camera_ok = init_camera()

    # 连接WiFi
    wifi_ok = connect_wifi()

    if wifi_ok:
        send_heartbeat()

    # 初始化UART
    print("[UART] 初始化...")
    try:
        uart = machine.UART(1, baudrate=UART_BAUDRATE, tx=1, rx=3)
        print("[UART] OK")
    except Exception as e:
        print("[ERR] UART失败:", e)
        return

    print("[系统] 就绪，等待指令...")

    last_heartbeat = time.ticks_ms()

    while True:
        # 定时心跳 (每30秒)
        now = time.ticks_ms()
        if time.ticks_diff(now, last_heartbeat) > 30000:
            last_heartbeat = now
            if not check_wifi():
                print("[WiFi] 断开，重连...")
                connect_wifi()
            if wifi_connected:
                send_heartbeat()

        # 处理UART指令
        if uart.any():
            try:
                cmd = uart.readline().decode().strip()
            except:
                time.sleep(0.1)
                continue

            if not cmd:
                time.sleep(0.1)
                continue

            print("[CMD]", cmd)

            if cmd.startswith("CAPTURE"):
                parts = cmd.split("|")
                method = parts[1] if len(parts) > 1 else "UNKNOWN"
                result = parts[2] if len(parts) > 2 else "UNKNOWN"
                if capture_and_upload(method, result):
                    uart.write("OK\n")
                    print("[UART] -> OK")
                else:
                    uart.write("FAIL\n")
                    print("[UART] -> FAIL")
            elif cmd.startswith("STATUS"):
                parts = cmd.split("|")
                lock_state = parts[1] if len(parts) > 1 else "UNKNOWN"
                try:
                    import json
                    data = json.dumps({"lock_state": lock_state}).encode()
                    headers = {
                        "Host": "{}:{}".format(UPLOAD_HOST, UPLOAD_PORT),
                        "Content-Type": "application/json",
                        "Content-Length": len(data),
                        "Connection": "close"
                    }
                    status = http_post(UPLOAD_HOST, UPLOAD_PORT, "/api/status", data, headers)
                    print("[STATUS] 门锁:", lock_state, "| 服务器:", status)
                except Exception as e:
                    print("[STATUS] 失败:", e)
                uart.write("OK\n")
            elif cmd == "PING":
                uart.write("PONG\n")
                print("[UART] -> PONG")
            else:
                uart.write("UNKNOWN\n")
                print("[UART] -> UNKNOWN")

        time.sleep(0.1)

if __name__ == "__main__":
    main()
