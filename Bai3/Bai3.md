# Bài 3 — Điều khiển thiết bị an toàn
Họ tên: ... · MSSV: ... · Nhóm: ...


## File trong repo này
- control.ino — firmware máy trạng thái DỪNG/CHẠY/LỖI (có ISR + timer), kèm giao diện web WebSocket
- fsm_diagram.png — sơ đồ trạng thái
- serial_log.txt — log Serial của cả 3 ca
![Sơ đồ trạng thái](Bai3/fsm_diagram.png)

## 4.1 Cấu hình phần cứng

Tải được chọn là **máy bơm DC** gắn vào cổng DC MOTOR 2 (đầu nối J10). Đường điều khiển là **GPIO23 (MOTOR2_INT1) và GPIO22 (MOTOR2_INT2) của ESP32-DevKitC-32E → điện trở nối tiếp 22 Ω (R33, R34) → IC cầu H A4950ELJTR (U3) → bơm DC**. A4950 là driver cầu H có MOSFET tích hợp (đã có sẵn diode thân dập xung), nên GPIO không điều khiển trực tiếp tải, đúng yêu cầu của đề. Nguồn cấp cho tải là đường **VDC** vào chân VBB của A4950 (điện áp: 12V), tách riêng với nguồn +5 V của vi điều khiển; chân VREF nối +5 V và điện trở cảm biến dòng R32 = 0,2 Ω ở chân LSS. Firmware chỉ chạy bơm một chiều: GPIO23 xuất PWM 20 kHz (8 bit), GPIO22 luôn ở mức thấp. Trạng thái an toàn là IN1 = IN2 = LOW (bơm tắt). Em chọn kênh MOTOR 2 thay vì MOTOR 1 vì MOTOR 1 dùng GPIO5, là chân strapping bị kéo lên cao và phát xung lúc khởi động, có thể làm bơm giật một nhịp khi cấp nguồn; GPIO22 và GPIO23 không có hạn chế này. Trong `setup()`, hai chân điều khiển được đưa về LOW ngay dòng đầu tiên, trước khi khởi tạo Serial, PWM, WiFi hay timer. Cảm biến nhiệt độ – độ ẩm đọc trên GPIO14 (DATA_RH).
## 4.2 Ngắt ngoài

Firmware có một ISR là `onTimer()` của timer phần cứng, và nó làm **đúng một việc**: tăng biến đếm `tickCount` và ghi mốc `micros()` vào `tickTimeUs` (trong vùng găng, không `Serial.print`, không `delay()`). Toàn bộ xử lý (tính chu kỳ, thống kê, log) nằm ở hàm `processTimerStats()` ngoài ISR.

Không có nút bấm vật lí

## 4.3 Timer phần cứng và PWM

**Timer.** Timer phần cứng được đặt chu kỳ **100 ms**. Để so sánh, em chạy song song một sự kiện 100 ms bằng `millis()` kiểu Bài 2 (`if (millis() - last >= 100) { last = millis(); ... }`). Cả hai đều đóng dấu `micros()` và đo khoảng cách giữa hai sự kiện liên tiếp. Số liệu lấy từ log Serial sau khoảng 90 s chạy (log in bằng µs, trong bảng đã quy đổi sang ms, làm tròn 4 chữ số thập phân):

| Chỉ tiêu | Timer phần cứng | millis() |
|---|---|---|
| Số mẫu | 790 | 791 |
| Chu kỳ trung bình (ms) | 100,0000 | 103,6527 |
| Lệch so với giá trị đặt | 0,0000 % | 3,6527 % |
| Min (ms) | 99,9980 | 99,2680 |
| Max (ms) | 100,0020 | 2834,4030 |
| Jitter max − min (ms) | 0,0040 | 2735,1350 |
| Độ lệch chuẩn σ (ms) | 0,0003 | 97,2634 |
| Số mẫu lệch > 1 ms | 0 | 3 |
| Tick bị lỡ | 27 | — |

Chu kỳ timer phần cứng đo thực tế **lệch 0,0000 % (trung bình đúng 100,0000 ms, dao động chỉ ±0,002 ms)** so với giá trị đã đặt. Bản dùng `millis()` lệch 3,65 %, chủ yếu do một lần `loop()` bị chặn khoảng 2,8 s: giá trị Max 2834,403 ms của `millis()` khớp với 27 tick timer bị vòng lặp bỏ lỡ (27 × 100 ms ≈ 2,7 s). Trong lúc đó ISR của timer vẫn chạy đều, còn đoạn `millis()` phụ thuộc vào việc `loop()` được gọi kịp. Phần lệch còn lại (khoảng 0,2 ms mỗi chu kỳ) do độ phân giải 1 ms của `millis()` và cách đặt `last = millis()` làm chu kỳ trôi dần. Nguyên nhân lần chặn 2,8 s chưa xác định: **[CẦN ĐIỀN: ghi lại sau khi chạy lại bằng lệnh CMPRESET, nêu thời điểm xảy ra và việc đang làm lúc đó, ví dụ mở web hay gửi lệnh]**. Nhận xét: timer phần cứng ổn định hơn nhiều vì không phụ thuộc vào thời gian xử lý của vòng lặp chính.

**PWM.** PWM chạy ở 20 kHz, 8 bit trên GPIO23, đổi duty bằng lệnh `PWM <1-100>` hoặc thanh trượt trên web. Ba mức duty đã thử:

| Duty | Điện áp TB lý thuyết trên GPIO23 (3,3 V × duty) |  Quan sát tốc độ / lưu lượng bơm |
|---|---|---|
| 25 % | ≈ 0,83 V | Chậm |
| 50 % | ≈ 1,65 V | Bình thường |
| 90 % | ≈ 2,97 V | Nhanh |


## 4.4 Kết quả ba ca thử

Giới hạn thời gian chạy đã chọn là **20 s** (`MAX_RUN_MS`, nằm trong khoảng 10–30 s đề gợi ý), duty mặc định 90 %. Hệ có thêm cooldown 5 s sau mỗi lần về DỪNG và giữ trạng thái LỖI 3 s trước khi về DỪNG. 

| Ca | Đầu vào | Kỳ vọng | Đạt? |
|---|---|---|---|
| 1. Bình thường | Gửi `ON` khi đang DỪNG, hết cooldown, DHT đọc hợp lệ | Log `-> CHAY (duty=90%, toi da 20 s)`, bơm chạy | Đạt |
| 2. Quá giờ | Giữ CHẠY, không gửi `OFF` | Sau 20 s log `-> DUNG (het gio)`, bơm tắt | Đạt |
| 3. Dữ liệu lỗi | Đang CHẠY, rút cảm biến DHT (đọc ra NaN) hoặc gửi lệnh sai định dạng | Log `-> LOI (cam bien NaN)` hoặc `-> LOI (lenh sai dinh dang)`, bơm tắt ngay, sau 3 s log `-> DUNG (het loi, ve trang thai an toan)` |  Đạt |

Quy tắc "LỖI hoặc hết giờ luôn về DỪNG" được đảm bảo trong `updateState()`: điều kiện lỗi và hết giờ được kiểm tra trước, lệnh `ON` không ghi đè được, và ngoài trạng thái CHẠY thì tải luôn bị ép tắt. Lệnh `ON` gửi khi đang CHẠY, đang LỖI hoặc đang cooldown bị từ chối và có log `TU CHOI: ...`. 

**Rủi ro phát hiện khi đo timer:** lần `loop()` bị chặn ~2,8 s ở mục 4.3 cho thấy máy trạng thái (chạy trong `loop()`) có thể chậm kiểm tra hết giờ hoặc lỗi cảm biến trong lúc bị chặn, nghĩa là bơm có thể chạy quá 20 s tới vài giây. Hướng khắc phục dự kiến: cho ISR của timer phần cứng cắt bơm khi quá giờ, độc lập với `loop()`. 

## 4.5 Đóng góp cá nhân


## Phụ lục: cách chạy firmware
- ESP32 Arduino core 3.x; thư viện **DHT sensor library** + **Adafruit Unified Sensor**, và **WebSockets** (Markus Sattler).
- Sửa `WIFI_SSID` / `WIFI_PASS` đầu `control.ino`. Nếu không kết nối được WiFi sau 10 s, board tự phát WiFi `ESP32-Pump`.
- Mở `http://<IP>/` hoặc `http://esp32-pump.local/` để xem máy trạng thái, chỉnh duty, xem log và bảng so sánh timer/millis().
- Lệnh (Serial 115200 hoặc ô lệnh trên web): `ON`, `OFF`, `PWM <1-100>`, `STATUS`, `CMP` (in bảng so sánh), `CMPRESET` (đo lại từ đầu).
