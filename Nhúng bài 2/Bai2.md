# Bài 2 — Đọc và kiểm tra cảm biến
Họ tên: ... · MSSV: ... · Nhóm: ...

## File trong repo này
- sensor_delay.ino — bản dùng delay()
- sensor_millis.ino — bản dùng millis()
- data.csv — dữ liệu gốc (timestamp, raw, converted, unit, valid)
- chart.png — biểu đồ trước/sau lọc

## 4.1 Cấu hình (setup)
-, Cảm biến: DHT22 (AM2302), đo nhiệt độ và độ ẩm, giao tiếp 1 dây.
-, Chân nối: DATA vào GPIO14 của ESP32 DOIT DEVKIT V1, VCC vào 3,3 V, GND vào GND. Có điện trở kéo lên 10 kΩ giữa DATA và VCC (module 3 chân đã có sẵn). [sửa lại nếu mạch của bạn khác]
-, Vì sao GPIO14: đây là chân số thông thường, không phải chân strapping (không ảnh hưởng quá trình khởi động của ESP32), và dùng chế độ số nên không xung đột với Wi-Fi.
-, Chu kỳ đọc: 2000 ms (0,5 Hz). Lý do:
  +, Datasheet DHT22 quy định tốc độ lấy mẫu tối đa 0,5 Hz, tức hai lần đọc phải cách nhau ít nhất 2 giây. Đọc nhanh hơn sẽ cho NaN hoặc giá trị cũ. Thư viện DHT cũng đặt khoảng tối thiểu 2000 ms. [đối chiếu lại với datasheet bạn đang dùng]
  +,Đề yêu cầu tối thiểu 30 mẫu trong 60 giây liên tục, nên 2 s/mẫu cho đúng 30 mẫu trong 60 s.
  +, Đề nêu DHT11 cần ≥ 1 s, DHT22 chậm hơn nên chu kỳ 2 s đáp ứng được cả hai loại.
=, Timestamp: dùng millis() vì board không có RTC.

## 4.2 Cách báo lỗi
Khi đọc thất bại (mất dây, sai chân, cảm biến không phản hồi), thư viện DHT trả về NaN. Chương trình kiểm tra bằng isnan() rồi:
ghi raw_value = NaN, converted_value = NaN và valid = 0, không thay bằng 0 để khỏi nhầm với giá trị đo thật;
bỏ mẫu đó khỏi bộ lọc, đồ thị đánh dấu bằng vạch đỏ, web hiện “Lỗi (NaN)”.

## 4.3 Bộ lọc đã chọn
Trung bình trượt (MA) và EMA để so sánh, cộng thêm bước Hampel loại xung. Tham số:

MA: cửa sổ N = 5.
EMA: α = 0,3.
Hampel: cửa sổ 7 mẫu, ngưỡng 3.

Ưu điểm:
Giảm nhiễu dao động. Theo lý thuyết với nhiễu trắng, MA (N=5) còn khoảng 0,45σ và EMA (=0,3) còn khoảng 0,42σ. Đo thực tế: nhiệt độ giảm x%, độ ẩm giảm y %.
Hampel loại xung đột biến nên MA/EMA không bị kéo lệch. Đã loại k/n mẫu.
EMA không cần lưu cửa sổ nên tốn ít bộ nhớ hơn MA.

Hạn chế:
Trễ: MA trễ (N−1)/2 = 2 mẫu ≈ 4 s, EMA trễ (1−α)/α ≈ 2,3 mẫu ≈ 4,7 s. Khi giá trị thay đổi thật, đường lọc đổi hướng chậm hơn đường gốc (khi tác động vào cảm biến).
Hampel có thể loại nhầm thay đổi thật kéo dài chỉ 1 đến 2 mẫu, và phải mất 3 mẫu mới công nhận đó là thay đổi thật.
5 mẫu hợp lệ đầu tiên chưa được lọc xung.
Tăng N hoặc giảm α thì mượt hơn nhưng trễ hơn (đánh đổi mượt và trễ).


## 4.4 Kết quả thí nghiệm lập lịch

Hai bản cùng đọc DHT22 mỗi 2 giây, khác nhau duy nhất ở cách chờ: **Bản A** dùng `delay(2000)`, **Bản B** dùng `millis()` (`lastTime += 2000`). Nút BOOT (GPIO0) làm tác vụ thử: ISR ghi `press_ms` (lúc bấm thật), `loop()` ghi `response_ms` (lúc phản hồi), `latency_ms = response_ms − press_ms`. Dữ liệu: `btn_delay.csv` (83 lần bấm) và `btn_millis.csv` (49 lần bấm).

### Độ trễ phản hồi nút bấm

| Chỉ số | Bản A (`delay`) | Bản B (`millis`) |
|---|---|---|
| Trung bình | **986,8 ms** | **0,02 ms** |
| Trung vị | 1050 ms | 0 ms |
| Nhỏ nhất / lớn nhất | 2 / **1961 ms** | 0 / **1 ms** |
| Độ lệch chuẩn | 594,9 ms | 0,14 ms |
| Số lần trễ > 500 ms | 61 / 83 (73,5 %) | 0 / 49 |

### Độ lệch chu kỳ đọc so với 2000 ms

| Chỉ số | Bản A (`delay`) | Bản B (`millis`) |
|---|---|---|
| Chu kỳ thực (min – max) | 2008 – 2042 ms | _(điền)_ |
| Chu kỳ trung bình | **≈ 2026 ms** | _(điền)_ |
| Độ lệch / mẫu | **+26 ms (+1,3 %)** | _(điền)_ |
| Trôi sau 30 mẫu | **≈ +0,78 s** | _(điền)_ |

Số liệu bản A tính từ khoảng giữa các lần chương trình "tỉnh dậy" trong log nút bấm. Số bản B (và đối chiếu bản A) tính từ cột `timestamp` của `dht22_temperature_*.csv`: `dt = timestamp[n] − timestamp[n−1]`.

### Nhận xét

- **Bản B phản hồi nhanh hơn rõ rệt:** độ trễ trung bình từ ≈ 987 ms xuống < 1 ms. Kết quả 0 ms nghĩa là nhỏ hơn 1 ms (giới hạn độ phân giải của `millis()`).
- **Bản A trễ từ 0 đến gần 2 giây, phân bố gần đều:** nút được bấm ở thời điểm ngẫu nhiên trong lúc `delay()` chặn chương trình, nên trung bình ≈ nửa chu kỳ (lý thuyết ≈ 1010 ms, đo 987 ms). 83 lần bấm chỉ được xử lý trong 22 lần "tỉnh dậy", có lô dồn tới 8 lần bấm.
- **Chu kỳ bản A bị trôi:** mỗi vòng mất 2000 ms của `delay()` cộng thêm thời gian đọc cảm biến, ghi dữ liệu và in log (khoảng 11 ms cho mỗi lần bấm được xử lý). Bản B tính mốc kế tiếp từ mốc lý tưởng nên không bị trôi kiểu này.
- **Hạn chế:** hai đợt thử có số lần bấm khác nhau (83 và 49) nên so sánh theo phân bố; ISR chống dội 50 ms; thời gian in log Serial làm `response_ms` của các lần bấm sau trong cùng lô lớn hơn khoảng 11 ms mỗi lần, sai số nhỏ so với độ trễ hàng trăm ms.

### Kết luận

Bản B (millis) phản hồi nhanh hơn rất nhiều (≈ 987 ms xuống < 1 ms), giữ chu kỳ sát 2000 ms và vẫn làm được việc khác trong lúc chờ. Bản A (delay) đơn giản hơn nhưng chương trình "đứng im" mỗi vòng, trễ tới gần 2 giây và trôi thêm khoảng 26 ms mỗi mẫu. `delay()` chỉ hợp chương trình rất nhỏ làm một việc; khi cần phản hồi sự kiện hoặc chạy nhiều tác vụ thì nên dùng `millis()`.

## 4.5 Đóng góp cá nhân
...
