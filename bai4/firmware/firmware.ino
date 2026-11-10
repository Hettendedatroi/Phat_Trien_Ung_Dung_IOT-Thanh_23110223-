/*
 * CSE3069 - Bài 4: Gửi dữ liệu qua MQTT (FreeRTOS: task + queue)
 * ESP32-DevKitC-32E + A4950 (cầu H) + bơm DC + DHT + (tuỳ chọn) cảm biến mưa
 *
 * ESP32 KHÔNG còn web server / WebSocket. Giao diện chạy trên laptop (thư mục dashboard/)
 * và nói chuyện với ESP32 chỉ qua MQTT.
 *
 * KIẾN TRÚC
 *   sensorTask (core 1, ưu tiên 2): đọc DHT/mưa mỗi 2 s -> cập nhật snapshot cho FSM
 *                                   -> đẩy bản ghi vào telemetryQ
 *   mqttTask   (core 0, ưu tiên 1): giữ kết nối broker; lấy bản ghi từ telemetryQ và publish;
 *                                   publish state (1 s) và log; nhận lệnh từ .../cmd.
 *                                   MỌI việc có thể bị treo do mạng đều nằm ở task này.
 *   safetyTask (core 1, ưu tiên 3): FSM bơm DỪNG/CHẠY/LỖI, chu kỳ 20 ms bằng vTaskDelayUntil,
 *                                   KHÔNG gọi hàm mạng, KHÔNG chờ vô hạn.
 *   loop()     (core 1, ưu tiên 1): chỉ đọc lệnh từ Serial.
 *
 *   Queue:
 *     telemetryQ : 10 bản ghi. Khi đầy -> BỎ BẢN GHI CŨ NHẤT, giữ bản mới (đếm qDropped).
 *     cmdQ       : 8 lệnh (Serial / MQTT -> safetyTask). Khi đầy -> bỏ lệnh mới + log.
 *
 * Topic:
 *   nhom01/node01/telemetry  (ESP32 -> broker)  số đo, mỗi 2 s
 *   nhom01/node01/state      (ESP32 -> broker)  trạng thái FSM + queue, mỗi 1 s
 *   nhom01/node01/log        (ESP32 -> broker)  từng dòng log
 *   nhom01/node01/status     (LWT, retained)    online / offline
 *   nhom01/node01/cmd        (broker -> ESP32)  lệnh
 *
 * Lệnh (Serial 115200 hoặc topic cmd):
 *   ON | OFF | PWM <1-100> | STATUS | SLOW <ms>  (SLOW: làm chậm task MQTT để thử queue đầy)
 *   Lệnh khác khi đang CHAY -> LOI
 */

#include <Arduino.h>
#include <stdarg.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

#include "secrets.h"    // WIFI_SSID, WIFI_PASS, MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS

// ===================== KIỂU DỮ LIỆU (khai báo trước hàm đầu tiên) =====================
enum State { ST_STOP, ST_RUN, ST_FAULT };

struct Telemetry {          // 1 bản ghi trong telemetryQ
  uint32_t ts;              // millis() lúc đọc
  uint32_t seq;             // số thứ tự bản tin (để phát hiện rớt khi đối chiếu log)
  float    temp, hum;
  int16_t  rain;            // < 0 = không có cảm biến -> null
  bool     valid;
  uint8_t  state;           // trạng thái FSM lúc đọc
};

struct Cmd {                // 1 lệnh trong cmdQ
  char text[40];
  bool bad;                 // true = đầu vào rác/quá dài
};

struct SensorSnap { bool valid; float t, h; };

// ===================== CẤU HÌNH =====================
constexpr uint32_t WIFI_TIMEOUT_MS = 10000;

const char* DEVICE_ID       = "node01";
const char* TOPIC_TELEMETRY = "nhom01/node01/telemetry";
const char* TOPIC_STATE     = "nhom01/node01/state";
const char* TOPIC_LOG       = "nhom01/node01/log";
const char* TOPIC_STATUS    = "nhom01/node01/status";
const char* TOPIC_CMD       = "nhom01/node01/cmd";

constexpr uint8_t  PIN_PUMP_IN1 = 23;      // PWM
constexpr uint8_t  PIN_PUMP_IN2 = 22;      // luôn LOW
constexpr uint8_t  PIN_DHT      = 14;
constexpr int8_t   PIN_RAIN     = -1;      // ĐIỀN chân ADC cảm biến mưa; -1 = chưa gắn -> "rain": null
#define DHTTYPE DHT22

constexpr uint32_t MAX_RUN_MS       = 20000;
constexpr uint32_t FAULT_HOLD_MS    = 3000;
constexpr uint32_t COOLDOWN_MS      = 5000;
constexpr uint32_t SENSOR_PERIOD_MS = 2000;    // = chu kỳ gửi MQTT (DHT cần >= 2 s)
constexpr uint32_t SAFETY_PERIOD_MS = 20;      // chu kỳ kiểm tra an toàn, độc lập MQTT
constexpr uint32_t HEARTBEAT_MS     = 2000;    // log nhịp task an toàn
constexpr uint32_t STATE_PERIOD_MS  = 1000;    // publish state lên dashboard
constexpr uint32_t MQTT_RETRY_MS    = 3000;

constexpr uint32_t PWM_FREQ_HZ      = 20000;
constexpr uint8_t  PWM_BITS         = 8;
constexpr uint8_t  DEFAULT_DUTY_PCT = 90;

constexpr int TELEMETRY_Q_LEN = 10;
constexpr int CMD_Q_LEN       = 8;
constexpr uint32_t MQTT_SLOW_MAX_MS = 8000;    // < keepalive (10 s) để không rớt kết nối khi thử tải

// ===================== BIẾN TOÀN CỤC =====================
const char* stateName(State s) {
  return s == ST_STOP ? "DUNG" : (s == ST_RUN ? "CHAY" : "LOI");
}

// --- Chỉ safetyTask ghi các biến này ---
volatile State   state = ST_STOP;
volatile uint32_t stateSince = 0;
volatile uint32_t lastRunEndMs = 0;
volatile uint8_t dutyPct = DEFAULT_DUTY_PCT;
bool evStart = false, evStop = false, evBadInput = false;

// --- Snapshot cảm biến: sensorTask ghi, safetyTask/mqttTask đọc (spinlock) ---
portMUX_TYPE sensMux = portMUX_INITIALIZER_UNLOCKED;
SensorSnap snap = { false, NAN, NAN };

// --- Queue / thống kê ---
QueueHandle_t telemetryQ = nullptr;
QueueHandle_t cmdQ = nullptr;
volatile uint32_t qSent = 0, qDropped = 0, qPubFail = 0, safetyBeats = 0;
volatile uint32_t mqttExtraDelayMs = 0;       // lệnh SLOW
volatile bool     mqttUp = false;

DHT dht(PIN_DHT, DHTTYPE);
WiFiClient   netClient;
PubSubClient mqtt(netClient);

// --- Log: nhiều task ghi (có mutex), mqttTask đọc và publish ---
constexpr int LOG_LINES = 80;
constexpr int LOG_LEN   = 128;
char logRing[LOG_LINES][LOG_LEN];
volatile uint32_t logTotal = 0;     // tổng số dòng đã ghi
uint32_t mqttLogSeq = 0;            // số dòng đã publish (chỉ mqttTask dùng)
SemaphoreHandle_t logMux = nullptr;

void logMsg(const char* fmt, ...) {
  char body[100];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  char line[LOG_LEN];
  snprintf(line, sizeof line, "[%lu] %s", (unsigned long)millis(), body);
  if (xSemaphoreTake(logMux, pdMS_TO_TICKS(50)) != pdTRUE) return;   // không chờ vô hạn
  Serial.println(line);
  strlcpy(logRing[logTotal % LOG_LINES], line, LOG_LEN);
  logTotal = logTotal + 1;
  xSemaphoreGive(logMux);
}

// Chỉ gọi từ mqttTask khi đang kết nối: publish các dòng log mới
void flushLogToMqtt() {
  for (int n = 0; n < 6; n++) {
    char line[LOG_LEN];
    bool has;
    if (xSemaphoreTake(logMux, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (logTotal - mqttLogSeq > (uint32_t)LOG_LINES) mqttLogSeq = logTotal - LOG_LINES;  // tụt quá xa -> nhảy
    has = (mqttLogSeq != logTotal);
    if (has) { strlcpy(line, logRing[mqttLogSeq % LOG_LINES], LOG_LEN); mqttLogSeq++; }
    xSemaphoreGive(logMux);
    if (!has) break;
    mqtt.publish(TOPIC_LOG, line);
  }
}

// ===================== GỬI LỆNH VÀO cmdQ (không chặn) =====================
void submitCommand(const char* text, bool bad) {
  Cmd c;
  memset(&c, 0, sizeof c);
  strlcpy(c.text, text, sizeof c.text);
  c.bad = bad;
  if (xQueueSend(cmdQ, &c, 0) != pdTRUE) logMsg("LOI: hang doi lenh day, bo lenh");
}

// ===================== ĐIỀU KHIỂN TẢI (chỉ safetyTask / setup) =====================
void pumpOff() {
  ledcWrite(PIN_PUMP_IN1, 0);
  digitalWrite(PIN_PUMP_IN2, LOW);
}

void pumpOn() {
  digitalWrite(PIN_PUMP_IN2, LOW);
  ledcWrite(PIN_PUMP_IN1, (uint32_t)dutyPct * ((1u << PWM_BITS) - 1) / 100);
}

// ===================== FSM =====================
void enterStop(const char* reason) {
  pumpOff();
  state = ST_STOP;
  stateSince = millis();
  lastRunEndMs = stateSince;
  logMsg("-> DUNG (%s)", reason);
}

void enterFault(const char* reason) {
  pumpOff();
  state = ST_FAULT;
  stateSince = millis();
  logMsg("-> LOI (%s)", reason);
}

void enterRun() {
  state = ST_RUN;
  stateSince = millis();
  pumpOn();
  logMsg("-> CHAY (duty=%u%%, toi da %lu s)", dutyPct, (unsigned long)(MAX_RUN_MS / 1000));
}

void updateState() {
  uint32_t now = millis();
  bool sv;
  portENTER_CRITICAL(&sensMux); sv = snap.valid; portEXIT_CRITICAL(&sensMux);

  switch (state) {
    case ST_RUN:
      if (evStart) logMsg("TU CHOI: dang CHAY, khong gia han thoi gian");
      if (evBadInput)                           enterFault("lenh sai dinh dang");
      else if (!sv)                             enterFault("cam bien NaN");
      else if (now - stateSince >= MAX_RUN_MS) enterStop("het gio");
      else if (evStop)                          enterStop("lenh OFF");
      break;

    case ST_STOP:
      if (evStart) {
        uint32_t since = now - lastRunEndMs;
        if (lastRunEndMs != 0 && since < COOLDOWN_MS)
          logMsg("TU CHOI: dang cooldown, con %lu ms", (unsigned long)(COOLDOWN_MS - since));
        else if (!sv)
          logMsg("TU CHOI: cam bien chua hop le, khong bat tai");
        else
          enterRun();
      }
      break;

    case ST_FAULT:
      if (evStart) logMsg("TU CHOI: dang LOI, con %lu ms moi ve DUNG",
                          (unsigned long)(FAULT_HOLD_MS - (now - stateSince)));
      if (now - stateSince >= FAULT_HOLD_MS) enterStop("het loi, ve trang thai an toan");
      break;
  }

  if (state != ST_RUN) pumpOff();           // phòng thủ
  evStart = evStop = evBadInput = false;
}

// ===================== XỬ LÝ LỆNH (chạy trong safetyTask) =====================
void applyCommand(const Cmd& c) {
  if (c.bad) { logMsg("LOI: dau vao qua dai/rac"); evBadInput = true; return; }
  String cmd(c.text);
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0) return;
  logMsg("CMD: %s", cmd.c_str());

  if (cmd == "ON") {
    evStart = true;
  } else if (cmd == "OFF") {
    evStop = true;
  } else if (cmd == "STATUS") {
    logMsg("STATE=%s duty=%u%% mqtt=%d queue=%u/%d sent=%lu drop=%lu pubfail=%lu",
           stateName(state), dutyPct, mqttUp ? 1 : 0,
           (unsigned)uxQueueMessagesWaiting(telemetryQ), TELEMETRY_Q_LEN,
           (unsigned long)qSent, (unsigned long)qDropped, (unsigned long)qPubFail);
  } else if (cmd.startsWith("SLOW ")) {
    long v = cmd.substring(5).toInt();
    if (v < 0) v = 0;
    if (v > (long)MQTT_SLOW_MAX_MS) v = MQTT_SLOW_MAX_MS;
    mqttExtraDelayMs = (uint32_t)v;
    logMsg("THU TAI: task MQTT cham them %ld ms moi vong", v);
  } else if (cmd.startsWith("PWM ")) {
    int v = cmd.substring(4).toInt();
    if (v >= 1 && v <= 100) {
      dutyPct = v;
      logMsg("Duty = %u%%", dutyPct);
      if (state == ST_RUN) pumpOn();
    } else {
      logMsg("LOI: PWM phai tu 1 den 100");
      evBadInput = true;
    }
  } else {
    logMsg("LOI: lenh khong hop le");
    evBadInput = true;
  }
}

// ===================== TASK AN TOÀN =====================
// Không có hàm mạng, không chờ vô hạn: chu kỳ cố định 20 ms bằng vTaskDelayUntil.
void safetyTask(void*) {
  TickType_t last = xTaskGetTickCount();
  uint32_t lastHb = 0;
  for (;;) {
    Cmd c;
    while (xQueueReceive(cmdQ, &c, 0) == pdTRUE) applyCommand(c);   // timeout 0 = không chờ
    updateState();
    safetyBeats = safetyBeats + 1;

    uint32_t now = millis();
    if (now - lastHb >= HEARTBEAT_MS) {
      lastHb = now;
      logMsg("HB: safety beat=%lu state=%s mqtt=%s queue=%u/%d drop=%lu",
             (unsigned long)safetyBeats, stateName(state), mqttUp ? "UP" : "DOWN",
             (unsigned)uxQueueMessagesWaiting(telemetryQ), TELEMETRY_Q_LEN,
             (unsigned long)qDropped);
    }
    vTaskDelayUntil(&last, pdMS_TO_TICKS(SAFETY_PERIOD_MS));
  }
}

// ===================== TASK CẢM BIẾN =====================
// Chính sách khi queue đầy: BỎ BẢN GHI CŨ NHẤT, giữ bản mới (dữ liệu mới có giá trị hơn).
void enqueueTelemetry(const Telemetry& r) {
  if (xQueueSend(telemetryQ, &r, 0) == pdTRUE) return;
  Telemetry old;
  xQueueReceive(telemetryQ, &old, 0);              // bỏ bản cũ nhất
  qDropped = qDropped + 1;
  logMsg("DROP: queue day, bo ban ghi seq=%lu (cu nhat)", (unsigned long)old.seq);
  if (xQueueSend(telemetryQ, &r, 0) != pdTRUE) qDropped = qDropped + 1;
}

void sensorTask(void*) {
  uint32_t seq = 0;
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    bool ok = !(isnan(h) || isnan(t));

    portENTER_CRITICAL(&sensMux);
    snap.valid = ok;
    if (ok) { snap.t = t; snap.h = h; }
    portEXIT_CRITICAL(&sensMux);

    if (ok) logMsg("DHT: T=%.1fC H=%.1f%%", t, h);
    else    logMsg("DHT: NaN (cam bien loi/rut day)");

    Telemetry r;
    memset(&r, 0, sizeof r);
    r.ts = millis();
    r.seq = ++seq;
    r.valid = ok;
    r.temp = ok ? t : 0;
    r.hum  = ok ? h : 0;
    r.rain = (PIN_RAIN >= 0) ? (int16_t)analogRead(PIN_RAIN) : -1;
    r.state = (uint8_t)state;
    enqueueTelemetry(r);

    vTaskDelayUntil(&last, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
  }
}

// ===================== TASK MQTT =====================
// Giá trị thiếu = null (phân biệt với 0 hợp lệ)
int buildPayload(char* o, size_t n, const Telemetry& r) {
  char tb[16], hb[16], rb[16];
  if (r.valid) { snprintf(tb, sizeof tb, "%.1f", r.temp); snprintf(hb, sizeof hb, "%.1f", r.hum); }
  else         { strcpy(tb, "null"); strcpy(hb, "null"); }
  if (r.rain >= 0) snprintf(rb, sizeof rb, "%d", r.rain); else strcpy(rb, "null");
  return snprintf(o, n,
    "{\"device_id\":\"%s\",\"ts\":%lu,\"seq\":%lu,\"temp\":%s,\"hum\":%s,\"rain\":%s,"
    "\"unit\":\"C\",\"valid\":%s,\"state\":\"%s\"}",
    DEVICE_ID, (unsigned long)r.ts, (unsigned long)r.seq, tb, hb, rb,
    r.valid ? "true" : "false", stateName((State)r.state));
}

// Trạng thái FSM + queue cho dashboard
void publishState() {
  uint32_t now = millis();
  State st = state;
  uint32_t rem = 0;
  if (st == ST_RUN) {
    uint32_t el = now - stateSince;
    rem = el >= MAX_RUN_MS ? 0 : MAX_RUN_MS - el;
  }
  uint32_t cd = 0;
  uint32_t lre = lastRunEndMs;
  if (st == ST_STOP && lre != 0 && now - lre < COOLDOWN_MS) cd = COOLDOWN_MS - (now - lre);

  SensorSnap s;
  portENTER_CRITICAL(&sensMux); s = snap; portEXIT_CRITICAL(&sensMux);

  char buf[420];
  snprintf(buf, sizeof buf,
    "{\"st\":%d,\"duty\":%u,\"rem\":%lu,\"max\":%lu,\"cd\":%lu,\"cdmax\":%lu,"
    "\"sv\":%d,\"t\":%.1f,\"h\":%.1f,\"up\":%lu,\"rssi\":%d,"
    "\"qn\":%u,\"qmax\":%d,\"qsent\":%lu,\"qdrop\":%lu,\"slow\":%lu,\"beat\":%lu}",
    (int)st, (unsigned)dutyPct, (unsigned long)rem, (unsigned long)MAX_RUN_MS,
    (unsigned long)cd, (unsigned long)COOLDOWN_MS,
    s.valid ? 1 : 0, s.valid ? s.t : 0.0f, s.valid ? s.h : 0.0f,
    (unsigned long)now, (int)WiFi.RSSI(),
    (unsigned)uxQueueMessagesWaiting(telemetryQ), TELEMETRY_Q_LEN,
    (unsigned long)qSent, (unsigned long)qDropped,
    (unsigned long)mqttExtraDelayMs, (unsigned long)safetyBeats);
  mqtt.publish(TOPIC_STATE, buf);
}

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  char buf[40];
  if (len >= sizeof buf) { submitCommand("", true); return; }
  memcpy(buf, payload, len);
  buf[len] = 0;
  submitCommand(buf, false);          // đi qua cmdQ -> FSM, không bỏ qua kiểm tra an toàn
}

void mqttTask(void*) {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(10);
  mqtt.setSocketTimeout(3);
  uint32_t lastTry = 0, lastState = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      mqttUp = false;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!mqtt.connected()) {
      if (mqttUp) { mqttUp = false; logMsg("MQTT: mat ket noi (state=%d)", mqtt.state()); }
      if (millis() - lastTry >= MQTT_RETRY_MS) {
        lastTry = millis();
        logMsg("MQTT: dang ket noi %s:%d ...", MQTT_HOST, MQTT_PORT);
        String cid = String("esp32-") + DEVICE_ID + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
        // Có thể treo vài giây nếu mất mạng — chỉ treo task này, safetyTask vẫn chạy
        if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS, TOPIC_STATUS, 1, true, "offline")) {
          mqtt.publish(TOPIC_STATUS, "online", true);
          mqtt.subscribe(TOPIC_CMD, 1);
          mqttUp = true;
          logMsg("MQTT: da ket noi, sub %s", TOPIC_CMD);
        } else {
          logMsg("MQTT: ket noi that bai, state=%d", mqtt.state());
        }
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    mqtt.loop();
    flushLogToMqtt();
    if (millis() - lastState >= STATE_PERIOD_MS) { lastState = millis(); publishState(); }

    Telemetry r;
    if (xQueueReceive(telemetryQ, &r, 0) == pdTRUE) {
      char buf[256];
      buildPayload(buf, sizeof buf, r);
      if (mqtt.publish(TOPIC_TELEMETRY, buf)) {
        qSent = qSent + 1;
      } else {
        qPubFail = qPubFail + 1;
        if (xQueueSendToFront(telemetryQ, &r, 0) != pdTRUE) qDropped = qDropped + 1;  // giữ lại để gửi sau
      }
    }

    // THU TAI: delay dài giả lập bên gửi chậm hơn bên đọc
    vTaskDelay(pdMS_TO_TICKS(mqttExtraDelayMs ? mqttExtraDelayMs : 20));
  }
}

// ===================== SERIAL =====================
void pollSerial() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf.length()) submitCommand(buf.c_str(), false);
      buf = "";
    } else if (buf.length() < 32) {
      buf += c;
    } else {
      buf = "";
      submitCommand("", true);
    }
  }
}

// ===================== WIFI =====================
void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Ket noi WiFi '%s'", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("WiFi OK, IP ESP32: %s\n", WiFi.localIP().toString().c_str());
  else
    Serial.println("WiFi chua ket noi duoc - se tu thu lai; bom van an toan, FSM van chay.");
}

// ===================== SETUP / LOOP =====================
void setup() {
  // 1. Đưa tải về trạng thái an toàn trước tiên
  pinMode(PIN_PUMP_IN1, OUTPUT);
  pinMode(PIN_PUMP_IN2, OUTPUT);
  digitalWrite(PIN_PUMP_IN1, LOW);
  digitalWrite(PIN_PUMP_IN2, LOW);

  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== TH04: Bom an toan + MQTT (FreeRTOS) ===");

  // 2. Tài nguyên FreeRTOS (tạo trước khi có task nào gọi logMsg)
  logMux     = xSemaphoreCreateMutex();
  telemetryQ = xQueueCreate(TELEMETRY_Q_LEN, sizeof(Telemetry));
  cmdQ       = xQueueCreate(CMD_Q_LEN, sizeof(Cmd));

  // 3. PWM + cảm biến
  ledcAttach(PIN_PUMP_IN1, PWM_FREQ_HZ, PWM_BITS);
  pumpOff();
  dht.begin();
  if (PIN_RAIN >= 0) pinMode(PIN_RAIN, INPUT);

  // 4. WiFi (bơm đã tắt nên chờ WiFi là an toàn)
  startWifi();

  stateSince = millis();
  logMsg("San sang. Trang thai: %s, bom TAT. Gioi han chay: %lu s",
         stateName(state), (unsigned long)(MAX_RUN_MS / 1000));
  logMsg("Lenh: ON | OFF | PWM <1-100> | STATUS | SLOW <ms>");

  // 5. Tạo task. xTaskCreatePinnedToCore = xTaskCreate + chỉ định lõi (stack tính bằng byte)
  xTaskCreatePinnedToCore(safetyTask, "safety", 4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(mqttTask,   "mqtt",   6144, nullptr, 1, nullptr, 0);
}

void loop() {   // chỉ còn đọc lệnh Serial
  pollSerial();
  delay(5);
}
