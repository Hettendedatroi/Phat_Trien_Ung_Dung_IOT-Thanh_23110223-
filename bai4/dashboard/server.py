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
import json
import os
import sys
import threading
import time
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

if PASSWORD is None:
    sys.exit("Thiếu biến môi trường MQTT_PASS (xem hướng dẫn ở đầu file).")

lock = threading.Lock()
data = {
    "broker_ok": False,      # dashboard <-> broker
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
    if ok:
        client.subscribe(BASE + "/#", 0)
        client.subscribe(BASE + "/status", 1)
        print("Đã kết nối broker", BROKER)
    else:
        print("Kết nối broker thất bại:", rc)


def on_disconnect(client, userdata, *args):
    with lock:
        data["broker_ok"] = False
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


try:   # paho-mqtt 2.x
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="dashboard-laptop")
except AttributeError:   # paho-mqtt 1.x
    client = mqtt.Client(client_id="dashboard-laptop")
client.username_pw_set(USER, PASSWORD)
client.on_connect = on_connect
client.on_disconnect = on_disconnect
client.on_message = on_message
client.reconnect_delay_set(1, 5)
client.connect_async(BROKER, PORT, keepalive=10)
client.loop_start()


class Handler(BaseHTTPRequestHandler):
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

    def do_GET(self):
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
                    "broker_ok": data["broker_ok"],
                    "node_status": data["node_status"],
                    "telemetry": data["telemetry"],
                    "telemetry_age": int((now - data["telemetry_rx"]) * 1000) if data["telemetry_rx"] else None,
                    "state": data["state"],
                    "state_age": int((now - data["state_rx"]) * 1000) if data["state_rx"] else None,
                    "logs": [[i, t] for i, t in logs if i > since],
                    "last_id": log_id,
                }
            self._send(200, json.dumps(out))
        else:
            self._send(404, "{}")

    def do_POST(self):
        if self.path != "/api/cmd":
            return self._send(404, "{}")
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
    print(f"Dashboard: http://{HTTP_HOST}:{HTTP_PORT}  (broker {BROKER}:{PORT})")
    ThreadingHTTPServer((HTTP_HOST, HTTP_PORT), Handler).serve_forever()
