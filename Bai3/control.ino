/*
 * CSE3069 - Bài 3: Điều khiển thiết bị an toàn
 * ESP32-DevKitC-32E + A4950 (cầu H) + máy bơm DC (DC MOTOR 2)
 * + Giao diện web (WebSocket): theo dõi FSM, chỉnh duty, xem log
 *
 * Đường điều khiển: GPIO23 (IN1, PWM) / GPIO22 (IN2, luôn LOW) -> A4950 -> bơm DC
 * Trạng thái an toàn: IN1 = IN2 = LOW (bơm tắt)
 *
 * Máy trạng thái: DUNG -> CHAY -> (hết giờ) DUNG / (lỗi) LOI -> DUNG
 * Cooldown: sau mỗi lần vào DUNG, lệnh ON bị TỪ CHỐI (có log) trong COOLDOWN_MS
 * So sánh: bảng số liệu chu kỳ timer phần cứng vs millis() (web + log; lệnh CMP, CMPRESET)
 * Lệnh (Serial 115200 hoặc từ web, dùng chung một đường xử lý):
 *   ON | OFF | PWM <1-100> | STATUS | CMP | CMPRESET
 *   Lệnh khác khi đang CHAY -> LOI


#include <Arduino.h>
#include <stdarg.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WebSocketsServer.h>
#include <DHT.h>

// ===================== CẤU HÌNH =====================
const char* WIFI_SSID = "";        // <-- đổi thành WiFi của bạn
const char* WIFI_PASS = "";        // <-- đổi
const char* AP_SSID   = "ESP32-Pump";      // phát WiFi riêng nếu không kết nối được
const char* AP_PASS   = "12345678";
constexpr uint32_t WIFI_TIMEOUT_MS = 10000;

constexpr uint8_t  PIN_PUMP_IN1 = 23;      // MOTOR2_INT1 (PWM)
constexpr uint8_t  PIN_PUMP_IN2 = 22;      // MOTOR2_INT2 (luôn LOW)
constexpr uint8_t  PIN_DHT      = 14;      // DATA_RH
#define DHTTYPE DHT22                      // đổi thành DHT11 nếu dùng DHT11

constexpr uint32_t MAX_RUN_MS        = 20000;  // giới hạn chạy tối đa (20 s)
constexpr uint32_t FAULT_HOLD_MS     = 3000;   // giữ LỖI 3 s rồi về DỪNG
constexpr uint32_t COOLDOWN_MS       = 5000;   // nghỉ 5 s sau mỗi lần về DỪNG mới cho ON lại
constexpr uint32_t SENSOR_PERIOD_MS  = 2000;   // DHT cần >= 2 s giữa 2 lần đọc

constexpr uint32_t PWM_FREQ_HZ       = 20000;
constexpr uint8_t  PWM_BITS          = 8;
constexpr uint8_t  DEFAULT_DUTY_PCT  = 90;

constexpr uint32_t TIMER_PERIOD_US   = 100000; // timer phần cứng: 100 ms
constexpr uint32_t TIMER_REPORT_MS   = 30000;  // tự in bảng so sánh ra log mỗi 30 s
constexpr uint32_t MILLIS_PERIOD_MS  = TIMER_PERIOD_US / 1000;  // millis() cùng chu kỳ 100 ms
constexpr uint32_t STATUS_PERIOD_MS  = 500;    // đẩy trạng thái lên web

// ===================== GIAO DIỆN WEB =====================
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 - Điều khiển bơm an toàn</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--tx:#1a1d21;--mut:#6b7280;--bd:#e5e7eb;--run:#2563eb;--err:#dc2626;--stop:#6b7280;--ok:#16a34a}
@media(prefers-color-scheme:dark){:root{--bg:#0f1216;--card:#181c22;--tx:#e8eaed;--mut:#9aa3af;--bd:#2a3039}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.45 system-ui,sans-serif}
.wrap{max-width:900px;margin:0 auto;padding:14px}
h1{font-size:18px;margin:0 0 12px;display:flex;align-items:center;gap:8px}
.dot{width:10px;height:10px;border-radius:50%;background:var(--err)}
.dot.ok{background:var(--ok)}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media(max-width:640px){.grid{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--bd);border-radius:12px;padding:14px}
.card h2{font-size:12px;letter-spacing:.06em;text-transform:uppercase;color:var(--mut);margin:0 0 10px}
.full{grid-column:1/-1}
.fsm{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.pill{padding:6px 14px;border-radius:999px;border:1px solid var(--bd);color:var(--mut);font-weight:600}
.pill.on.s0{background:var(--stop);color:#fff;border-color:var(--stop)}
.pill.on.s1{background:var(--run);color:#fff;border-color:var(--run)}
.pill.on.s2{background:var(--err);color:#fff;border-color:var(--err)}
.arrow{color:var(--mut)}
.bar{height:8px;background:var(--bd);border-radius:4px;overflow:hidden;margin:12px 0 6px}
.bar>div{height:100%;width:0;background:var(--run);transition:width .4s linear}
.row{display:flex;justify-content:space-between;color:var(--mut);font-size:13px}
.big{font-size:22px;font-weight:700;color:var(--tx)}
button{font:inherit;padding:8px 14px;border-radius:8px;border:1px solid var(--bd);background:var(--card);color:var(--tx);cursor:pointer}
button:active{transform:scale(.97)}
.b-on{background:var(--run);border-color:var(--run);color:#fff}
.b-off{background:var(--err);border-color:var(--err);color:#fff}
.btns{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}
input[type=range]{width:100%}
.kv{display:grid;grid-template-columns:auto 1fr;gap:4px 12px}
.kv span:nth-child(odd){color:var(--mut)}
#log{height:280px;overflow:auto;background:#0b0d10;color:#c9d1d9;border-radius:8px;padding:10px;font:12px/1.5 ui-monospace,Consolas,monospace;white-space:pre-wrap;word-break:break-all}
#log .e{color:#ff7b72}#log .w{color:#e3b341}#log .c{color:#79c0ff}
.cmd{display:flex;gap:8px;margin-top:8px}
.cmd input{flex:1;font:inherit;padding:8px;border-radius:8px;border:1px solid var(--bd);background:var(--card);color:var(--tx)}
.lg{font-size:13px;color:var(--mut);margin:6px 0}
.lg i{display:inline-block;width:10px;height:10px;border-radius:2px;margin:0 4px 0 12px;vertical-align:baseline}
.lg i:first-child{margin-left:0}
table{width:100%;border-collapse:collapse;margin-top:10px;font-size:14px}
td,th{padding:5px 8px;border-bottom:1px solid var(--bd);text-align:right}
td:first-child,th:first-child{text-align:left;color:var(--mut);font-weight:400}
</style></head><body><div class="wrap">
<h1><span id="dot" class="dot"></span>Điều khiển bơm an toàn — ESP32 + A4950 <small id="conn" style="color:var(--mut);font-weight:400">đang kết nối…</small></h1>
<div class="grid">

<div class="card full"><h2>Máy trạng thái</h2>
<div class="fsm">
<span id="p0" class="pill s0">DỪNG</span><span class="arrow">→</span>
<span id="p1" class="pill s1">CHẠY</span><span class="arrow">→</span>
<span id="p2" class="pill s2">LỖI</span><span class="arrow">→ DỪNG</span>
</div>
<div class="bar"><div id="bar"></div></div>
<div class="row"><span id="pump" class="big">BƠM TẮT</span><span>Còn lại: <b id="rem">—</b> / giới hạn <b id="max">—</b></span></div>
</div>

<div class="card"><h2>Điều khiển</h2>
<div>Duty cycle: <span class="big" id="dv">90</span>%</div>
<input id="duty" type="range" min="1" max="100" value="90">
<div class="btns">
<button onclick="setDuty(25)">25%</button>
<button onclick="setDuty(50)">50%</button>
<button onclick="setDuty(90)">90%</button>
</div>
<div class="btns">
<button class="b-on" onclick="send('ON')">BẬT (ON)</button>
<button class="b-off" onclick="send('OFF')">TẮT (OFF)</button>
<button onclick="send('XYZ')" title="Thử Ca 3: lệnh sai định dạng">Gửi lệnh sai</button>
</div>
</div>

<div class="card"><h2>Cảm biến & hệ thống</h2>
<div class="kv">
<span>Nhiệt độ</span><b id="t">—</b>
<span>Độ ẩm</span><b id="h">—</b>
<span>Cảm biến</span><b id="sv">—</b>
<span>Uptime</span><b id="up">—</b>
<span>WiFi RSSI</span><b id="rs">—</b>
</div></div>

<div class="card full"><h2>So sánh chu kỳ: timer phần cứng và millis() (đặt <span id="per">100</span> ms)</h2>
<table>
<thead><tr><th>Chỉ tiêu (tính từ lúc bật hoặc lần reset)</th><th>Timer HW</th><th>millis()</th></tr></thead>
<tbody id="cmpb"><tr><td colspan="3">chờ số liệu…</td></tr></tbody>
</table>
<div class="lg">Mỗi chu kỳ được đo bằng micros() giữa hai sự kiện liên tiếp. "Mẫu lệch &gt; 1 ms" là số chu kỳ sai khác giá trị đặt quá 1 ms.</div>
<div class="btns"><button onclick="send('CMP')">In bảng ra log</button><button onclick="send('CMPRESET')">Reset số liệu</button><button onclick="dlCmp()">Tải bảng .txt</button></div>
</div>

<div class="card full"><h2>Serial log</h2>
<div id="log"></div>
<div class="cmd">
<input id="ci" placeholder="Gõ lệnh: ON, OFF, PWM 50, STATUS…" autocomplete="off">
<button onclick="sendBox()">Gửi</button>
<button onclick="logEl.innerHTML='';lines=[]">Xóa</button>
<button onclick="dl()">Tải .txt</button>
</div></div>

</div></div>
<script>
const $=id=>document.getElementById(id);
const logEl=$('log');let lines=[],ws,dragging=false;
function addLog(s){
  lines.push(s);
  const d=document.createElement('div');
  d.textContent=s;
  if(/LOI|NaN|TU CHOI|loi/.test(s))d.className='e';
  else if(/CMD:/.test(s))d.className='c';
  else if(/->/.test(s))d.className='w';
  const stick=logEl.scrollTop+logEl.clientHeight>=logEl.scrollHeight-30;
  logEl.appendChild(d);
  while(logEl.childElementCount>1000)logEl.removeChild(logEl.firstChild);
  if(stick)logEl.scrollTop=logEl.scrollHeight;
}
function send(c){if(ws&&ws.readyState===1)ws.send(c)}
function sendBox(){const v=$('ci').value.trim();if(v){send(v);$('ci').value=''}}
$('ci').addEventListener('keydown',e=>{if(e.key==='Enter')sendBox()});
function setDuty(v){$('duty').value=v;$('dv').textContent=v;send('PWM '+v)}
$('duty').addEventListener('input',e=>{dragging=true;$('dv').textContent=e.target.value});
$('duty').addEventListener('change',e=>{dragging=false;send('PWM '+e.target.value)});
function dl(){
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([lines.join('\n')+'\n'],{type:'text/plain'}));
  a.download='serial_log.txt';a.click();
}
function upd(s){
  [0,1,2].forEach(i=>$('p'+i).classList.toggle('on',s.st===i));
  $('pump').textContent=s.st===1?'BƠM ĐANG CHẠY ('+s.duty+'%)':
    (s.cd>0?'BƠM TẮT — COOLDOWN '+(s.cd/1000).toFixed(1)+' s':'BƠM TẮT');
  $('rem').textContent=s.st===1?(s.rem/1000).toFixed(1)+' s':'—';
  $('max').textContent=(s.max/1000)+' s';
  if(s.st===1){$('bar').style.background='var(--run)';$('bar').style.width=(100*(s.max-s.rem)/s.max)+'%'}
  else if(s.cd>0){$('bar').style.background='#d97706';$('bar').style.width=(100*s.cd/s.cdmax)+'%'}
  else $('bar').style.width='0%';
  if(!dragging){$('duty').value=s.duty;$('dv').textContent=s.duty}
  $('t').textContent=s.sv?s.t.toFixed(1)+' °C':'NaN';
  $('h').textContent=s.sv?s.h.toFixed(1)+' %':'NaN';
  $('sv').textContent=s.sv?'Hợp lệ':'LỖI (NaN)';
  $('sv').style.color=s.sv?'var(--ok)':'var(--err)';
  if(s.hw)cmpUpd(s);
  $('up').textContent=Math.floor(s.up/1000)+' s';
  $('rs').textContent=s.rssi?s.rssi+' dBm':'AP mode';
}
let cmpRows=[];
function cmpUpd(s){
  const a=s.hw,b=s.ms,P=s.per,ok=x=>x[0]>0;
  const f=(x,i,d)=>ok(x)?x[i].toFixed(d):'—';
  const dev=x=>ok(x)?((x[1]-P)*100/P).toFixed(4)+' %':'—';
  const jit=x=>ok(x)?(x[3]-x[2]):'—';
  cmpRows=[
    ['Chu kỳ đặt (µs)',P,P],
    ['Số mẫu',a[0],b[0]],
    ['Trung bình (µs)',f(a,1,1),f(b,1,1)],
    ['Lệch TB so với giá trị đặt',dev(a),dev(b)],
    ['Min (µs)',ok(a)?a[2]:'—',ok(b)?b[2]:'—'],
    ['Max (µs)',ok(a)?a[3]:'—',ok(b)?b[3]:'—'],
    ['Jitter max−min (µs)',jit(a),jit(b)],
    ['Độ lệch chuẩn σ (µs)',f(a,4,1),f(b,4,1)],
    ['Số mẫu lệch > 1 ms',a[5],b[5]],
    ['Tick bị lỡ',a[6],'—']
  ];
  $('cmpb').innerHTML=cmpRows.map(r=>'<tr><td>'+r[0]+'</td><td>'+r[1]+'</td><td>'+r[2]+'</td></tr>').join('');
  $('per').textContent=P/1000;
}
function dlCmp(){
  const t='Chi tieu\tTimer HW\tmillis()\n'+cmpRows.map(r=>r.join('\t')).join('\n')+'\n';
  const a=document.createElement('a');
  a.href=URL.createObjectURL(new Blob([t],{type:'text/plain'}));
  a.download='so_sanh_timer_millis.txt';a.click();
}
function connect(){
  ws=new WebSocket('ws://'+location.hostname+':81/');
  ws.onopen=()=>{$('dot').classList.add('ok');$('conn').textContent='đã kết nối'};
  ws.onclose=()=>{$('dot').classList.remove('ok');$('conn').textContent='mất kết nối, thử lại…';setTimeout(connect,1500)};
  ws.onmessage=e=>{
    const d=e.data;
    if(d[0]==='S'){try{upd(JSON.parse(d.slice(1)))}catch(x){}}
    else if(d[0]==='L')addLog(d.slice(1));
  };
}
connect();
</script></body></html>)rawliteral";

// ===================== BIẾN TOÀN CỤC =====================
// Kiểu dữ liệu phải khai báo TRƯỚC hàm đầu tiên, vì Arduino IDE tự chèn nguyên mẫu
// hàm (prototype) lên đầu file; nếu struct nằm sau thì báo lỗi "'Stat' does not name a type".
// Thống kê chu kỳ để so sánh timer phần cứng với millis().
struct Stat {
  uint32_t n = 0, mn = UINT32_MAX, mx = 0, out = 0;
  int64_t  sum = 0;      // tổng độ lệch so với chu kỳ đặt (us)
  double   sumsq = 0;    // tổng bình phương độ lệch
  void add(uint32_t dt) {
    int32_t d = (int32_t)dt - (int32_t)TIMER_PERIOD_US;
    n++; sum += d; sumsq += (double)d * d;
    if (dt < mn) mn = dt;
    if (dt > mx) mx = dt;
    if (d > 1000 || d < -1000) out++;     // lệch quá 1 ms
  }
  void reset() { n = 0; mn = UINT32_MAX; mx = 0; out = 0; sum = 0; sumsq = 0; }
};

enum State { ST_STOP, ST_RUN, ST_FAULT };
const char* stateName(State s) {
  return s == ST_STOP ? "DUNG" : (s == ST_RUN ? "CHAY" : "LOI");
}

State    state = ST_STOP;
uint32_t stateSince = 0;
uint32_t lastRunEndMs = 0;   // mốc vào DUNG gần nhất (0 = chưa từng chạy)
uint8_t  dutyPct = DEFAULT_DUTY_PCT;

// Sự kiện do lệnh tạo ra, updateState() sẽ xử lý
bool evStart = false, evStop = false, evBadInput = false;

DHT dht(PIN_DHT, DHTTYPE);
bool     sensorValid = false;
float    lastT = NAN, lastH = NAN;
uint32_t lastSensorMs = 0;

WebServer        server(80);
WebSocketsServer ws(81);
uint32_t lastStatusMs = 0;

// ---- Bộ đệm log (gửi lại cho client mới kết nối) ----
constexpr int LOG_LINES = 80;
constexpr int LOG_LEN   = 128;
char logRing[LOG_LINES][LOG_LEN];
int  logHead = 0, logCount = 0;

// ---- Timer phần cứng: ISR chỉ tăng bộ đếm và ghi mốc thời gian ----
hw_timer_t* hwTimer = nullptr;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t tickCount = 0;
volatile uint32_t tickTimeUs = 0;

void IRAM_ATTR onTimer() {
  portENTER_CRITICAL_ISR(&timerMux);
  tickCount++;
  tickTimeUs = (uint32_t)micros();
  portEXIT_CRITICAL_ISR(&timerMux);
}

// ---- Thống kê so sánh timer phần cứng và millis() (xử lý ngoài ISR) ----
// Tích lũy từ lúc bật (hoặc lần CMPRESET), không reset theo cửa sổ.

Stat statHw, statMs;
uint32_t missedHw = 0;                    // số tick timer HW bị lỡ (ISR chạy mà loop không kịp xử lý)
uint32_t seenCount = 0, seenTimeUs = 0;
uint32_t lastTimerReportMs = 0;

void printCompare();                      // định nghĩa ở phần SO SÁNH bên dưới

void statCalc(const Stat& st, double& avg, double& sd) {
  if (!st.n) { avg = 0; sd = 0; return; }
  double mean = (double)st.sum / st.n;
  double var = st.sumsq / st.n - mean * mean;
  if (var < 0) var = 0;
  avg = TIMER_PERIOD_US + mean;
  sd = sqrt(var);
}

// [n, trung bình, min, max, sigma, số mẫu lệch >1ms, tick lỡ]
void statJson(char* o, size_t sz, const Stat& st, uint32_t missed) {
  double avg, sd;
  statCalc(st, avg, sd);
  snprintf(o, sz, "[%lu,%.1f,%lu,%lu,%.1f,%lu,%lu]",
           (unsigned long)st.n, avg, (unsigned long)(st.n ? st.mn : 0), (unsigned long)st.mx,
           sd, (unsigned long)st.out, (unsigned long)missed);
}

// ===================== LOG: Serial + WebSocket =====================
void emitLine(const char* line) {
  Serial.println(line);
  strncpy(logRing[logHead], line, LOG_LEN - 1);
  logRing[logHead][LOG_LEN - 1] = 0;
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;

  String m = String("L") + line;
  ws.broadcastTXT(m);
}

// Log có mốc thời gian [millis]
void logMsg(const char* fmt, ...) {
  char body[100];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  char line[LOG_LEN];
  snprintf(line, sizeof line, "[%lu] %s", (unsigned long)millis(), body);
  emitLine(line);
}

// Log không mốc thời gian (dùng in bảng cho thẳng cột)
void logTable(const char* fmt, ...) {
  char line[LOG_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  emitLine(line);
}

void pushStatus() {
  uint32_t now = millis();
  uint32_t rem = 0;
  if (state == ST_RUN) {
    uint32_t el = now - stateSince;
    rem = el >= MAX_RUN_MS ? 0 : MAX_RUN_MS - el;
  }
  uint32_t cd = 0;   // thời gian cooldown còn lại
  if (state == ST_STOP && lastRunEndMs != 0 && now - lastRunEndMs < COOLDOWN_MS)
    cd = COOLDOWN_MS - (now - lastRunEndMs);
  char hwS[96], msS[96];
  statJson(hwS, sizeof hwS, statHw, missedHw);
  statJson(msS, sizeof msS, statMs, 0);
  char buf[420];
  snprintf(buf, sizeof buf,
    "S{\"st\":%d,\"duty\":%u,\"rem\":%lu,\"max\":%lu,\"cd\":%lu,\"cdmax\":%lu,\"sv\":%d,\"t\":%.1f,\"h\":%.1f,"
    "\"per\":%lu,\"hw\":%s,\"ms\":%s,\"up\":%lu,\"rssi\":%d}",
    (int)state, dutyPct, (unsigned long)rem, (unsigned long)MAX_RUN_MS,
    (unsigned long)cd, (unsigned long)COOLDOWN_MS,
    sensorValid ? 1 : 0, sensorValid ? lastT : 0.0f, sensorValid ? lastH : 0.0f,
    (unsigned long)TIMER_PERIOD_US, hwS, msS,
    (unsigned long)now, WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  String m(buf);
  ws.broadcastTXT(m);
  lastStatusMs = now;
}

// ===================== ĐIỀU KHIỂN TẢI =====================
void pumpOff() {
  ledcWrite(PIN_PUMP_IN1, 0);
  digitalWrite(PIN_PUMP_IN2, LOW);
}

void pumpOn() {
  digitalWrite(PIN_PUMP_IN2, LOW);
  ledcWrite(PIN_PUMP_IN1, (uint32_t)dutyPct * ((1u << PWM_BITS) - 1) / 100);
}

// ===================== CHUYỂN TRẠNG THÁI =====================
void enterStop(const char* reason) {
  pumpOff();                       // tắt tải TRƯỚC khi đổi trạng thái
  state = ST_STOP;
  stateSince = millis();
  lastRunEndMs = stateSince;       // bắt đầu cooldown
  logMsg("-> DUNG (%s)", reason);
  pushStatus();
}

void enterFault(const char* reason) {
  pumpOff();                       // tắt tải ngay lập tức
  state = ST_FAULT;
  stateSince = millis();
  logMsg("-> LOI (%s)", reason);
  pushStatus();
}

void enterRun() {
  state = ST_RUN;
  stateSince = millis();
  pumpOn();
  logMsg("-> CHAY (duty=%u%%, toi da %lu s)", dutyPct, (unsigned long)(MAX_RUN_MS / 1000));
  pushStatus();
}

void updateState() {
  uint32_t now = millis();

  switch (state) {
    case ST_RUN:
      if (evStart) logMsg("TU CHOI: dang CHAY, khong gia han thoi gian");
      // Ưu tiên cao nhất: lỗi / hết giờ, lệnh ON không ghi đè được
      if (evBadInput) {
        enterFault("lenh sai dinh dang");
      } else if (!sensorValid) {
        enterFault("cam bien NaN");
      } else if (now - stateSince >= MAX_RUN_MS) {
        enterStop("het gio");
      } else if (evStop) {
        enterStop("lenh OFF");
      }
      break;

    case ST_STOP:
      if (evStart) {
        uint32_t since = now - lastRunEndMs;
        if (lastRunEndMs != 0 && since < COOLDOWN_MS)
          logMsg("TU CHOI: dang cooldown, con %lu ms", (unsigned long)(COOLDOWN_MS - since));
        else if (!sensorValid)
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

  // Phòng thủ: ngoài trạng thái CHAY, tải luôn phải tắt
  if (state != ST_RUN) pumpOff();

  evStart = evStop = evBadInput = false;
}

// ===================== CẢM BIẾN =====================
void readSensor() {
  uint32_t now = millis();
  if (now - lastSensorMs < SENSOR_PERIOD_MS) return;
  lastSensorMs = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  sensorValid = !(isnan(h) || isnan(t));
  if (sensorValid) {
    lastT = t; lastH = h;
    logMsg("DHT: T=%.1fC H=%.1f%%", t, h);
  } else {
    logMsg("DHT: NaN (cam bien loi/rut day)");
  }
}

// ===================== LỆNH (Serial + WebSocket) =====================
void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0) return;
  logMsg("CMD: %s", cmd.c_str());

  if (cmd == "ON") {
    evStart = true;
  } else if (cmd == "OFF") {
    evStop = true;
  } else if (cmd == "CMP") {
    printCompare();
  } else if (cmd == "CMPRESET") {
    statHw.reset(); statMs.reset(); missedHw = 0;
    logMsg("Da reset so lieu so sanh timer/millis");
  } else if (cmd == "STATUS") {
    logMsg("STATE=%s duty=%u%% sensorValid=%d T=%.1f H=%.1f",
           stateName(state), dutyPct, sensorValid, lastT, lastH);
  } else if (cmd.startsWith("PWM ")) {
    int v = cmd.substring(4).toInt();
    if (v >= 1 && v <= 100) {
      dutyPct = v;
      logMsg("Duty = %u%%", dutyPct);
      if (state == ST_RUN) pumpOn();   // áp dụng ngay khi đang chạy
      pushStatus();
    } else {
      logMsg("LOI: PWM phai tu 1 den 100");
      evBadInput = true;
    }
  } else {
    logMsg("LOI: lenh khong hop le");
    evBadInput = true;   // chỉ gây LỖI nếu đang CHẠY (xem updateState)
  }
}

void pollSerial() {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      handleCommand(buf);
      buf = "";
    } else if (buf.length() < 32) {
      buf += c;
    } else {
      buf = "";             // quá dài -> coi là rác
      evBadInput = true;
    }
  }
}

// ===================== WEBSOCKET =====================
void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) {
    // gửi lại log cũ + trạng thái hiện tại cho client mới
    int start = (logHead - logCount + LOG_LINES) % LOG_LINES;
    for (int i = 0; i < logCount; i++) {
      String m = String("L") + logRing[(start + i) % LOG_LINES];
      ws.sendTXT(num, m);
    }
    pushStatus();
  } else if (type == WStype_TEXT) {
    if (length > 32) { evBadInput = true; logMsg("LOI: lenh qua dai"); return; }
    String cmd((const char*)payload, length);
    handleCommand(cmd);   // đi qua đúng đường xử lý như Serial, không bỏ qua FSM
  }
}

// ===================== SO SÁNH: TIMER PHẦN CỨNG vs millis() =====================
// Cả hai tạo sự kiện mỗi ~100 ms; mỗi sự kiện được đóng dấu micros(), rồi đo khoảng
// cách tới sự kiện trước.
//  - HW: ISR của timer phần cứng đóng dấu thời gian ngay trong ngắt.
//  - MS: vòng lặp kiểu Bài 2:  if (millis() - last >= 100) { last = millis(); ... }

void processMillisTick() {
  static uint32_t lastMs = 0, prevUs = 0;
  static bool first = true;
  uint32_t nowMs = millis();
  if (nowMs - lastMs >= MILLIS_PERIOD_MS) {
    lastMs = nowMs;                       // kiểu Bài 2: tính từ lúc phát hiện -> trôi dần
    uint32_t us = (uint32_t)micros();
    if (!first) statMs.add(us - prevUs);
    first = false;
    prevUs = us;
  }
}

void processTimerStats() {
  uint32_t c, t;
  portENTER_CRITICAL(&timerMux);
  c = tickCount; t = tickTimeUs;
  portEXIT_CRITICAL(&timerMux);

  if (c != seenCount) {
    if (seenCount != 0) {
      if (c == seenCount + 1) statHw.add(t - seenTimeUs);
      else missedHw += c - seenCount - 1;       // loop bận quá lâu, lỡ tick
    }
    seenCount = c; seenTimeUs = t;
  }

  uint32_t now = millis();
  if (now - lastTimerReportMs >= TIMER_REPORT_MS) {
    lastTimerReportMs = now;
    printCompare();
  }
}

// In bảng so sánh ra Serial + web log (tự động mỗi 30 s hoặc khi gõ CMP)
void printCompare() {
  if (statHw.n == 0 || statMs.n == 0) { logMsg("SO SANH: chua co du so lieu"); return; }
  double ha, hsd, ma, msd;
  statCalc(statHw, ha, hsd);
  statCalc(statMs, ma, msd);
  double hdev = (ha - TIMER_PERIOD_US) * 100.0 / TIMER_PERIOD_US;
  double mdev = (ma - TIMER_PERIOD_US) * 100.0 / TIMER_PERIOD_US;

  logTable("=== SO SANH CHU KY DAT %lu us: TIMER HW vs millis() ===", (unsigned long)TIMER_PERIOD_US);
  logTable("%-22s%12s%12s", "Chi tieu", "Timer HW", "millis()");
  logTable("%-22s%12lu%12lu", "So mau", (unsigned long)statHw.n, (unsigned long)statMs.n);
  logTable("%-22s%12.1f%12.1f", "Trung binh (us)", ha, ma);
  logTable("%-22s%12.4f%12.4f", "Lech TB (%)", hdev, mdev);
  logTable("%-22s%12lu%12lu", "Min (us)", (unsigned long)statHw.mn, (unsigned long)statMs.mn);
  logTable("%-22s%12lu%12lu", "Max (us)", (unsigned long)statHw.mx, (unsigned long)statMs.mx);
  logTable("%-22s%12lu%12lu", "Jitter max-min (us)",
           (unsigned long)(statHw.mx - statHw.mn), (unsigned long)(statMs.mx - statMs.mn));
  logTable("%-22s%12.1f%12.1f", "Do lech chuan (us)", hsd, msd);
  logTable("%-22s%12lu%12lu", "Mau lech >1ms", (unsigned long)statHw.out, (unsigned long)statMs.out);
  logTable("%-22s%12lu%12s", "Tick HW bi lo", (unsigned long)missedHw, "-");
}

// ===================== WIFI =====================
void startNetwork() {
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

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK. Mo web: http://%s/\n", WiFi.localIP().toString().c_str());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("Khong ket noi duoc WiFi -> phat AP '%s' (pass %s). Mo web: http://%s/\n",
                  AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
  }
  if (MDNS.begin("esp32-pump")) Serial.println("mDNS: http://esp32-pump.local/");

  server.on("/", []() { server.send(200, "text/html", INDEX_HTML); });
  server.begin();
  ws.begin();
  ws.onEvent(wsEvent);
}

// ===================== SETUP / LOOP =====================
void setup() {
  // 1. Việc đầu tiên: đưa tải về trạng thái an toàn
  pinMode(PIN_PUMP_IN1, OUTPUT);
  pinMode(PIN_PUMP_IN2, OUTPUT);
  digitalWrite(PIN_PUMP_IN1, LOW);
  digitalWrite(PIN_PUMP_IN2, LOW);

  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== TH03: Dieu khien bom an toan ===");

  // 2. PWM (core 3.x): gắn kênh cho IN1 rồi đặt duty 0
  ledcAttach(PIN_PUMP_IN1, PWM_FREQ_HZ, PWM_BITS);
  pumpOff();

  // 3. Cảm biến
  dht.begin();

  // 4. Mạng + web (bơm đã tắt nên chờ WiFi là an toàn)
  startNetwork();

  // 5. Timer phần cứng (core 3.x): tick 1 MHz, báo mỗi 100 ms, tự nạp lại
  hwTimer = timerBegin(1000000);
  timerAttachInterrupt(hwTimer, &onTimer);
  timerAlarm(hwTimer, TIMER_PERIOD_US, true, 0);

  stateSince = millis();
  logMsg("San sang. Trang thai: %s, bom TAT. Gioi han chay: %lu s",
         stateName(state), (unsigned long)(MAX_RUN_MS / 1000));
  logMsg("Lenh: ON | OFF | PWM <1-100> | STATUS | CMP | CMPRESET");
}

void loop() {
  server.handleClient(); // phục vụ trang web (không chặn)
  ws.loop();             // WebSocket (không chặn)
  pollSerial();          // lệnh từ Serial (không chặn)
  readSensor();          // đọc mỗi 2 s bằng millis()
  updateState();         // máy trạng thái, không dùng delay()
  processMillisTick();   // sự kiện 100 ms kiểu millis() (để so sánh)
  processTimerStats();   // xử lý dữ liệu timer ngoài ISR

  if (millis() - lastStatusMs >= STATUS_PERIOD_MS) pushStatus();
}
