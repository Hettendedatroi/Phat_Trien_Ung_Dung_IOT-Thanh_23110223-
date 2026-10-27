// =====================================================================
//  BẢN A: LẬP LỊCH BẰNG delay(2000)
//  - Mỗi vòng: đo 1 mẫu -> đổi LED 1 lần -> phản hồi nút -> delay(2000)
//  - Trong lúc delay(), chương trình "đứng im": nút bấm chỉ được phản hồi
//    sau khi hết delay (độ trễ 0..2000 ms), LED không nháy 500 ms được.
//  - Chu kỳ thực = 2000 ms + thời gian đọc cảm biến -> trôi dần.
//  (Tên thư mục sketch phải trùng tên file .ino khi mở bằng Arduino IDE)
// =====================================================================
#define MODE_NAME "delay"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <DHT.h>

// ================== CẤU HÌNH ==================
const char* ssid     = "TN-WF-T3";       // Wi-Fi 2.4 GHz
const char* password = "TriNam123$$";

#define DHTPIN  14
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

const unsigned long SAMPLE_INTERVAL = 2000;  // 1 mẫu / 2 giây

// Hiệu chỉnh: converted = raw + offset (để 0 nếu chưa hiệu chuẩn)
const float TEMP_OFFSET = 0.0;
const float HUM_OFFSET  = 0.0;

// Nhiễu giả (có thể bật/tắt trên web)
volatile uint8_t noiseLevel = 2;   // 0=tắt, 1=nhẹ, 2=vừa, 3=mạnh

// Bộ nhớ lưu dữ liệu: 1000 dòng = 500 mẫu (~16 phút)
#define MAX_ROWS 1000
struct Row {
  unsigned long ts;
  float raw;
  float conv;
  bool isTemp;
  bool valid;
};
Row rows[MAX_ROWS];
int rowCount = 0;
SemaphoreHandle_t bufMutex;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ================== THÍ NGHIỆM NÚT BẤM ==================
#define LED_PIN 2   // LED tích hợp (quan sát bằng mắt)
#define BTN_PIN 0   // nút BOOT trên board (nhấn = LOW)

struct BtnEvent { unsigned long press; unsigned long resp; };
#define MAX_BTN 100
BtnEvent btnEvents[MAX_BTN];
int btnCount = 0;

// ISR ghi lại THỜI ĐIỂM BẤM thật (ngay lúc nhấn), loop() mới là nơi PHẢN HỒI
#define PRESS_Q 16
volatile unsigned long pressQ[PRESS_Q];
volatile uint8_t qHead = 0, qTail = 0;
volatile unsigned long lastPressIsr = 0;

void IRAM_ATTR onButtonIsr() {
  unsigned long now = millis();
  if (now - lastPressIsr < 50) return;          // chống dội phím 50 ms
  lastPressIsr = now;
  uint8_t next = (qHead + 1) % PRESS_Q;
  if (next != qTail) { pressQ[qHead] = now; qHead = next; }
}

// ================== GIAO DIỆN WEB ==================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="vi">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>DHT22 - Lọc nhiễu MA / EMA</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 0; padding: 16px 10px; background: #f4f4f9; color: #222; }
    .wrap { max-width: 760px; margin: 0 auto; }
    h2 { text-align: center; margin: 6px 0 14px; }
    .card { background: #fff; padding: 16px; margin: 12px 0; border-radius: 10px; box-shadow: 0 0 10px rgba(0,0,0,0.1); }
    .row { display: flex; gap: 12px; flex-wrap: wrap; align-items: center; }
    .grow { flex: 1; }
    .value { font-size: 26px; font-weight: bold; color: #007BFF; }
    .bar { background: #e5e5ea; border-radius: 6px; height: 12px; overflow: hidden; margin: 8px 0; }
    .bar > div { background: #007BFF; height: 100%; width: 0%; transition: width .3s; }
    .ok { color: #28a745; font-weight: bold; }
    .small { font-size: 13px; color: #555; }
    .btn { padding: 8px 14px; border: none; border-radius: 6px; font-size: 14px; cursor: pointer; text-decoration: none; color: #fff; display: inline-block; }
    .b-green { background: #28a745; } .b-blue { background: #007BFF; } .b-red { background: #dc3545; }
    select { padding: 4px; font-size: 14px; }
    canvas { display: block; width: 100%; }
    .legend span { display: inline-block; margin-right: 14px; font-size: 13px; }
    .sw { display: inline-block; width: 18px; height: 4px; vertical-align: middle; margin-right: 5px; }
    .notes { background: #f8f9fb; border-left: 4px solid #007BFF; padding: 8px 12px; font-size: 13.5px; line-height: 1.55; margin-top: 10px; }
    .tag { font-size: 12px; background: #ffe8cc; color: #a05a00; padding: 2px 8px; border-radius: 10px; }
  </style>
</head>
<body>
<div class="wrap">
  <h2>Giám sát DHT22: dữ liệu gốc &amp; lọc nhiễu <span class="tag" id="modeTag"></span></h2>

  <div class="card">
    <div class="row">
      <div class="grow">Nhiệt độ: <span id="temp" class="value">--</span> &deg;C</div>
      <div class="grow">Độ ẩm: <span id="hum" class="value">--</span> %</div>
    </div>
    <div id="status" style="margin-top:10px">Đã đo: 0 / 30 mẫu</div>
    <div class="bar"><div id="bar"></div></div>
    <div class="small" id="detail"></div>
  </div>

  <div class="card">
    <h3 style="margin-top:0">Chu kỳ lấy mẫu thực tế (so với 2000 ms)</h3>
    <div class="notes" id="periodInfo">Đang chờ dữ liệu...</div>
    <div class="row" style="margin-top:10px">
      <a class="btn b-green" href="/btn.csv" download>⬇ CSV thử nút</a>
    </div>
    <p class="small">Thử nút: nhấn nút BOOT trên board nhiều lần (nên nhấn rải rác trong lúc chờ), xem Serial Monitor hoặc tải CSV thử nút (press, response, latency).</p>
  </div>

  <div class="card">
    <div class="row">
      <label><b>Nhiễu giả</b>:
        <select id="noiseSel" onchange="setNoise()">
          <option value="0">Tắt</option><option value="1">Nhẹ</option><option value="2" selected>Vừa</option><option value="3">Mạnh</option>
        </select></label>
      <label><input type="checkbox" id="hampelChk" checked onchange="paramChange()"> Loại xung đột biến (Hampel) trước khi lọc</label>
      <div class="small" style="width:100%">Nhiễu giả = nhiễu Gauss + xung đột biến + mất mẫu (NaN); luôn có tối thiểu 1 xung / 12 mẫu và 1 NaN / 20 mẫu khi bật.</div>
    </div>
    <div class="row" style="margin-top:10px">
      <label>Trung bình trượt, cửa sổ N =
        <select id="selN" onchange="paramChange()">
          <option>3</option><option selected>5</option><option>7</option><option>10</option>
        </select></label>
      <label>EMA, &alpha; =
        <select id="selA" onchange="paramChange()">
          <option>0.1</option><option>0.2</option><option selected>0.3</option><option>0.5</option>
        </select></label>
      <button class="btn b-red" onclick="resetData()">Xóa &amp; đo lại</button>
    </div>
  </div>

  <div class="card">
    <h3 style="margin-top:0">Nhiệt độ (&deg;C)</h3>
    <canvas id="cT"></canvas>
    <div class="legend">
      <span><i class="sw" style="background:#b0b5ba"></i>Gốc</span>
      <span><i class="sw" style="background:#007BFF"></i>Trung bình trượt (MA)</span>
      <span><i class="sw" style="background:#ff7a00"></i>EMA</span>
      <span><i class="sw" style="background:#fff;border:2px solid #d63384;height:8px;width:8px;border-radius:50%"></i>Xung bị loại</span>
      <span><i class="sw" style="background:#dc3545;height:8px;width:3px"></i>Mẫu lỗi (NaN)</span>
    </div>
    <div class="notes" id="infoT"></div>
    <div class="row" style="margin-top:10px">
      <a class="btn b-green" href="/temp.csv" download="dht22_temperature.csv">⬇ CSV gốc</a>
      <button class="btn b-blue" onclick="exportFiltered('temp')">⬇ CSV đã lọc</button>
    </div>
  </div>

  <div class="card">
    <h3 style="margin-top:0">Độ ẩm (%)</h3>
    <canvas id="cH"></canvas>
    <div class="legend">
      <span><i class="sw" style="background:#b0b5ba"></i>Gốc</span>
      <span><i class="sw" style="background:#007BFF"></i>Trung bình trượt (MA)</span>
      <span><i class="sw" style="background:#ff7a00"></i>EMA</span>
      <span><i class="sw" style="background:#fff;border:2px solid #d63384;height:8px;width:8px;border-radius:50%"></i>Xung bị loại</span>
      <span><i class="sw" style="background:#dc3545;height:8px;width:3px"></i>Mẫu lỗi (NaN)</span>
    </div>
    <div class="notes" id="infoH"></div>
    <div class="row" style="margin-top:10px">
      <a class="btn b-green" href="/hum.csv" download="dht22_humidity.csv">⬇ CSV gốc</a>
      <button class="btn b-blue" onclick="exportFiltered('hum')">⬇ CSV đã lọc</button>
    </div>
  </div>

  <p class="small" style="text-align:center">
    CSV gốc: timestamp(ms), raw_value, converted_value, unit, valid &mdash; mỗi đại lượng một file.<br>
    CSV đã lọc: thêm cột outlier, ma_N và ema_&alpha; (chỉ tính trên mẫu hợp lệ).
  </p>
</div>

<script>
  var TARGET = 30;
  var CH = {
    temp: { name: "Nhiệt độ", unit: "C", canvas: "cT", info: "infoT", val: "temp", pts: [], lastTs: -1 },
    hum:  { name: "Độ ẩm",    unit: "%", canvas: "cH", info: "infoH", val: "hum",  pts: [], lastTs: -1 }
  };
  var firstTs = null, lastAny = null;
  var MA_N = 5, EMA_A = 0.3, USE_HAMPEL = true;
  var pending = false;

  function scheduleRender() {
    if (pending) return;
    pending = true;
    requestAnimationFrame(function () { pending = false; render(); });
  }

  function clearAll() {
    ["temp", "hum"].forEach(function (k) { CH[k].pts = []; CH[k].lastTs = -1; });
    firstTs = null; lastAny = null;
    document.getElementById('temp').innerHTML = "--";
    document.getElementById('hum').innerHTML = "--";
    scheduleRender();
  }

  function addPoint(key, ts, rawStr, convStr, valid) {
    var ch = CH[key];
    if (!ch || ts <= ch.lastTs) return;           // bỏ dòng trùng
    ch.lastTs = ts;
    if (firstTs === null || ts < firstTs) firstTs = ts;
    if (lastAny === null || ts > lastAny) lastAny = ts;
    ch.pts.push({ ts: ts, rawStr: rawStr, convStr: convStr,
                  raw: parseFloat(rawStr), conv: parseFloat(convStr), valid: !!valid });
    document.getElementById(ch.val).innerHTML = valid ? convStr : "Lỗi (NaN)";
  }

  // ---------- LỌC ----------
  function median(a) {
    var s = a.slice().sort(function (x, y) { return x - y; });
    var m = s.length >> 1;
    return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
  }

  // Bước 1 (tùy chọn): Hampel loại xung đột biến  ->  Bước 2: MA và EMA trên dữ liệu đã làm sạch
  function computeFilters(ch) {
    var win = [], sum = 0, ema = null;
    var hw = [], consec = 0;                        // cửa sổ Hampel: 7 giá trị đã làm sạch gần nhất
    var floor = ch.unit === "C" ? 0.15 : 0.8;       // ngưỡng sàn (nhiễu nền của cảm biến)
    ch.pts.forEach(function (p) {
      p.outlier = false;
      if (!p.valid) { p.clean = null; p.ma = null; p.ema = null; return; }
      var x = p.conv;
      if (USE_HAMPEL && hw.length >= 5) {
        var m = median(hw);
        var mad = median(hw.map(function (v) { return Math.abs(v - m); }));
        var sigma = Math.max(1.4826 * mad, floor);
        if (Math.abs(x - m) > 3 * sigma) {
          if (consec < 2) { p.outlier = true; consec++; x = m; }   // xung: thay bằng trung vị
          else { hw = []; consec = 0; }                            // 3 mẫu liên tiếp lệch = thay đổi thật, chấp nhận
        } else {
          consec = 0;
        }
      }
      hw.push(x); if (hw.length > 7) hw.shift();
      p.clean = x;
      win.push(x); sum += x;
      if (win.length > MA_N) sum -= win.shift();
      p.ma = sum / win.length;                      // N-1 mẫu đầu: cửa sổ mở rộng
      ema = (ema === null) ? x : EMA_A * x + (1 - EMA_A) * ema;
      p.ema = ema;
    });
  }

  function roughness(v) {                           // RMS chênh lệch 2 mẫu liên tiếp
    if (v.length < 3) return null;
    var s = 0;
    for (var i = 1; i < v.length; i++) { var d = v[i] - v[i - 1]; s += d * d; }
    return Math.sqrt(s / (v.length - 1));
  }

  function samplePeriod() {
    var n = CH.temp.pts.length;
    if (n < 2 || firstTs === null) return 2;
    return (lastAny - firstTs) / 1000 / (n - 1);
  }

  function statsHTML(ch) {
    var v = ch.pts.filter(function (p) { return p.valid; });
    if (v.length < 6) return "Cần thêm dữ liệu để nhận xét (&ge; 6 mẫu hợp lệ).";
    var r0 = roughness(v.map(function (p) { return p.conv; }));
    var r1 = roughness(v.map(function (p) { return p.ma; }));
    var r2 = roughness(v.map(function (p) { return p.ema; }));
    var red1 = (1 - r1 / r0) * 100, red2 = (1 - r2 / r0) * 100;
    var dt = samplePeriod();
    var d1 = (MA_N - 1) / 2 * dt, d2 = (1 - EMA_A) / EMA_A * dt;
    var u = ch.unit === "C" ? "&deg;C" : "%";

    var outCount = v.filter(function (p) { return p.outlier; }).length;
    var h = "<b>Độ mượt</b> (RMS chênh lệch giữa 2 mẫu liên tiếp, càng nhỏ càng mượt):<br>" +
            "&bull; Gốc: " + r0.toFixed(3) + " " + u + "<br>" +
            "&bull; MA (N=" + MA_N + "): " + r1.toFixed(3) + " " + u + " (giảm " + red1.toFixed(0) + "%)<br>" +
            "&bull; EMA (&alpha;=" + EMA_A + "): " + r2.toFixed(3) + " " + u + " (giảm " + red2.toFixed(0) + "%)<br>" +
            "<b>Độ trễ ước tính</b> (trễ nhóm, so với tín hiệu gốc):<br>" +
            "&bull; MA &asymp; (N&minus;1)/2 = " + ((MA_N - 1) / 2).toFixed(1) + " mẫu &asymp; " + d1.toFixed(1) + " s<br>" +
            "&bull; EMA &asymp; (1&minus;&alpha;)/&alpha; = " + ((1 - EMA_A) / EMA_A).toFixed(1) + " mẫu &asymp; " + d2.toFixed(1) + " s<br>" +
            (USE_HAMPEL ? "<b>Loại xung (Hampel):</b> đã loại " + outCount + " / " + v.length +
              " mẫu hợp lệ (lệch trung vị &gt; 3&sigma;) trước khi đưa vào MA/EMA.<br>" : "") +
            "<b>Nhận xét:</b> ";

    if (red1 < 15 && red2 < 15) {
      h += "dữ liệu gốc đã khá ít nhiễu nên hai bộ lọc chưa tạo khác biệt rõ; hãy bật nhiễu giả để quan sát. ";
    } else {
      var better = r1 < r2 ? "MA" : "EMA";
      h += "với tham số hiện tại " + better + " cho đường cong mượt hơn. ";
      h += "MA loại bỏ xung đột biến tốt khi cửa sổ đủ lớn nhưng trễ " + d1.toFixed(1) +
           " s; EMA phản ứng nhanh hơn với thay đổi thật khi &alpha; lớn nhưng xung đột biến còn \"lọt\" nhiều hơn. ";
    }
    if (USE_HAMPEL) h += "Nhờ loại xung trước, đường MA/EMA không còn bị kéo lệch bởi các điểm đột biến; bỏ tích Hampel để thấy xung làm lệch cả hai đường. ";
    else h += "Chưa loại xung nên mỗi điểm đột biến kéo lệch MA/EMA trong vài mẫu (MA lệch trong N mẫu, EMA giảm dần theo &alpha;). ";
    h += "Tăng N hoặc giảm &alpha; &rArr; mượt hơn nhưng trễ hơn (đánh đổi mượt &ndash; trễ). " +
         "Để thấy độ trễ rõ, chạm nhẹ hoặc thổi hơi vào cảm biến tạo thay đổi đột ngột rồi quan sát đường MA/EMA bám theo chậm hơn đường gốc.";
    return h;
  }

  // ---------- VẼ ĐỒ THỊ ----------
  function drawChart(ch) {
    var cv = document.getElementById(ch.canvas);
    var W = cv.parentElement.clientWidth - 32, H = 240;
    if (W < 100) W = 300;
    var dpr = window.devicePixelRatio || 1;
    cv.width = W * dpr; cv.height = H * dpr;
    cv.style.width = W + "px"; cv.style.height = H + "px";
    var g = cv.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, W, H);

    var L = 46, R = 10, T = 10, B = 30;
    var valid = ch.pts.filter(function (p) { return p.valid; });
    if (valid.length < 2) {
      g.fillStyle = "#888"; g.font = "13px Arial"; g.textAlign = "center";
      g.fillText("Đang chờ dữ liệu...", W / 2, H / 2);
      return;
    }
    var ymin = Infinity, ymax = -Infinity;
    valid.forEach(function (p) {
      [p.conv, p.ma, p.ema].forEach(function (v) { if (v < ymin) ymin = v; if (v > ymax) ymax = v; });
    });
    var pad = (ymax - ymin) * 0.1 || 0.5;
    ymin -= pad; ymax += pad;
    var tmax = Math.max((lastAny - firstTs) / 1000, 1);
    function X(ts) { return L + (ts - firstTs) / 1000 / tmax * (W - L - R); }
    function Y(v) { return T + (ymax - v) / (ymax - ymin) * (H - T - B); }

    g.strokeStyle = "#e5e5ea"; g.fillStyle = "#666"; g.font = "11px Arial"; g.lineWidth = 1;
    g.textAlign = "right";
    for (var i = 0; i <= 4; i++) {
      var v = ymin + (ymax - ymin) * i / 4, y = Y(v);
      g.beginPath(); g.moveTo(L, y); g.lineTo(W - R, y); g.stroke();
      g.fillText(v.toFixed(1), L - 5, y + 4);
    }
    g.textAlign = "center";
    for (var j = 0; j <= 5; j++) {
      var x = L + (W - L - R) * j / 5;
      g.beginPath(); g.moveTo(x, T); g.lineTo(x, H - B); g.stroke();
      g.fillText((tmax * j / 5).toFixed(0), x, H - B + 14);
    }
    g.fillText("Thời gian (s)", L + (W - L - R) / 2, H - 4);

    function series(key, color, w) {
      g.beginPath();
      valid.forEach(function (p, k) {
        var x = X(p.ts), y = Y(p[key]);
        if (k === 0) g.moveTo(x, y); else g.lineTo(x, y);
      });
      g.strokeStyle = color; g.lineWidth = w; g.stroke();
    }
    series('conv', '#b0b5ba', 1.2);
    g.fillStyle = '#8a9096';
    valid.forEach(function (p) { g.fillRect(X(p.ts) - 1.5, Y(p.conv) - 1.5, 3, 3); });
    series('ma', '#007BFF', 2);
    series('ema', '#ff7a00', 2);

    ch.pts.forEach(function (p) {
      if (p.valid) return;
      var x = X(p.ts);
      g.strokeStyle = 'rgba(220,53,69,0.45)'; g.lineWidth = 1; g.setLineDash([4, 3]);
      g.beginPath(); g.moveTo(x, T); g.lineTo(x, H - B); g.stroke(); g.setLineDash([]);
      g.fillStyle = '#dc3545'; g.fillRect(x - 2, H - B - 12, 4, 12);
    });
    g.strokeStyle = '#d63384'; g.lineWidth = 2;
    valid.forEach(function (p) {
      if (!p.outlier) return;
      g.beginPath(); g.arc(X(p.ts), Y(p.conv), 5.5, 0, 2 * Math.PI); g.stroke();
    });
  }

  function render() {
    ["temp", "hum"].forEach(function (k) {
      var ch = CH[k];
      computeFilters(ch);
      drawChart(ch);
      document.getElementById(ch.info).innerHTML = statsHTML(ch);
    });
    var t = CH.temp.pts, n = t.length;
    var bad = t.filter(function (p) { return !p.valid; }).length;
    var sec = (firstTs !== null && lastAny !== null) ? Math.round((lastAny - firstTs) / 1000) : 0;
    document.getElementById('status').innerHTML = (n >= TARGET && sec >= 60)
      ? '<span class="ok">Đã đủ ' + n + ' mẫu (' + sec + ' s) &ndash; có thể xuất CSV</span>'
      : 'Đã đo: ' + n + ' / ' + TARGET + ' mẫu';
    document.getElementById('bar').style.width = Math.min(100, n / TARGET * 100) + '%';
    document.getElementById('detail').innerHTML =
      'Thời gian: ' + sec + ' s (cần &ge; 60 s) | Mẫu lỗi (NaN): ' + bad;
    document.getElementById('periodInfo').innerHTML = periodHTML();
  }

  // ---------- CHU KỲ LẤY MẪU THỰC TẾ ----------
  function periodHTML() {
    var t = CH.temp.pts;
    if (t.length < 3) return "Cần &ge; 3 mẫu để tính.";
    var dts = [], sum = 0, i;
    for (i = 1; i < t.length; i++) { var d = t[i].ts - t[i - 1].ts; dts.push(d); sum += d; }
    var n = dts.length, mean = sum / n, s2 = 0, mn = Infinity, mx = -Infinity;
    dts.forEach(function (d) { s2 += (d - mean) * (d - mean); if (d < mn) mn = d; if (d > mx) mx = d; });
    var sd = Math.sqrt(s2 / (n - 1));
    var total = t[t.length - 1].ts - t[0].ts, ideal = n * 2000, drift = total - ideal;
    return "Số khoảng đo: " + n + "<br>" +
           "&bull; Chu kỳ trung bình: <b>" + mean.toFixed(1) + " ms</b> (lệch " + (mean - 2000 >= 0 ? "+" : "") + (mean - 2000).toFixed(1) + " ms so với 2000)<br>" +
           "&bull; Độ lệch chuẩn: " + sd.toFixed(2) + " ms &nbsp;|&nbsp; Min: " + mn + " ms &nbsp;|&nbsp; Max: " + mx + " ms<br>" +
           "&bull; Trôi tích lũy sau " + (n + 1) + " mẫu: <b>" + (drift >= 0 ? "+" : "") + drift + " ms</b> " +
           "(= tổng thời gian &minus; " + n + "&times;2000)";
  }

  // ---------- XUẤT CSV ĐÃ LỌC ----------
  function exportFiltered(key) {
    var ch = CH[key];
    computeFilters(ch);
    var out = ["timestamp,raw_value,converted_value,unit,valid,outlier,ma_" + MA_N + ",ema_" + EMA_A];
    ch.pts.forEach(function (p) {
      out.push([p.ts, p.rawStr, p.convStr, ch.unit, p.valid ? 1 : 0, p.outlier ? 1 : 0,
                p.ma === null ? "NaN" : p.ma.toFixed(3),
                p.ema === null ? "NaN" : p.ema.toFixed(3)].join(","));
    });
    var blob = new Blob([out.join("\n") + "\n"], { type: "text/csv;charset=utf-8" });
    var a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = key === "temp" ? "dht22_temperature_filtered.csv" : "dht22_humidity_filtered.csv";
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(function () { URL.revokeObjectURL(a.href); }, 1000);
  }

  // ---------- ĐIỀU KHIỂN ----------
  function paramChange() {
    MA_N = parseInt(document.getElementById('selN').value);
    EMA_A = parseFloat(document.getElementById('selA').value);
    USE_HAMPEL = document.getElementById('hampelChk').checked;
    scheduleRender();
  }
  function setNoise() {
    var lv = document.getElementById('noiseSel').value;
    fetch('/noise?level=' + lv).then(function (r) { return r.text(); }).then(function (t) {
      document.getElementById('noiseSel').value = t.trim();
    });
  }
  function resetData() {
    if (confirm("Xóa toàn bộ dữ liệu hiện tại và bắt đầu đo lại?")) fetch('/reset');
  }

  // ---------- TẢI DỮ LIỆU & WEBSOCKET ----------
  function loadHistory(key) {
    return fetch('/' + key + '.csv').then(function (r) { return r.text(); }).then(function (txt) {
      var lines = txt.trim().split("\n");
      for (var i = 1; i < lines.length; i++) {
        var p = lines[i].trim().split(",");
        if (p.length < 5) continue;
        addPoint(key, parseInt(p[0]), p[1], p[2], parseInt(p[4]));
      }
    }).catch(function () {});
  }

  function initWebSocket() {
    var ws = new WebSocket('ws://' + window.location.hostname + '/ws');
    ws.onmessage = function (event) {
      var d = JSON.parse(event.data);
      if (d.type === "reset") { clearAll(); return; }
      addPoint(d.type, d.timestamp, d.raw, d.converted, d.valid);
      scheduleRender();
    };
    ws.onclose = function () { setTimeout(initWebSocket, 2000); };
  }

  window.addEventListener('resize', scheduleRender);
  window.addEventListener('load', function () {
    fetch('/mode').then(function (r) { return r.text(); }).then(function (t) {
      document.getElementById('modeTag').textContent = 'Bản ' + t;
    }).catch(function () {});
    fetch('/noise').then(function (r) { return r.text(); }).then(function (t) {
      document.getElementById('noiseSel').value = t.trim();
    }).catch(function () {});
    loadHistory("temp").then(function () { return loadHistory("hum"); }).then(function () {
      render();
      initWebSocket();
    });
  });
</script>
</body>
</html>
)rawliteral";

// ================== HÀM PHỤ ==================
void formatVal(char* buf, size_t size, float v, bool valid) {
  if (valid) snprintf(buf, size, "%.2f", v);
  else       snprintf(buf, size, "NaN");
}

// Xấp xỉ phân phối chuẩn N(0,1) bằng tổng 3 số ngẫu nhiên đều
float gauss() {
  float s = 0;
  for (int i = 0; i < 3; i++) s += random(-1000, 1001) / 1000.0f;
  return s;
}

// Mức nhiễu giả: 0 = tắt, 1 = nhẹ, 2 = vừa, 3 = mạnh
struct NoiseCfg {
  float sigT, sigH;            // độ lệch chuẩn nhiễu Gauss (°C, %)
  int spikePct, dropPct;       // xác suất xung đột biến, mất mẫu (%)
  float spkTmin, spkTmax;      // biên độ xung nhiệt độ (°C)
  float spkHmin, spkHmax;      // biên độ xung độ ẩm (%)
};
const NoiseCfg NOISE[4] = {
  { 0.00f, 0.0f,  0,  0, 0.0f, 0.0f,  0.0f,  0.0f },
  { 0.15f, 0.5f,  3,  3, 1.5f, 2.5f,  4.0f,  6.0f },
  { 0.30f, 1.0f,  8,  6, 2.0f, 3.5f,  6.0f, 10.0f },
  { 0.50f, 2.0f, 12, 10, 3.0f, 5.0f,  8.0f, 14.0f }
};

// Thêm nhiễu giả: Gauss + xung đột biến + mất mẫu (NaN), theo mức nhiễu.
// Có "đảm bảo": tối thiểu 1 xung mỗi 12 mẫu và 1 lần mất mẫu mỗi 20 mẫu (mỗi đại lượng),
// để trong một lượt đo 30 mẫu chắc chắn thấy cả xung lẫn NaN.
void applyNoise(bool isTemp, float &v, bool &valid) {
  uint8_t lv = noiseLevel;
  if (lv == 0 || lv > 3) return;
  const NoiseCfg &c = NOISE[lv];
  static int sinceSpike[2] = {0, 0}, sinceDrop[2] = {0, 0};
  int k = isTemp ? 0 : 1;
  sinceSpike[k]++; sinceDrop[k]++;

  if (random(100) < c.dropPct || sinceDrop[k] >= 20) {
    sinceDrop[k] = 0; valid = false; v = NAN; return;
  }
  if (!valid) return;
  v += gauss() * (isTemp ? c.sigT : c.sigH);
  if (random(100) < c.spikePct || sinceSpike[k] >= 12) {
    sinceSpike[k] = 0;
    float lo = isTemp ? c.spkTmin : c.spkHmin;
    float hi = isTemp ? c.spkTmax : c.spkHmax;
    float spike = random((long)(lo * 100), (long)(hi * 100) + 1) / 100.0f;
    v += random(2) ? spike : -spike;
  }
  if (!isTemp) v = constrain(v, 0.0f, 100.0f);
}

// Lưu 1 dòng vào bộ nhớ, đẩy lên web và in ra Serial (CSV)
void recordSample(bool isTemp, float raw, float offset, bool valid, unsigned long ts) {
  float conv = valid ? raw + offset : NAN;   // lỗi giữ nguyên NaN, KHÔNG thay bằng 0
  const char* unit = isTemp ? "C" : "%";

  xSemaphoreTake(bufMutex, portMAX_DELAY);
  if (rowCount >= MAX_ROWS) {
    memmove(rows, rows + 1, (MAX_ROWS - 1) * sizeof(Row));
    rowCount = MAX_ROWS - 1;
  }
  Row r = { ts, raw, conv, isTemp, valid };
  rows[rowCount++] = r;
  xSemaphoreGive(bufMutex);

  char rawStr[16], convStr[16];
  formatVal(rawStr, sizeof(rawStr), raw, valid);
  formatVal(convStr, sizeof(convStr), conv, valid);

  char json[200];
  snprintf(json, sizeof(json),
           "{\"type\":\"%s\",\"timestamp\":%lu,\"raw\":\"%s\",\"converted\":\"%s\",\"unit\":\"%s\",\"valid\":%d}",
           isTemp ? "temp" : "hum", ts, rawStr, convStr, unit, valid ? 1 : 0);
  ws.textAll(json);

  Serial.printf("%lu,%s,%s,%s,%d\n", ts, rawStr, convStr, unit, valid ? 1 : 0);
}

// Gửi file CSV của 1 đại lượng (đúng 5 cột theo yêu cầu)
void sendCsv(AsyncWebServerRequest *request, bool isTemp) {
  AsyncResponseStream *response = request->beginResponseStream("text/csv; charset=utf-8");
  response->addHeader("Content-Disposition",
      isTemp ? "attachment; filename=\"dht22_temperature_" MODE_NAME ".csv\""
             : "attachment; filename=\"dht22_humidity_" MODE_NAME ".csv\"");
  response->print("timestamp,raw_value,converted_value,unit,valid\n");

  xSemaphoreTake(bufMutex, portMAX_DELAY);
  for (int i = 0; i < rowCount; i++) {
    if (rows[i].isTemp != isTemp) continue;
    char rawStr[16], convStr[16];
    formatVal(rawStr, sizeof(rawStr), rows[i].raw, rows[i].valid);
    formatVal(convStr, sizeof(convStr), rows[i].conv, rows[i].valid);
    response->printf("%lu,%s,%s,%s,%d\n", rows[i].ts, rawStr, convStr,
                     isTemp ? "C" : "%", rows[i].valid ? 1 : 0);
  }
  xSemaphoreGive(bufMutex);

  request->send(response);
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT)         Serial.printf("Client #%u connected\n", client->id());
  else if (type == WS_EVT_DISCONNECT) Serial.printf("Client #%u disconnected\n", client->id());
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  Serial.println("MODE: " MODE_NAME);
  bufMutex = xSemaphoreCreateMutex();
  randomSeed(esp_random());
  dht.begin();
  pinMode(LED_PIN, OUTPUT);
  pinMode(BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), onButtonIsr, FALLING);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting Wi-Fi...");
  }
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  ws.onEvent(onEvent);
  server.addHandler(&ws);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html; charset=utf-8", index_html);
  });

  server.on("/temp.csv", HTTP_GET, [](AsyncWebServerRequest *request) { sendCsv(request, true); });
  server.on("/hum.csv",  HTTP_GET, [](AsyncWebServerRequest *request) { sendCsv(request, false); });

  // Kết quả thử nút: thời điểm bấm, thời điểm phản hồi, độ trễ
  server.on("/btn.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("text/csv; charset=utf-8");
    response->addHeader("Content-Disposition", "attachment; filename=\"btn_" MODE_NAME ".csv\"");
    response->print("mode,press_ms,response_ms,latency_ms\n");
    xSemaphoreTake(bufMutex, portMAX_DELAY);
    for (int i = 0; i < btnCount; i++) {
      response->printf("%s,%lu,%lu,%lu\n", MODE_NAME, btnEvents[i].press, btnEvents[i].resp,
                       btnEvents[i].resp - btnEvents[i].press);
    }
    xSemaphoreGive(bufMutex);
    request->send(response);
  });

  server.on("/mode", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", MODE_NAME);
  });

  // Mức nhiễu giả: /noise?level=0..3 (không tham số = chỉ đọc trạng thái)
  server.on("/noise", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("level")) {
      int lv = request->getParam("level")->value().toInt();
      if (lv >= 0 && lv <= 3) noiseLevel = lv;
    }
    request->send(200, "text/plain", String(noiseLevel));
  });

  // Xóa dữ liệu, đo lại từ đầu
  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
    xSemaphoreTake(bufMutex, portMAX_DELAY);
    rowCount = 0;
    btnCount = 0;
    xSemaphoreGive(bufMutex);
    ws.textAll("{\"type\":\"reset\"}");
    request->send(200, "text/plain", "OK");
  });

  server.begin();
}

// ================== HÀM DÙNG CHUNG (giống hệt nhau ở cả hai bản) ==================
// Đọc DHT22 + nhiễu giả + lưu/gửi 1 mẫu
void sampleOnce() {
  unsigned long ts = millis();               // timestamp = millis() tại thời điểm đo

  // force=true: luôn đọc cảm biến thật, tránh thư viện trả giá trị cũ trong bộ nhớ đệm
  float t = dht.readTemperature(false, true);
  float h = dht.readHumidity();              // dùng kết quả vừa đọc, cùng thời điểm với t
  bool tOk = !isnan(t);
  bool hOk = !isnan(h);

  applyNoise(true,  t, tOk);
  applyNoise(false, h, hOk);

  recordSample(true,  t, TEMP_OFFSET, tOk, ts);
  recordSample(false, h, HUM_OFFSET,  hOk, ts);
}

// Phản hồi nút bấm: lấy các lần nhấn đã được ISR ghi lại, in ra thời điểm bấm / phản hồi / độ trễ
void buttonTask() {
  while (qTail != qHead) {
    unsigned long press = pressQ[qTail];
    qTail = (qTail + 1) % PRESS_Q;
    unsigned long resp = millis();
    xSemaphoreTake(bufMutex, portMAX_DELAY);
    if (btnCount < MAX_BTN) { btnEvents[btnCount].press = press; btnEvents[btnCount].resp = resp; btnCount++; }
    xSemaphoreGive(bufMutex);
    Serial.printf("[BTN] press=%lu ms, response=%lu ms, latency=%lu ms\n", press, resp, resp - press);
  }
}

// ================== LOOP (BẢN A: delay) ==================
void loop() {
  ws.cleanupClients();

  sampleOnce();
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));   // LED chỉ đổi được 1 lần / vòng (~2 s)
  buttonTask();

  delay(SAMPLE_INTERVAL);                         // chặn toàn bộ loop 2 giây
}
