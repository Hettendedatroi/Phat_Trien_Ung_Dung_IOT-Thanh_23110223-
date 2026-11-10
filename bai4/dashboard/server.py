"""
Dashboard chạy trên laptop: subscribe MQTT, phục vụ trang web index.html.
Không dùng WebSocket: trình duyệt hỏi /api/data mỗi ~0.7 s.

Chạy (PowerShell):
    pip install paho-mqtt
    $env:MQTT_HOST="192.168.x.x"      # IP máy chạy broker (hoặc localhost nếu cùng máy)
    $env:MQTT_USER="nhom01"
    $env:MQTT_PASS="mat_khau"          # không ghi mật khẩu vào file
    python server.py
Mở:  http://localhost:8080
"""
import base64
import hmac
import json
import os
import sys
import threading
import time
import uuid
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import paho.mqtt.client as mqtt

BROKER = os.environ.get("MQTT_HOST", "localhost")
PORT = int(os.environ.get("MQTT_PORT", "1883"))
USER = os.environ.get("MQTT_USER", "nhom01")
PASSWORD = os.environ.get("MQTT_PASS")
BASE = os.environ.get("MQTT_BASE", "nhom01/node01")
HTTP_HOST = os.environ.get("DASH_HOST", "127.0.0.1")   # chỉ máy này; đổi 0.0.0.0 nếu muốn máy khác vào
HTTP_PORT = int(os.environ.get("DASH_PORT", "8080"))
WEB_USER = os.environ.get("DASH_USER", "admin")      # đăng nhập trang web (Basic Auth)
WEB_PASS = os.environ.get("DASH_PASS") or None        # để trống = không yêu cầu mật khẩu
READONLY = os.environ.get("DASH_READONLY") == "1"     # 1 = chỉ xem, tắt nút điều khiển bơm

if PASSWORD is None:
    sys.exit("Thiếu biến môi trường MQTT_PASS (xem hướng dẫn ở đầu file).")

lock = threading.Lock()
data = {
    "broker_ok": False,      # dashboard <-> broker
    "broker_change": 0.0,    # thời điểm broker_ok đổi giá trị gần nhất
    "node_status": None,     # "online"/"offline" (LWT)
    "telemetry": None, "telemetry_rx": 0.0,
    "state": None, "state_rx": 0.0,
}
logs = deque(maxlen=1000)    # (id, text)
log_id = 0


def on_connect(client, userdata, flags, rc, *args):
    ok = (getattr(rc, "value", rc) == 0)
    with lock:
        data["broker_ok"] = ok
        data["broker_change"] = time.time()
    if ok:
        client.subscribe(BASE + "/#", 0)
        client.subscribe(BASE + "/status", 1)
        print("Đã kết nối broker", BROKER, "client_id =", CLIENT_ID)
    else:
        print("Kết nối broker thất bại:", rc)


def on_disconnect(client, userdata, *args):
    with lock:
        data["broker_ok"] = False
        data["broker_change"] = time.time()
    print("Mất kết nối broker, paho sẽ tự thử lại")


def on_message(client, userdata, msg):
    global log_id
    topic = msg.topic[len(BASE) + 1:]
    text = msg.payload.decode("utf-8", errors="replace")
    now = time.time()
    with lock:
        if topic == "status":
            data["node_status"] = text
        elif topic == "telemetry":
            try:
                data["telemetry"] = json.loads(text)
                data["telemetry_rx"] = now
            except ValueError:
                pass
        elif topic == "state":
            try:
                data["state"] = json.loads(text)
                data["state_rx"] = now
            except ValueError:
                pass
        elif topic == "log":
            log_id += 1
            logs.append((log_id, text))


# client_id PHẢI duy nhất: hai tiến trình trùng id sẽ đá nhau liên tục -> chập chờn
CLIENT_ID = "dashboard-" + uuid.uuid4().hex[:8]
try:   # paho-mqtt 2.x
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=CLIENT_ID)
except AttributeError:   # paho-mqtt 1.x
    client = mqtt.Client(client_id=CLIENT_ID)
client.username_pw_set(USER, PASSWORD)
client.on_connect = on_connect
client.on_disconnect = on_disconnect
client.on_message = on_message
client.reconnect_delay_set(1, 5)
client.connect_async(BROKER, PORT, keepalive=10)
client.loop_start()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"   # giữ kết nối, không mở TCP mới cho mỗi lần hỏi
    timeout = 30                    # dọn kết nối treo

    def log_message(self, *a):   # im lặng
        pass

    def _send(self, code, body, ctype="application/json"):
        raw = body if isinstance(body, bytes) else body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype + "; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def _authorized(self):
        if not WEB_PASS:
            return True
        h = self.headers.get("Authorization", "")
        if h.startswith("Basic "):
            try:
                u, _, p = base64.b64decode(h[6:]).decode("utf-8").partition(":")
            except Exception:
                return False
            return hmac.compare_digest(u, WEB_USER) and hmac.compare_digest(p, WEB_PASS)
        return False

    def _deny(self):
        self.send_response(401)
        self.send_header("WWW-Authenticate", 'Basic realm="Dashboard"')
        self.send_header("Content-Length", "0")
        self.end_headers()
        self.close_connection = True

    def do_GET(self):
        if not self._authorized():
            return self._deny()
        path, _, query = self.path.partition("?")
        if path == "/":
            with open(os.path.join(os.path.dirname(__file__), "index.html"), "rb") as f:
                self._send(200, f.read(), "text/html")
        elif path == "/api/data":
            since = 0
            for kv in query.split("&"):
                if kv.startswith("since="):
                    try:
                        since = int(kv[6:])
                    except ValueError:
                        pass
            now = time.time()
            with lock:
                out = {
                    "broker_ok": data["broker_ok"] or (now - data["broker_change"] < 3.0),
                    "node_status": data["node_status"],
                    "telemetry": data["telemetry"],
                    "telemetry_age": int((now - data["telemetry_rx"]) * 1000) if data["telemetry_rx"] else None,
                    "state": data["state"],
                    "state_age": int((now - data["state_rx"]) * 1000) if data["state_rx"] else None,
                    "logs": [[i, t] for i, t in logs if i > since],
                    "last_id": log_id,
                    "readonly": READONLY,
                }
            self._send(200, json.dumps(out))
        else:
            self._send(404, "{}")

    def do_POST(self):
        if not self._authorized():
            return self._deny()
        if self.path != "/api/cmd":
            return self._send(404, "{}")
        if READONLY:
            return self._send(403, json.dumps({"ok": False, "error": "readonly"}))
        n = int(self.headers.get("Content-Length", "0"))
        try:
            cmd = json.loads(self.rfile.read(n)).get("cmd", "")
        except ValueError:
            cmd = ""
        cmd = str(cmd).strip()[:32]
        if not cmd or not client.is_connected():
            return self._send(503, json.dumps({"ok": False}))
        client.publish(BASE + "/cmd", cmd, qos=1)
        self._send(200, json.dumps({"ok": True}))


if __name__ == "__main__":
    print(f"Dashboard: http://{HTTP_HOST}:{HTTP_PORT}  (broker {BROKER}:{PORT})"
          f"  auth={'on' if WEB_PASS else 'OFF'}  readonly={READONLY}")
    ThreadingHTTPServer((HTTP_HOST, HTTP_PORT), Handler).serve_forever()
