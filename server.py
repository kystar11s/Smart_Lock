from flask import Flask, request, jsonify, render_template_string, send_from_directory, session, redirect, url_for
import os
import time
import sqlite3
import secrets
from werkzeug.security import generate_password_hash, check_password_hash

app = Flask(__name__)
app.secret_key = secrets.token_hex(16)

UPLOAD_FOLDER = 'uploads'
DB_FILE = 'access.db'
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

ADMIN_USER = 'admin'
ADMIN_PASS_HASH = generate_password_hash('123456')

def get_db():
    conn = sqlite3.connect(DB_FILE)
    conn.row_factory = sqlite3.Row
    return conn

def init_db():
    conn = get_db()
    c = conn.cursor()
    c.execute('''CREATE TABLE IF NOT EXISTS logs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        filename TEXT NOT NULL,
        timestamp INTEGER NOT NULL,
        method TEXT NOT NULL,
        result TEXT NOT NULL,
        size INTEGER DEFAULT 0
    )''')
    c.execute('''CREATE TABLE IF NOT EXISTS whitelist (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        type TEXT NOT NULL,
        value TEXT NOT NULL,
        name TEXT DEFAULT '',
        created_at INTEGER NOT NULL
    )''')
    c.execute('''CREATE TABLE IF NOT EXISTS device_status (
        id INTEGER PRIMARY KEY CHECK (id = 1),
        lock_state TEXT DEFAULT 'LOCKED',
        cam_online INTEGER DEFAULT 0,
        last_update INTEGER DEFAULT 0
    )''')
    c.execute('''CREATE TABLE IF NOT EXISTS settings (
        key TEXT PRIMARY KEY,
        value TEXT NOT NULL
    )''')
    c.execute('''CREATE TABLE IF NOT EXISTS blacklist (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        type TEXT NOT NULL,
        value TEXT NOT NULL,
        name TEXT DEFAULT '',
        created_at INTEGER NOT NULL
    )''')
    c.execute('INSERT OR IGNORE INTO device_status (id) VALUES (1)')
    c.execute("INSERT OR IGNORE INTO settings (key, value) VALUES ('door_password', '1234')")
    c.execute("INSERT OR IGNORE INTO settings (key, value) VALUES ('config_version', '1')")
    conn.commit()
    conn.close()

def insert_log(filename, timestamp, method, result, size):
    conn = get_db()
    conn.execute('INSERT INTO logs (filename, timestamp, method, result, size) VALUES (?, ?, ?, ?, ?)',
                 (filename, timestamp, method, result, size))
    conn.commit()
    conn.close()

def get_all_logs():
    conn = get_db()
    rows = conn.execute('SELECT filename, timestamp, method, result, size FROM logs ORDER BY timestamp DESC').fetchall()
    conn.close()
    return [dict(r) for r in rows]

def delete_log(filename):
    conn = get_db()
    conn.execute('DELETE FROM logs WHERE filename = ?', (filename,))
    conn.commit()
    conn.close()

def delete_all_logs():
    conn = get_db()
    conn.execute('DELETE FROM logs')
    conn.commit()
    conn.close()

def get_whitelist():
    conn = get_db()
    rows = conn.execute('SELECT id, type, value, name, created_at FROM whitelist ORDER BY created_at DESC').fetchall()
    conn.close()
    return [dict(r) for r in rows]

def add_whitelist(type_, value, name):
    conn = get_db()
    conn.execute('INSERT INTO whitelist (type, value, name, created_at) VALUES (?, ?, ?, ?)',
                 (type_, value, name, int(time.time())))
    conn.commit()
    conn.close()
    bump_config_version()

def delete_whitelist(id_):
    conn = get_db()
    conn.execute('DELETE FROM whitelist WHERE id = ?', (id_,))
    conn.commit()
    conn.close()
    bump_config_version()

def get_blacklist():
    conn = get_db()
    rows = conn.execute('SELECT id, type, value, name, created_at FROM blacklist ORDER BY created_at DESC').fetchall()
    conn.close()
    return [dict(r) for r in rows]

def add_blacklist(type_, value, name):
    conn = get_db()
    conn.execute('INSERT INTO blacklist (type, value, name, created_at) VALUES (?, ?, ?, ?)',
                 (type_, value, name, int(time.time())))
    conn.commit()
    conn.close()
    bump_config_version()

def delete_blacklist(id_):
    conn = get_db()
    conn.execute('DELETE FROM blacklist WHERE id = ?', (id_,))
    conn.commit()
    conn.close()
    bump_config_version()

def update_device_status(lock_state):
    conn = get_db()
    conn.execute('UPDATE device_status SET lock_state = ?, last_update = ? WHERE id = 1',
                 (lock_state, int(time.time())))
    conn.commit()
    conn.close()

def get_device_status():
    conn = get_db()
    row = conn.execute('SELECT * FROM device_status WHERE id = 1').fetchone()
    conn.close()
    if not row:
        return {'lock_state': 'UNKNOWN', 'cam_online': 0, 'last_update': 0}
    status = dict(row)
    if status['cam_online'] and status['last_update'] and (time.time() - status['last_update'] > 60):
        update_cam_online(False)
        status['cam_online'] = 0
    return status

def update_cam_online(online):
    conn = get_db()
    conn.execute('UPDATE device_status SET cam_online = ?, last_update = ? WHERE id = 1',
                 (1 if online else 0, int(time.time())))
    conn.commit()
    conn.close()

def get_setting(key):
    conn = get_db()
    row = conn.execute('SELECT value FROM settings WHERE key = ?', (key,)).fetchone()
    conn.close()
    return row[0] if row else None

def set_setting(key, value):
    conn = get_db()
    conn.execute('INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)', (key, value))
    conn.commit()
    conn.close()

def bump_config_version():
    v = int(get_setting('config_version') or '1')
    set_setting('config_version', str(v + 1))

def get_config_for_esp32():
    password = get_setting('door_password') or '1234'
    version = get_setting('config_version') or '1'
    conn = get_db()
    rfid_rows = conn.execute("SELECT value FROM whitelist WHERE type='rfid'").fetchall()
    fp_rows = conn.execute("SELECT value FROM whitelist WHERE type='fp'").fetchall()
    rfid_bl_rows = conn.execute("SELECT value FROM blacklist WHERE type='rfid'").fetchall()
    fp_bl_rows = conn.execute("SELECT value FROM blacklist WHERE type='fp'").fetchall()
    conn.close()
    return {
        'version': int(version),
        'door_password': password,
        'rfid_whitelist': [r[0] for r in rfid_rows],
        'fp_whitelist': [r[0] for r in fp_rows],
        'rfid_blacklist': [r[0] for r in rfid_bl_rows],
        'fp_blacklist': [r[0] for r in fp_bl_rows],
        'unlock_request': int(get_setting('unlock_request') or '0')
    }

def send_notification(title, content, event_type='all'):
    key = get_setting('serverchan_key')
    if not key:
        return
    allowed = get_setting('notify_events') or 'success,fail,remote'
    if event_type != 'all' and event_type not in allowed.split(','):
        return
    try:
        import urllib.request
        import urllib.parse
        url = f"https://sctapi.ftqq.com/{key}.send"
        data = urllib.parse.urlencode({'title': title, 'desp': content}).encode()
        req = urllib.request.Request(url, data=data)
        urllib.request.urlopen(req, timeout=5)
        print(f"[NOTIFY] {title}")
    except Exception as e:
        print(f"[NOTIFY] 失败: {e}")

def login_required(f):
    from functools import wraps
    @wraps(f)
    def decorated(*args, **kwargs):
        if not session.get('logged_in'):
            return redirect(url_for('login_page'))
        return f(*args, **kwargs)
    return decorated

init_db()

LOGIN_HTML = '''
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>管理员登录</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: #0f0f1a;
            color: #e0e0e0;
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
        }
        .login-box {
            background: #1a1a2e;
            border-radius: 12px;
            padding: 40px;
            width: 380px;
            border: 1px solid rgba(255,255,255,0.08);
        }
        .login-box h2 {
            text-align: center;
            color: #00d4ff;
            margin-bottom: 8px;
            font-size: 22px;
        }
        .login-box .sub {
            text-align: center;
            color: #666;
            font-size: 13px;
            margin-bottom: 30px;
        }
        .form-group { margin-bottom: 20px; }
        .form-group label {
            display: block;
            font-size: 13px;
            color: #888;
            margin-bottom: 6px;
        }
        .form-group input {
            width: 100%;
            padding: 12px 14px;
            background: rgba(255,255,255,0.05);
            border: 1px solid rgba(255,255,255,0.1);
            border-radius: 6px;
            color: #e0e0e0;
            font-size: 14px;
            outline: none;
            transition: border-color 0.2s;
        }
        .form-group input:focus { border-color: #00d4ff; }
        .btn-login {
            width: 100%;
            padding: 12px;
            background: #00d4ff;
            color: #0f0f1a;
            border: none;
            border-radius: 6px;
            font-size: 15px;
            font-weight: 600;
            cursor: pointer;
            transition: background 0.2s;
        }
        .btn-login:hover { background: #00b8e6; }
        .error {
            background: rgba(255,71,87,0.15);
            color: #ff4757;
            padding: 10px;
            border-radius: 6px;
            font-size: 13px;
            margin-bottom: 20px;
            text-align: center;
        }
        .back {
            display: block;
            text-align: center;
            margin-top: 16px;
            color: #666;
            font-size: 13px;
            text-decoration: none;
        }
        .back:hover { color: #00d4ff; }
    </style>
</head>
<body>
    <div class="login-box">
        <h2>管理员登录</h2>
        <p class="sub">智能门禁管理系统</p>
        {% if error %}
        <div class="error">{{ error }}</div>
        {% endif %}
        <form method="POST" action="/login">
            <div class="form-group">
                <label>用户名</label>
                <input type="text" name="username" placeholder="请输入用户名" required autofocus />
            </div>
            <div class="form-group">
                <label>密码</label>
                <input type="password" name="password" placeholder="请输入密码" required />
            </div>
            <button type="submit" class="btn-login">登 录</button>
        </form>
        <a href="/" class="back">返回首页</a>
    </div>
</body>
</html>
'''

ADMIN_HTML = '''
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>智能门禁管理系统</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: #0f0f1a;
            color: #e0e0e0;
            min-height: 100vh;
        }
        .container { max-width: 1100px; margin: 0 auto; padding: 0 20px; }
        header {
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 100%);
            border-bottom: 1px solid rgba(255,255,255,0.08);
            padding: 16px 0;
            position: sticky;
            top: 0;
            z-index: 100;
        }
        .header-inner {
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        header h1 { font-size: 20px; color: #00d4ff; font-weight: 600; }
        header .sub { font-size: 12px; color: #666; margin-top: 2px; }
        .header-right { display: flex; align-items: center; gap: 12px; }
        .user-badge {
            font-size: 12px;
            color: #888;
            background: rgba(255,255,255,0.05);
            padding: 6px 12px;
            border-radius: 6px;
        }
        .btn {
            padding: 8px 16px;
            border: none;
            border-radius: 6px;
            font-size: 13px;
            font-weight: 500;
            cursor: pointer;
            transition: all 0.2s;
        }
        .btn-primary { background: #00d4ff; color: #0f0f1a; }
        .btn-primary:hover { background: #00b8e6; }
        .btn-danger { background: #ff4757; color: #fff; }
        .btn-danger:hover { background: #ff6b81; }
        .btn-outline {
            background: transparent;
            color: #888;
            border: 1px solid rgba(255,255,255,0.15);
        }
        .btn-outline:hover { border-color: #00d4ff; color: #00d4ff; }
        .btn-sm { padding: 5px 12px; font-size: 12px; }
        .btn-success { background: #2ed573; color: #0f0f1a; }
        .btn-success:hover { background: #26c066; }

        .status-bar {
            background: rgba(255,255,255,0.03);
            border: 1px solid rgba(255,255,255,0.06);
            border-radius: 8px;
            padding: 14px 20px;
            margin: 20px 0;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        .status-left { display: flex; gap: 24px; font-size: 13px; }
        .status-dot {
            display: inline-block;
            width: 8px;
            height: 8px;
            border-radius: 50%;
            margin-right: 6px;
            vertical-align: middle;
        }
        .dot-green { background: #2ed573; box-shadow: 0 0 6px #2ed573; }
        .dot-red { background: #ff4757; box-shadow: 0 0 6px #ff4757; }
        .dot-yellow { background: #ffa502; box-shadow: 0 0 6px #ffa502; }
        .status-label { color: #666; }
        .status-value { color: #e0e0e0; font-weight: 500; }

        .tabs {
            display: flex;
            gap: 0;
            margin-bottom: 20px;
            border-bottom: 1px solid rgba(255,255,255,0.08);
        }
        .tab {
            padding: 12px 24px;
            font-size: 14px;
            color: #888;
            cursor: pointer;
            border-bottom: 2px solid transparent;
            transition: all 0.2s;
        }
        .tab:hover { color: #e0e0e0; }
        .tab.active { color: #00d4ff; border-bottom-color: #00d4ff; }

        .toolbar {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 15px;
        }
        .stats-row { font-size: 13px; color: #888; }
        .stats-row span { color: #00d4ff; font-weight: 600; }

        .table-wrap {
            background: rgba(255,255,255,0.03);
            border-radius: 10px;
            overflow: hidden;
            border: 1px solid rgba(255,255,255,0.06);
        }
        table { width: 100%; border-collapse: collapse; }
        thead th {
            background: rgba(0,0,0,0.3);
            padding: 12px 16px;
            text-align: left;
            font-size: 12px;
            color: #888;
            font-weight: 500;
            text-transform: uppercase;
            letter-spacing: 0.5px;
        }
        tbody tr {
            border-bottom: 1px solid rgba(255,255,255,0.04);
            cursor: pointer;
            transition: background 0.15s;
        }
        tbody tr:hover { background: rgba(0,212,255,0.05); }
        tbody tr:last-child { border-bottom: none; }
        tbody td { padding: 14px 16px; font-size: 13px; }

        .thumb {
            width: 48px;
            height: 36px;
            border-radius: 4px;
            object-fit: cover;
            background: #1a1a2e;
        }
        .time-col { color: #aaa; white-space: nowrap; }
        .badge {
            display: inline-block;
            padding: 3px 10px;
            border-radius: 4px;
            font-size: 12px;
            font-weight: 500;
        }
        .m-rfid { background: rgba(0,212,255,0.15); color: #00d4ff; }
        .m-fp { background: rgba(46,213,115,0.15); color: #2ed573; }
        .m-pwd { background: rgba(255,165,2,0.15); color: #ffa502; }
        .m-ble { background: rgba(155,89,182,0.15); color: #b569d9; }
        .m-unknown { background: rgba(136,136,136,0.15); color: #888; }
        .r-success { background: rgba(46,213,115,0.15); color: #2ed573; }
        .r-fail { background: rgba(255,71,87,0.15); color: #ff4757; }
        .w-rfid { background: rgba(0,212,255,0.15); color: #00d4ff; }
        .w-fp { background: rgba(46,213,115,0.15); color: #2ed573; }
        .b-rfid { background: rgba(255,71,87,0.15); color: #ff4757; }
        .b-fp { background: rgba(255,165,2,0.15); color: #ffa502; }
        .size-col { color: #666; }
        .actions-col { text-align: right; }

        .empty {
            text-align: center;
            padding: 80px 20px;
            color: #555;
        }

        .modal-overlay {
            display: none;
            position: fixed;
            top: 0; left: 0; right: 0; bottom: 0;
            background: rgba(0,0,0,0.85);
            z-index: 1000;
            justify-content: center;
            align-items: center;
        }
        .modal-overlay.active { display: flex; }
        .modal {
            background: #1a1a2e;
            border-radius: 12px;
            max-width: 600px;
            width: 90%;
            overflow: hidden;
            border: 1px solid rgba(255,255,255,0.1);
        }
        .modal img {
            width: 100%;
            max-height: 400px;
            object-fit: contain;
            background: #000;
        }
        .modal-info { padding: 20px; }
        .modal-info .detail-row {
            display: flex;
            justify-content: space-between;
            padding: 8px 0;
            border-bottom: 1px solid rgba(255,255,255,0.05);
            font-size: 14px;
        }
        .modal-info .detail-row:last-child { border-bottom: none; }
        .modal-info .label { color: #888; }
        .modal-close {
            display: block;
            width: 100%;
            padding: 14px;
            border: none;
            background: rgba(255,255,255,0.05);
            color: #888;
            font-size: 14px;
            cursor: pointer;
        }
        .modal-close:hover { background: rgba(255,255,255,0.1); color: #fff; }

        .add-form {
            background: rgba(255,255,255,0.03);
            border: 1px solid rgba(255,255,255,0.06);
            border-radius: 8px;
            padding: 20px;
            margin-bottom: 20px;
            display: none;
        }
        .add-form.active { display: block; }
        .add-form h3 { font-size: 14px; color: #00d4ff; margin-bottom: 15px; }
        .form-row { display: flex; gap: 12px; align-items: flex-end; }
        .form-field { flex: 1; }
        .form-field label {
            display: block;
            font-size: 12px;
            color: #888;
            margin-bottom: 4px;
        }
        .form-field input, .form-field select {
            width: 100%;
            padding: 10px 12px;
            background: rgba(255,255,255,0.05);
            border: 1px solid rgba(255,255,255,0.1);
            border-radius: 6px;
            color: #e0e0e0;
            font-size: 13px;
            outline: none;
        }
        .form-field input:focus, .form-field select:focus { border-color: #00d4ff; }
        .form-field select option { background: #1a1a2e; }
    </style>
</head>
<body>
    <header>
        <div class="container">
            <div class="header-inner">
                <div>
                    <h1>智能门禁管理系统</h1>
                    <div class="sub">实时监控中 | 本地服务 | 自动记录验证信息</div>
                </div>
                <div class="header-right">
                    <span id="sysTime" style="color:#00d4ff;font-size:14px;font-weight:600;margin-right:12px"></span>
                    <span class="user-badge">admin</span>
                    <a href="/logout" class="btn btn-outline">退出</a>
                </div>
            </div>
        </div>
    </header>

    <div class="container">
        <div class="status-bar">
            <div class="status-left">
                <div>
                    <span class="status-label">门锁状态: </span>
                    <span class="status-value" id="lockState">
                        <span class="status-dot dot-green"></span>已锁定
                    </span>
                </div>
                <div>
                    <span class="status-label">摄像头: </span>
                    <span class="status-value" id="camStatus">
                        <span class="status-dot dot-green"></span>在线
                    </span>
                </div>
                <div>
                    <span class="status-label">最后活动: </span>
                    <span class="status-value" id="lastActivity">-</span>
                </div>
            </div>
            <div style="display:flex;gap:10px">
                <button class="btn btn-primary btn-sm" onclick="remoteUnlock()">远程开锁</button>
                <button class="btn btn-outline btn-sm" onclick="refreshStatus()">刷新</button>
            </div>
        </div>

        <div class="tabs">
            <div class="tab active" onclick="switchTab('records')">验证记录</div>
            <div class="tab" onclick="switchTab('whitelist')">白名单管理</div>
            <div class="tab" onclick="switchTab('blacklist')">黑名单管理</div>
            <div class="tab" onclick="switchTab('notify')">通知设置</div>
            <div class="tab" onclick="switchTab('settings')">密码设置</div>
        </div>

        <div id="tab-records">
            <div class="toolbar">
                <div class="stats-row">
                    共 <span id="totalCount">0</span> 条记录 &nbsp;|&nbsp;
                    存储 <span id="totalSize">0 KB</span>
                </div>
                <div style="display:flex;gap:10px">
                    <button class="btn btn-primary" onclick="loadRecords()">刷新</button>
                    <button class="btn btn-outline btn-sm" onclick="exportCSV()">导出CSV</button>
                    <button class="btn btn-danger btn-sm" onclick="deleteAllRecords()">清空</button>
                </div>
            </div>
            <div class="table-wrap">
                <table>
                    <thead>
                        <tr>
                            <th style="width:60px">照片</th>
                            <th>时间</th>
                            <th>验证方式</th>
                            <th>结果</th>
                            <th>大小</th>
                            <th style="width:80px;text-align:right">操作</th>
                        </tr>
                    </thead>
                    <tbody id="recordsBody"></tbody>
                </table>
            </div>
            <div class="empty" id="recordsEmpty" style="display:none">暂无门禁记录</div>
        </div>

        <div id="tab-whitelist" style="display:none">
            <div class="toolbar">
                <div></div>
                <button class="btn btn-success" onclick="toggleAddForm()">+ 添加</button>
            </div>
            <div class="add-form" id="addForm">
                <h3>添加白名单</h3>
                <div class="form-row">
                    <div class="form-field">
                        <label>类型</label>
                        <select id="addType">
                            <option value="rfid">RFID 卡号</option>
                            <option value="fp">指纹 ID</option>
                        </select>
                    </div>
                    <div class="form-field">
                        <label>卡号/ID</label>
                        <input type="text" id="addValue" placeholder="如: 0C:D5:D4:E7" />
                    </div>
                    <div class="form-field">
                        <label>备注名称</label>
                        <input type="text" id="addName" placeholder="如: 我的卡片" />
                    </div>
                    <button class="btn btn-success" onclick="addWhitelist()">确认添加</button>
                </div>
            </div>
            <div class="table-wrap">
                <table>
                    <thead>
                        <tr>
                            <th>类型</th>
                            <th>卡号/ID</th>
                            <th>备注名称</th>
                            <th>添加时间</th>
                            <th style="width:80px;text-align:right">操作</th>
                        </tr>
                    </thead>
                    <tbody id="whitelistBody"></tbody>
                </table>
            </div>
            <div class="empty" id="whitelistEmpty" style="display:none">暂无白名单</div>
        </div>

        <div id="tab-blacklist" style="display:none">
            <div class="toolbar">
                <div></div>
                <button class="btn btn-danger" onclick="toggleAddBlacklistForm()">+ 添加黑名单</button>
            </div>
            <div class="add-form" id="addBlacklistForm">
                <h3>添加黑名单</h3>
                <div class="form-row">
                    <div class="form-field">
                        <label>类型</label>
                        <select id="blAddType">
                            <option value="rfid">RFID 卡号</option>
                            <option value="fp">指纹 ID</option>
                        </select>
                    </div>
                    <div class="form-field">
                        <label>卡号/ID</label>
                        <input type="text" id="blAddValue" placeholder="如: 0C:D5:D4:E7" />
                    </div>
                    <div class="form-field">
                        <label>备注名称</label>
                        <input type="text" id="blAddName" placeholder="如: 丢失的卡" />
                    </div>
                    <button class="btn btn-danger" onclick="addBlacklist()">确认添加</button>
                </div>
            </div>
            <div class="table-wrap">
                <table>
                    <thead>
                        <tr>
                            <th>类型</th>
                            <th>卡号/ID</th>
                            <th>备注名称</th>
                            <th>添加时间</th>
                            <th style="width:80px;text-align:right">操作</th>
                        </tr>
                    </thead>
                    <tbody id="blacklistBody"></tbody>
                </table>
            </div>
            <div class="empty" id="blacklistEmpty" style="display:none">暂无黑名单</div>
        </div>

        <div id="tab-notify" style="display:none">
            <div style="display:flex;gap:20px;flex-wrap:wrap">
                <div class="add-form active" style="flex:1;min-width:350px">
                    <div style="display:flex;align-items:center;gap:12px;margin-bottom:20px">
                        <div style="width:44px;height:44px;border-radius:12px;background:linear-gradient(135deg,#2ed573,#17c964);display:flex;align-items:center;justify-content:center;font-size:20px">📱</div>
                        <div>
                            <h3 style="margin:0;font-size:16px;color:#fff">微信通知</h3>
                            <div style="font-size:12px;color:#888;margin-top:2px">通过 Server酱 推送到微信</div>
                        </div>
                        <div id="notifyStatus" style="margin-left:auto;padding:4px 12px;border-radius:20px;font-size:12px;background:#333;color:#888">未配置</div>
                    </div>
                    <div style="display:flex;flex-direction:column;gap:16px">
                        <div class="form-field">
                            <label style="font-size:13px;color:#aaa;margin-bottom:6px;display:block">SendKey</label>
                            <input type="text" id="notifyKey" placeholder="SCT 开头的 key" style="background:#1a1a2e;border:1px solid #333;border-radius:8px;padding:10px 14px;color:#fff;font-size:14px;width:100%;box-sizing:border-box;transition:border-color 0.2s" onfocus="this.style.borderColor='#2ed573'" onblur="this.style.borderColor='#333'" />
                        </div>
                        <div style="background:#1a1a2e;border-radius:10px;padding:14px 16px;font-size:12px;color:#888;line-height:1.8">
                            <div style="color:#aaa;font-weight:500;margin-bottom:4px">📖 获取步骤</div>
                            <div>1. 访问 <a href="https://sct.ftqq.com/" target="_blank" style="color:#00d4ff;text-decoration:none">sct.ftqq.com</a></div>
                            <div>2. 用 GitHub 账号登录</div>
                            <div>3. 点击「SendKey」复制 key</div>
                            <div>4. 粘贴到上方输入框</div>
                        </div>
                        <div style="display:flex;gap:10px">
                            <button class="btn btn-primary" onclick="saveNotify()" style="flex:1">保存配置</button>
                            <button class="btn btn-outline" onclick="testNotify()" style="flex:1">测试推送</button>
                        </div>
                        <div id="notifyMsg" style="font-size:13px;display:none;padding:10px 14px;border-radius:8px"></div>
                    </div>
                </div>

                <div class="add-form active" style="flex:0.6;min-width:280px">
                    <div style="display:flex;align-items:center;gap:12px;margin-bottom:20px">
                        <div style="width:44px;height:44px;border-radius:12px;background:linear-gradient(135deg,#ffa502,#ff6348);display:flex;align-items:center;justify-content:center;font-size:20px">🔔</div>
                        <div>
                            <h3 style="margin:0;font-size:16px;color:#fff">通知类型</h3>
                            <div style="font-size:12px;color:#888;margin-top:2px">选择需要推送的事件</div>
                        </div>
                    </div>
                    <div style="display:flex;flex-direction:column;gap:10px">
                        <label style="display:flex;align-items:center;gap:12px;padding:12px 14px;background:#1a1a2e;border-radius:8px;cursor:pointer;transition:background 0.2s" onmouseover="this.style.background='#222'" onmouseout="this.style.background='#1a1a2e'">
                            <input type="checkbox" id="evtSuccess" checked style="width:18px;height:18px;accent-color:#2ed573" onchange="saveNotifyEvents()" />
                            <div>
                                <div style="font-size:13px;color:#fff">开锁成功</div>
                                <div style="font-size:11px;color:#666">RFID / 指纹 / 密码 开门</div>
                            </div>
                        </label>
                        <label style="display:flex;align-items:center;gap:12px;padding:12px 14px;background:#1a1a2e;border-radius:8px;cursor:pointer;transition:background 0.2s" onmouseover="this.style.background='#222'" onmouseout="this.style.background='#1a1a2e'">
                            <input type="checkbox" id="evtFail" checked style="width:18px;height:18px;accent-color:#ff4757" onchange="saveNotifyEvents()" />
                            <div>
                                <div style="font-size:13px;color:#fff">开锁失败</div>
                                <div style="font-size:11px;color:#666">密码错误 / 未授权</div>
                            </div>
                        </label>
                        <label style="display:flex;align-items:center;gap:12px;padding:12px 14px;background:#1a1a2e;border-radius:8px;cursor:pointer;transition:background 0.2s" onmouseover="this.style.background='#222'" onmouseout="this.style.background='#1a1a2e'">
                            <input type="checkbox" id="evtRemote" checked style="width:18px;height:18px;accent-color:#00d4ff" onchange="saveNotifyEvents()" />
                            <div>
                                <div style="font-size:13px;color:#fff">远程开锁</div>
                                <div style="font-size:11px;color:#666">管理员通过网页开锁</div>
                            </div>
                        </label>
                        <label style="display:flex;align-items:center;gap:12px;padding:12px 14px;background:#1a1a2e;border-radius:8px;cursor:pointer;transition:background 0.2s" onmouseover="this.style.background='#222'" onmouseout="this.style.background='#1a1a2e'">
                            <input type="checkbox" id="evtCapture" style="width:18px;height:18px;accent-color:#ffa502" onchange="saveNotifyEvents()" />
                            <div>
                                <div style="font-size:13px;color:#fff">摄像头抓拍</div>
                                <div style="font-size:11px;color:#666">每次开锁拍照通知</div>
                            </div>
                        </label>
                    </div>
                </div>
            </div>
        </div>

        <div id="tab-settings" style="display:none">
            <div style="display:flex;gap:20px;flex-wrap:wrap">
                <div class="add-form active" style="flex:1;min-width:350px">
                    <div style="display:flex;align-items:center;gap:12px;margin-bottom:20px">
                        <div style="width:44px;height:44px;border-radius:12px;background:linear-gradient(135deg,#7c5cfc,#5b3cc4);display:flex;align-items:center;justify-content:center;font-size:20px">🔐</div>
                        <div>
                            <h3 style="margin:0;font-size:16px;color:#fff">开门密码</h3>
                            <div style="font-size:12px;color:#888;margin-top:2px">修改门锁的 4 位数字密码</div>
                        </div>
                    </div>
                    <div style="display:flex;flex-direction:column;gap:16px">
                        <div class="form-field">
                            <label style="font-size:13px;color:#aaa;margin-bottom:6px;display:block">当前密码</label>
                            <div style="position:relative">
                                <input type="password" id="currentPwd" readonly style="background:#1a1a2e;border:1px solid #333;border-radius:8px;padding:10px 40px 10px 14px;color:#fff;font-size:16px;width:100%;box-sizing:border-box;opacity:0.6;letter-spacing:4px" />
                                <span onclick="togglePwdVis('currentPwd')" style="position:absolute;right:12px;top:50%;transform:translateY(-50%);cursor:pointer;font-size:16px;opacity:0.5">👁</span>
                            </div>
                        </div>
                        <div class="form-field">
                            <label style="font-size:13px;color:#aaa;margin-bottom:6px;display:block">新密码</label>
                            <div style="position:relative">
                                <input type="password" id="newPwd" placeholder="输入 4 位数字" maxlength="4" pattern="[0-9]{4}" oninput="this.value=this.value.replace(/[^0-9]/g,'')" style="background:#1a1a2e;border:1px solid #333;border-radius:8px;padding:10px 40px 10px 14px;color:#fff;font-size:16px;width:100%;box-sizing:border-box;letter-spacing:4px;transition:border-color 0.2s" onfocus="this.style.borderColor='#7c5cfc'" onblur="this.style.borderColor='#333'" />
                                <span onclick="togglePwdVis('newPwd')" style="position:absolute;right:12px;top:50%;transform:translateY(-50%);cursor:pointer;font-size:16px;opacity:0.5">👁</span>
                            </div>
                            <div id="pwdStrength" style="display:flex;gap:4px;margin-top:8px">
                                <div style="flex:1;height:3px;border-radius:2px;background:#333;transition:background 0.3s" id="str1"></div>
                                <div style="flex:1;height:3px;border-radius:2px;background:#333;transition:background 0.3s" id="str2"></div>
                                <div style="flex:1;height:3px;border-radius:2px;background:#333;transition:background 0.3s" id="str3"></div>
                            </div>
                            <div id="pwdHint" style="font-size:11px;color:#666;margin-top:4px"></div>
                        </div>
                        <div class="form-field">
                            <label style="font-size:13px;color:#aaa;margin-bottom:6px;display:block">确认新密码</label>
                            <div style="position:relative">
                                <input type="password" id="confirmPwd" placeholder="再次输入密码" maxlength="4" pattern="[0-9]{4}" oninput="this.value=this.value.replace(/[^0-9]/g,'')" style="background:#1a1a2e;border:1px solid #333;border-radius:8px;padding:10px 40px 10px 14px;color:#fff;font-size:16px;width:100%;box-sizing:border-box;letter-spacing:4px;transition:border-color 0.2s" onfocus="this.style.borderColor='#7c5cfc'" onblur="this.style.borderColor='#333'" />
                                <span onclick="togglePwdVis('confirmPwd')" style="position:absolute;right:12px;top:50%;transform:translateY(-50%);cursor:pointer;font-size:16px;opacity:0.5">👁</span>
                            </div>
                            <div id="pwdMatch" style="font-size:11px;margin-top:4px"></div>
                        </div>
                        <button class="btn btn-primary" onclick="savePassword()" style="align-self:flex-start;background:linear-gradient(135deg,#7c5cfc,#5b3cc4)">保存密码</button>
                        <div id="pwdMsg" style="font-size:13px;display:none;padding:10px 14px;border-radius:8px"></div>
                    </div>
                </div>

                <div class="add-form active" style="flex:0.6;min-width:280px">
                    <div style="display:flex;align-items:center;gap:12px;margin-bottom:20px">
                        <div style="width:44px;height:44px;border-radius:12px;background:linear-gradient(135deg,#00d4ff,#0098b3);display:flex;align-items:center;justify-content:center;font-size:20px">💡</div>
                        <div>
                            <h3 style="margin:0;font-size:16px;color:#fff">安全提示</h3>
                            <div style="font-size:12px;color:#888;margin-top:2px">保护你的门锁安全</div>
                        </div>
                    </div>
                    <div style="display:flex;flex-direction:column;gap:12px">
                        <div style="padding:14px;background:#1a1a2e;border-radius:8px;border-left:3px solid #2ed573">
                            <div style="font-size:13px;color:#fff;margin-bottom:4px">✅ 建议</div>
                            <div style="font-size:12px;color:#888;line-height:1.6">使用不与其他账户重复的 4 位数字组合，定期更换密码。</div>
                        </div>
                        <div style="padding:14px;background:#1a1a2e;border-radius:8px;border-left:3px solid #ffa502">
                            <div style="font-size:13px;color:#fff;margin-bottom:4px">⚠️ 注意</div>
                            <div style="font-size:12px;color:#888;line-height:1.6">修改后密码将同步到 ESP32，请确保设备在线。</div>
                        </div>
                        <div style="padding:14px;background:#1a1a2e;border-radius:8px;border-left:3px solid #ff4757">
                            <div style="font-size:13px;color:#fff;margin-bottom:4px">🚫 避免</div>
                            <div style="font-size:12px;color:#888;line-height:1.6">不要使用 0000、1111、1234 等简单组合。</div>
                        </div>
                    </div>
                </div>
            </div>
        </div>
    </div>

    <div class="modal-overlay" id="modal" onclick="closeModal(event)">
        <div class="modal" onclick="event.stopPropagation()">
            <img id="modalImg" src="" />
            <div class="modal-info">
                <div class="detail-row"><span class="label">时间</span><span class="value" id="modalTime"></span></div>
                <div class="detail-row"><span class="label">验证方式</span><span class="value" id="modalMethod"></span></div>
                <div class="detail-row"><span class="label">结果</span><span class="value" id="modalResult"></span></div>
                <div class="detail-row"><span class="label">文件大小</span><span class="value" id="modalSize"></span></div>
            </div>
            <button class="modal-close" onclick="closeModal()">关闭</button>
        </div>
    </div>

    <script>
        function fmtSize(b) {
            if (b < 1024) return b + ' B';
            if (b < 1048576) return (b/1024).toFixed(1) + ' KB';
            return (b/1048576).toFixed(2) + ' MB';
        }
        function fmtTime(ts) {
            const d = new Date(ts * 1000);
            const pad = n => String(n).padStart(2, '0');
            return d.getFullYear() + '-' + pad(d.getMonth()+1) + '-' + pad(d.getDate()) + ' '
                 + pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
        }
        function methodBadge(m) {
            const map = {'RFID': ['m-rfid','RFID刷卡'], '指纹': ['m-fp','指纹识别'], '密码': ['m-pwd','密码输入'], '蓝牙': ['m-ble','蓝牙开锁']};
            const [cls, txt] = map[m] || ['m-unknown', m];
            return '<span class="badge ' + cls + '">' + txt + '</span>';
        }
        function resultBadge(r) {
            const cls = r === 'SUCCESS' ? 'r-success' : 'r-fail';
            const txt = r === 'SUCCESS' ? '成功' : '失败';
            return '<span class="badge ' + cls + '">' + txt + '</span>';
        }

        let currentTab = 'records';
        function switchTab(tab) {
            currentTab = tab;
            document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
            event.target.classList.add('active');
            document.getElementById('tab-records').style.display = tab === 'records' ? 'block' : 'none';
            document.getElementById('tab-whitelist').style.display = tab === 'whitelist' ? 'block' : 'none';
            document.getElementById('tab-blacklist').style.display = tab === 'blacklist' ? 'block' : 'none';
            document.getElementById('tab-notify').style.display = tab === 'notify' ? 'block' : 'none';
            document.getElementById('tab-settings').style.display = tab === 'settings' ? 'block' : 'none';
            if (tab === 'records') loadRecords();
            if (tab === 'whitelist') loadWhitelist();
            if (tab === 'blacklist') loadBlacklist();
            if (tab === 'settings') loadSettings();
        }

        async function loadRecords() {
            const res = await fetch('/photos');
            const data = await res.json();
            const tbody = document.getElementById('recordsBody');
            const empty = document.getElementById('recordsEmpty');
            if (data.length === 0) {
                tbody.innerHTML = '';
                empty.style.display = 'block';
                document.getElementById('totalCount').textContent = '0';
                document.getElementById('totalSize').textContent = '0 KB';
                return;
            }
            empty.style.display = 'none';
            const totalSize = data.reduce((s, p) => s + p.size, 0);
            document.getElementById('totalCount').textContent = data.length;
            document.getElementById('totalSize').textContent = fmtSize(totalSize);
            tbody.innerHTML = data.map(p => `
                <tr onclick="showDetail('${p.filename}','${fmtTime(p.timestamp)}','${p.method}','${p.result}',${p.size})">
                    <td><img class="thumb" src="/photo/${encodeURIComponent(p.filename)}" /></td>
                    <td class="time-col">${fmtTime(p.timestamp)}</td>
                    <td>${methodBadge(p.method)}</td>
                    <td>${resultBadge(p.result)}</td>
                    <td class="size-col">${fmtSize(p.size)}</td>
                    <td class="actions-col"><button class="btn btn-danger btn-sm" onclick="event.stopPropagation();delRecord('${p.filename}')">删除</button></td>
                </tr>
            `).join('');
        }

        async function loadWhitelist() {
            const res = await fetch('/api/whitelist');
            const data = await res.json();
            const tbody = document.getElementById('whitelistBody');
            const empty = document.getElementById('whitelistEmpty');
            if (data.length === 0) {
                tbody.innerHTML = '';
                empty.style.display = 'block';
                return;
            }
            empty.style.display = 'none';
            tbody.innerHTML = data.map(w => `
                <tr>
                    <td><span class="badge ${w.type === 'rfid' ? 'w-rfid' : 'w-fp'}">${w.type === 'rfid' ? 'RFID' : '指纹'}</span></td>
                    <td style="font-family:monospace">${w.value}</td>
                    <td>${w.name || '-'}</td>
                    <td class="time-col">${fmtTime(w.created_at)}</td>
                    <td class="actions-col"><button class="btn btn-danger btn-sm" onclick="delWhitelist(${w.id})">删除</button></td>
                </tr>
            `).join('');
        }

        function toggleAddForm() {
            document.getElementById('addForm').classList.toggle('active');
        }

        async function addWhitelist() {
            const type = document.getElementById('addType').value;
            const value = document.getElementById('addValue').value.trim();
            const name = document.getElementById('addName').value.trim();
            if (!value) { alert('请输入卡号/ID'); return; }
            await fetch('/api/whitelist', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({type, value, name})
            });
            document.getElementById('addValue').value = '';
            document.getElementById('addName').value = '';
            document.getElementById('addForm').classList.remove('active');
            loadWhitelist();
        }

        async function delWhitelist(id) {
            if (!confirm('确定删除该白名单？')) return;
            await fetch('/api/whitelist/' + id, {method: 'DELETE'});
            loadWhitelist();
        }

        async function loadBlacklist() {
            const res = await fetch('/api/blacklist');
            const data = await res.json();
            const tbody = document.getElementById('blacklistBody');
            const empty = document.getElementById('blacklistEmpty');
            if (data.length === 0) {
                tbody.innerHTML = '';
                empty.style.display = 'block';
                return;
            }
            empty.style.display = 'none';
            tbody.innerHTML = data.map(b => `
                <tr>
                    <td><span class="badge ${b.type === 'rfid' ? 'b-rfid' : 'b-fp'}">${b.type === 'rfid' ? 'RFID' : '指纹'}</span></td>
                    <td style="font-family:monospace">${b.value}</td>
                    <td>${b.name || '-'}</td>
                    <td class="time-col">${fmtTime(b.created_at)}</td>
                    <td class="actions-col"><button class="btn btn-danger btn-sm" onclick="delBlacklist(${b.id})">删除</button></td>
                </tr>
            `).join('');
        }

        function toggleAddBlacklistForm() {
            document.getElementById('addBlacklistForm').classList.toggle('active');
        }

        async function addBlacklist() {
            const type = document.getElementById('blAddType').value;
            const value = document.getElementById('blAddValue').value.trim();
            const name = document.getElementById('blAddName').value.trim();
            if (!value) { alert('请输入卡号/ID'); return; }
            await fetch('/api/blacklist', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({type, value, name})
            });
            document.getElementById('blAddValue').value = '';
            document.getElementById('blAddName').value = '';
            document.getElementById('addBlacklistForm').classList.remove('active');
            loadBlacklist();
        }

        async function delBlacklist(id) {
            if (!confirm('确定删除该黑名单？')) return;
            await fetch('/api/blacklist/' + id, {method: 'DELETE'});
            loadBlacklist();
        }

        async function refreshStatus() {
            const res = await fetch('/api/status');
            const s = await res.json();
            const lockDot = s.lock_state === 'LOCKED' ? 'dot-green' : 'dot-red';
            const lockText = s.lock_state === 'LOCKED' ? '已锁定' : '已开锁';
            document.getElementById('lockState').innerHTML = '<span class="status-dot ' + lockDot + '"></span>' + lockText;
            const camDot = s.cam_online ? 'dot-green' : 'dot-red';
            const camText = s.cam_online ? '在线' : '离线';
            document.getElementById('camStatus').innerHTML = '<span class="status-dot ' + camDot + '"></span>' + camText;
            document.getElementById('lastActivity').textContent = s.last_update > 0 ? fmtTime(s.last_update) : '-';
        }

        function showDetail(filename, time, method, result, size) {
            document.getElementById('modalImg').src = '/photo/' + encodeURIComponent(filename);
            document.getElementById('modalTime').textContent = time;
            const methodText = method === 'RFID' ? 'RFID刷卡' : method === '指纹' ? '指纹识别' : method === '密码' ? '密码输入' : method;
            document.getElementById('modalMethod').textContent = methodText;
            document.getElementById('modalResult').innerHTML = resultBadge(result);
            document.getElementById('modalSize').textContent = fmtSize(size);
            document.getElementById('modal').classList.add('active');
        }
        function closeModal(e) {
            if (!e || e.target === document.getElementById('modal'))
                document.getElementById('modal').classList.remove('active');
        }

        async function delRecord(filename) {
            if (!confirm('确定删除该记录？')) return;
            await fetch('/delete/' + encodeURIComponent(filename), {method: 'DELETE'});
            loadRecords();
        }
        async function deleteAllRecords() {
            if (!confirm('确定清空所有记录？此操作不可恢复！')) return;
            await fetch('/delete-all', {method: 'DELETE'});
            loadRecords();
        }

        async function loadSettings() {
            const res = await fetch('/api/config');
            const cfg = await res.json();
            document.getElementById('currentPwd').value = cfg.door_password;
            const nr = await fetch('/api/notify-settings');
            const ncfg = await nr.json();
            const key = ncfg.serverchan_key || '';
            document.getElementById('notifyKey').value = key;
            const status = document.getElementById('notifyStatus');
            if (key) {
                status.textContent = '已配置';
                status.style.background = '#2ed573';
                status.style.color = '#fff';
            }
            const events = (ncfg.notify_events || 'success,fail,remote').split(',');
            document.getElementById('evtSuccess').checked = events.includes('success');
            document.getElementById('evtFail').checked = events.includes('fail');
            document.getElementById('evtRemote').checked = events.includes('remote');
            document.getElementById('evtCapture').checked = events.includes('capture');
        }

        async function savePassword() {
            const newPwd = document.getElementById('newPwd').value.trim();
            const confirmPwd = document.getElementById('confirmPwd').value.trim();
            const msg = document.getElementById('pwdMsg');
            if (newPwd.length !== 4 || !/^[0-9]{4}$/.test(newPwd)) {
                msg.style.display = 'block';
                msg.style.background = 'rgba(255,71,87,0.1)';
                msg.style.color = '#ff4757';
                msg.textContent = '❌ 密码必须是 4 位数字';
                return;
            }
            if (newPwd !== confirmPwd) {
                msg.style.display = 'block';
                msg.style.background = 'rgba(255,71,87,0.1)';
                msg.style.color = '#ff4757';
                msg.textContent = '❌ 两次输入的密码不一致';
                return;
            }
            const res = await fetch('/api/config', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({door_password: newPwd})
            });
            const data = await res.json();
            msg.style.display = 'block';
            if (data.success) {
                msg.style.background = 'rgba(46,213,115,0.1)';
                msg.style.color = '#2ed573';
                msg.textContent = '✅ 密码已保存，ESP32 将在下次同步时更新';
                document.getElementById('currentPwd').value = newPwd;
            } else {
                msg.style.background = 'rgba(255,71,87,0.1)';
                msg.style.color = '#ff4757';
                msg.textContent = '❌ 保存失败: ' + (data.error || '未知错误');
            }
            document.getElementById('newPwd').value = '';
            document.getElementById('confirmPwd').value = '';
            document.getElementById('pwdHint').textContent = '';
            document.getElementById('pwdMatch').textContent = '';
            document.getElementById('str1').style.background = '#333';
            document.getElementById('str2').style.background = '#333';
            document.getElementById('str3').style.background = '#333';
            loadSettings();
        }

        function togglePwdVis(id) {
            const el = document.getElementById(id);
            el.type = el.type === 'password' ? 'text' : 'password';
        }

        document.getElementById('newPwd').addEventListener('input', function() {
            const v = this.value;
            const s1 = document.getElementById('str1');
            const s2 = document.getElementById('str2');
            const s3 = document.getElementById('str3');
            const hint = document.getElementById('pwdHint');
            s1.style.background = '#333'; s2.style.background = '#333'; s3.style.background = '#333';
            if (v.length === 0) { hint.textContent = ''; return; }
            let score = 0;
            if (/^(\\d)\\1{3}$/.test(v)) { hint.textContent = '❌ 不要使用重复数字'; hint.style.color = '#ff4757'; }
            else if (/^(0123|1234|2345|3456|4567|5678|6789|9876|8765|7654|6543|5432|4321|3210)/.test(v)) { hint.textContent = '⚠️ 连续数字不够安全'; hint.style.color = '#ffa502'; score = 1; }
            else { hint.textContent = '✨ 密码强度不错'; hint.style.color = '#2ed573'; score = 2; }
            if (v.length >= 1) s1.style.background = score >= 0 ? '#2ed573' : '#ff4757';
            if (v.length >= 2) s2.style.background = score >= 1 ? '#ffa502' : '#2ed573';
            if (v.length >= 3) s3.style.background = score >= 2 ? '#2ed573' : '#ffa502';
        });

        document.getElementById('confirmPwd').addEventListener('input', function() {
            const match = document.getElementById('pwdMatch');
            const newPwd = document.getElementById('newPwd').value;
            if (this.value.length === 0) { match.textContent = ''; return; }
            if (this.value === newPwd) { match.textContent = '✅ 密码一致'; match.style.color = '#2ed573'; }
            else { match.textContent = '❌ 密码不一致'; match.style.color = '#ff4757'; }
        });

        function updateSysTime() {
            const now = new Date();
            const pad = n => String(n).padStart(2, '0');
            document.getElementById('sysTime').textContent =
                now.getFullYear() + '-' + pad(now.getMonth()+1) + '-' + pad(now.getDate()) + ' '
                + pad(now.getHours()) + ':' + pad(now.getMinutes()) + ':' + pad(now.getSeconds());
        }
        updateSysTime();
        setInterval(updateSysTime, 1000);

        async function remoteUnlock() {
            if (!confirm('确定远程开锁？')) return;
            await fetch('/api/unlock');
            alert('开锁指令已发送');
        }

        async function exportCSV() {
            window.location.href = '/api/export/csv';
        }

        async function saveNotify() {
            const key = document.getElementById('notifyKey').value.trim();
            const msg = document.getElementById('notifyMsg');
            const status = document.getElementById('notifyStatus');
            await fetch('/api/notify-settings', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({serverchan_key: key})
            });
            msg.style.display = 'block';
            msg.style.background = 'rgba(46,213,115,0.1)';
            msg.style.color = '#2ed573';
            msg.textContent = '✅ 通知配置已保存';
            if (key) {
                status.textContent = '已配置';
                status.style.background = '#2ed573';
                status.style.color = '#fff';
            } else {
                status.textContent = '未配置';
                status.style.background = '#333';
                status.style.color = '#888';
            }
        }

        async function testNotify() {
            const res = await fetch('/api/notify-test');
            const data = await res.json();
            const msg = document.getElementById('notifyMsg');
            msg.style.display = 'block';
            if (data.success) {
                msg.style.background = 'rgba(46,213,115,0.1)';
                msg.style.color = '#2ed573';
                msg.textContent = '✅ 测试通知已发送，请检查微信';
            } else {
                msg.style.background = 'rgba(255,71,87,0.1)';
                msg.style.color = '#ff4757';
                msg.textContent = '❌ 发送失败，请检查 SendKey 是否正确';
            }
        }

        function saveNotifyEvents() {
            const events = [];
            if (document.getElementById('evtSuccess').checked) events.push('success');
            if (document.getElementById('evtFail').checked) events.push('fail');
            if (document.getElementById('evtRemote').checked) events.push('remote');
            if (document.getElementById('evtCapture').checked) events.push('capture');
            fetch('/api/notify-settings', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({notify_events: events.join(',')})
            });
        }

        loadRecords();
        refreshStatus();
        setInterval(refreshStatus, 5000);
    </script>
</body>
</html>
'''

@app.route('/')
def index():
    return redirect(url_for('login_page'))

@app.route('/login', methods=['GET', 'POST'])
def login_page():
    error = None
    if request.method == 'POST':
        username = request.form.get('username', '')
        password = request.form.get('password', '')
        if username == ADMIN_USER and check_password_hash(ADMIN_PASS_HASH, password):
            session['logged_in'] = True
            return redirect(url_for('admin_page'))
        error = '用户名或密码错误'
    return render_template_string(LOGIN_HTML, error=error)

@app.route('/logout')
def logout():
    session.pop('logged_in', None)
    return redirect(url_for('login_page'))

@app.route('/admin')
@login_required
def admin_page():
    return render_template_string(ADMIN_HTML)

@app.route('/upload', methods=['POST'])
def upload():
    if 'file' not in request.files:
        return jsonify({'error': 'No file part'}), 400
    file = request.files['file']
    if file.filename == '':
        return jsonify({'error': 'No selected file'}), 400
    if file:
        timestamp = int(time.time())
        filename = f"{timestamp}.jpg"
        filepath = os.path.join(UPLOAD_FOLDER, filename)
        file.save(filepath)
        method = request.form.get('method', 'UNKNOWN')
        result = request.form.get('result', 'UNKNOWN')
        size = os.path.getsize(filepath)
        insert_log(filename, timestamp, method, result, size)
        print(f"[UPLOAD] {filename} | {method} | {result}")
        t = time.strftime('%H:%M:%S', time.localtime(timestamp))
        method_cn = {'RFID': 'RFID刷卡', '指纹': '指纹识别', '密码': '密码输入', '蓝牙': '蓝牙开锁'}.get(method, method)
        result_cn = '成功' if result == 'SUCCESS' else '失败'
        evt = 'success' if result == 'SUCCESS' else 'fail'
        send_notification(f'门禁{result_cn}', f'{t} {method_cn} {result_cn}', evt)
        return jsonify({'success': True, 'filename': filename}), 200
    return jsonify({'error': 'Upload failed'}), 500

@app.route('/photos')
def list_photos():
    return jsonify(get_all_logs())

@app.route('/photo/<filename>')
def get_photo(filename):
    return send_from_directory(UPLOAD_FOLDER, filename)

@app.route('/delete/<filename>', methods=['DELETE'])
def delete_photo(filename):
    filepath = os.path.join(UPLOAD_FOLDER, filename)
    if os.path.exists(filepath):
        os.remove(filepath)
    delete_log(filename)
    return jsonify({'success': True}), 200

@app.route('/delete-all', methods=['DELETE'])
def delete_all():
    for f in os.listdir(UPLOAD_FOLDER):
        if f.endswith('.jpg') or f.endswith('.jpeg'):
            os.remove(os.path.join(UPLOAD_FOLDER, f))
    delete_all_logs()
    return jsonify({'success': True}), 200

@app.route('/api/whitelist')
@login_required
def api_whitelist():
    return jsonify(get_whitelist())

@app.route('/api/whitelist', methods=['POST'])
@login_required
def api_add_whitelist():
    data = request.json
    add_whitelist(data['type'], data['value'], data.get('name', ''))
    return jsonify({'success': True}), 200

@app.route('/api/whitelist/<int:id>', methods=['DELETE'])
@login_required
def api_del_whitelist(id):
    delete_whitelist(id)
    return jsonify({'success': True}), 200

@app.route('/api/blacklist')
@login_required
def api_blacklist():
    return jsonify(get_blacklist())

@app.route('/api/blacklist', methods=['POST'])
@login_required
def api_add_blacklist():
    data = request.json
    add_blacklist(data['type'], data['value'], data.get('name', ''))
    return jsonify({'success': True}), 200

@app.route('/api/blacklist/<int:id>', methods=['DELETE'])
@login_required
def api_del_blacklist(id):
    delete_blacklist(id)
    return jsonify({'success': True}), 200

@app.route('/api/alert', methods=['POST'])
def api_alert():
    data = request.json
    if data and data.get('type') == 'brute_force':
        count = data.get('count', '?')
        send_notification('暴力破解告警', f'密码连续错误{count}次，系统已锁定30秒！请检查是否有人试图入侵。', 'fail')
        print(f"[ALERT] 暴力破解告警: {count}次连续错误")
    return jsonify({'success': True}), 200

@app.route('/api/log', methods=['POST'])
def api_log():
    data = request.json
    if data:
        method = data.get('method', 'UNKNOWN')
        result = data.get('result', 'UNKNOWN')
        ts = data.get('timestamp', int(time.time()))
        insert_log('offline.jpg', ts, method, result, 0)
        print(f"[LOG] 离线记录补传: {method} {result}")
    return jsonify({'success': True}), 200

@app.route('/api/status')
def api_status():
    return jsonify(get_device_status())

@app.route('/api/status', methods=['POST'])
def api_update_status():
    data = request.json
    if 'lock_state' in data:
        update_device_status(data['lock_state'])
    if 'cam_online' in data:
        update_cam_online(data['cam_online'])
    return jsonify({'success': True}), 200

@app.route('/api/config')
def api_get_config():
    return jsonify(get_config_for_esp32())

@app.route('/api/config', methods=['POST'])
@login_required
def api_set_config():
    data = request.json
    if 'door_password' in data:
        set_setting('door_password', data['door_password'])
        bump_config_version()
    return jsonify({'success': True, 'config': get_config_for_esp32()}), 200

@app.route('/api/unlock')
@login_required
def api_unlock():
    set_setting('unlock_request', '1')
    send_notification('远程开锁', '管理员通过网页触发了远程开锁', 'remote')
    return jsonify({'success': True}), 200

@app.route('/api/unlock/done')
def api_unlock_done():
    set_setting('unlock_request', '0')
    return jsonify({'success': True}), 200

@app.route('/api/notify-settings', methods=['GET'])
@login_required
def api_get_notify_settings():
    return jsonify({
        'serverchan_key': get_setting('serverchan_key') or '',
        'notify_events': get_setting('notify_events') or 'success,fail,remote'
    }), 200

@app.route('/api/notify-settings', methods=['POST'])
@login_required
def api_notify_settings():
    data = request.json
    if 'serverchan_key' in data:
        set_setting('serverchan_key', data['serverchan_key'])
    if 'notify_events' in data:
        set_setting('notify_events', data['notify_events'])
    return jsonify({'success': True}), 200

@app.route('/api/notify-test')
@login_required
def api_notify_test():
    send_notification('测试通知', '门禁系统通知测试成功！')
    return jsonify({'success': True}), 200

@app.route('/api/export/csv')
@login_required
def api_export_csv():
    import io
    output = io.StringIO()
    output.write('\ufeff时间,验证方式,结果,文件名,大小\n')
    logs = get_all_logs()
    for log in logs:
        t = time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(log['timestamp']))
        method = log['method']
        result = '成功' if log['result'] == 'SUCCESS' else '失败'
        output.write(f'"{t}",{method},{result},{log["filename"]},{log["size"]}\n')
    from flask import Response
    return Response(output.getvalue(), mimetype='text/csv',
                    headers={'Content-Disposition': 'attachment; filename=access_logs.csv'})

if __name__ == '__main__':
    print("=" * 50)
    print("  智能门禁管理系统 v2.1")
    print("=" * 50)
    print(f"  访问地址: http://127.0.0.1:8000")
    print(f"  管理登录: admin / 123456")
    print(f"  数据库:   {DB_FILE}")
    print(f"  照片目录: {UPLOAD_FOLDER}/")
    print("=" * 50)
    app.run(host='0.0.0.0', port=8000, debug=True)
