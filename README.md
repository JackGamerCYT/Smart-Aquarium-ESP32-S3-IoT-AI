# Smart Aquarium v4.0: điều khiển nhiệt PID bằng MOSFET

Hệ thống giữ nhiệt độ nước bằng thuật toán PID, dùng 2 MOSFET IRLZ44N: một con điều khiển sò lạnh Peltier 12V, một con điều khiển lõi sưởi 5V. Ngoài ra có cho cá ăn bằng servo, màn OLED hiển thị tại chỗ, và giám sát/điều khiển từ xa qua MQTT, backend Render + Postgres Neon và dashboard trên Vercel.

> Môn Embedded System Design (GVHD Huỳnh Hoàng Hà, HCMUTE). Linh kiện đúng theo sơ đồ `hehehehe.pdsprj`, **không thêm linh kiện nào**.

```text
aquarium_v4/
├── firmware/
│   ├── smart_aquarium_pid/     firmware chính (PID + MQTT + OLED + cho ăn)
│   ├── test_2_mosfet/          test Q1, Q2 (bật 3s, tắt 3s; lệnh 1/2/0/a để giữ trạng thái)
│   ├── test_ds18b20/           test cảm biến ở GPIO6
│   ├── test_oled/              test OLED 0x3C
│   └── test_servo/             test servo cho ăn ở GPIO7
├── backend/                    Node: nghe MQTT, ghi Postgres, REST API, trợ lý dữ liệu
│   └── device_sim.js           mô phỏng ESP32 để test web khi chưa có phần cứng
├── index.html                  Web dashboard v6 (deploy lên Vercel)
├── render.yaml · vercel.json · .env.example
```

---

## 1. Nối chân (theo sơ đồ)

| Chân ESP32-S3 | Cổng trên mạch | Nối với |
|---|---|---|
| GPIO4 (IN1) | R1 2K2 → Gate Q1 | **SL**: sò lạnh Peltier 12V, điều khiển bằng PWM |
| GPIO5 (IN2) | R2 2K2 → Gate Q2 | **TO**: lõi sưởi 5V, điều khiển bằng PWM |
| GPIO6 | SERVO 2 | **DS18B20**: chân 3 DATA, chân 2 VCC (3V3), chân 1 GND |
| GPIO7 | SERVO 1 | **Servo MG90S cho ăn** |
| GPIO8 / GPIO9 | U2, TGT | **OLED 0.96" 0x3C** (SDA / SCL) |
| GPIO0 | nút BOOT có sẵn trên DevKit | Bấm để cho ăn ngay |

### ⚠️ 3 lỗi trên sơ đồ/mạch phải sửa, nếu không thì không chạy

Cả 3 cách sửa dưới đây đều chỉ là tháo linh kiện hoặc đổi dây, không thêm linh kiện mới.

1. **MOSFET đang nối sai kiểu.** Tải đang nằm ở chân Source và R3/R4 2K2 nằm nối tiếp ở chân Drain, nên dòng qua tải chỉ khoảng 5 mA. Sửa từng con như sau:
   - Tháo R3 (và R4). Hai lỗ hàn còn lại dùng để nối tải: tải (+) vào lỗ phía 12V (5V với con sưởi), tải (−) vào lỗ phía chân D.
   - Chập 2 chân domino SL (và TO) để chân S nối thẳng xuống GND.
   - Kết quả: `12V → tải → Drain Q1 → Source → GND`. Với sò lạnh khoảng 5 A thì hàn dây to thẳng vào chân D, đừng để dòng lớn chạy qua lỗ hàn nhỏ.
2. **Servo đang lấy nguồn từ chân 3V3 của DevKit.** MG90S cần 4,8–6V, và mạch ổn áp 3V3 trên DevKit không chịu nổi dòng servo, ESP32 sẽ tự reset. Chuyển dây chân 2 cổng **SERVO 1** sang đường **5V** (ngõ ra LM2596).
   - Cổng **SERVO 2** đang cắm DS18B20 thì giữ nguyên 3V3. 3V3 là đúng cho cảm biến.
3. **OLED đang cấp 5V.** Đo chân SDA/SCL lúc không có dữ liệu: nếu ra khoảng 5V thì chuyển VCC của OLED sang 3V3, vì chân GPIO8/9 của ESP32 chỉ chịu được 3,3V.

### Lưu ý phần cứng
- Sò lạnh **bắt buộc** có tản nhiệt và quạt ở mặt nóng. Mạch không có kênh điều khiển quạt, nên cắm quạt thẳng vào 12V để quạt chạy liên tục.
- Q1 dẫn khoảng 5 A khi gate chỉ được kích 3,3V, tỏa khoảng 1,5 W nhiệt, nên phải gắn tản nhiệt cho Q1.
- Lõi sưởi dùng chung LM2596 với ESP32. Firmware tăng công suất từ từ (tối đa 20%/giây) để nguồn 5V không bị sụt. Nếu vẫn thấy reset thì giảm "Sưởi tối đa" trên web.
- Mạch **không có RTC**: giờ lấy từ NTP khi có Internet. Nếu mất điện rồi khởi động lại mà không có mạng, lịch cho ăn sẽ tạm dừng; bấm **"Đồng bộ giờ từ trình duyệt"** trên web để chạy lại.

---

## 2. Thuật toán điều khiển

```
e = Nhiệt độ đặt − Nhiệt độ đo            (e > 0: nước lạnh hơn mức đặt)
u = Kp·e + Ki·∫e dt − Kd·dT/dt   ∈ [−100 … +100] %
u > +3%  → lõi sưởi PWM = u
u < −3%  → sò lạnh PWM = |u|
```
- **Một bộ PID duy nhất** cho cả sưởi và làm lạnh, nên sò và sưởi **không bao giờ** chạy cùng lúc.
- **Vùng chết (mặc định ±0,1°C):** sai số nhỏ hơn mức này coi như đã đạt, khâu I giữ nguyên công suất đang cần.
- **Chống vọt lố:**
  - Chỉ cộng dồn tích phân khi sai số nhỏ hơn 2°C.
  - Ngừng cộng dồn khi đầu ra đã đạt giới hạn (bão hòa).
  - Khâu D tính theo nhiệt độ đo, không theo sai số, nên đổi nhiệt độ đặt không làm đầu ra giật.
- **An toàn**, áp dụng cả ở chế độ MANUAL:

| Tình huống | Firmware xử lý |
|---|---|
| Mất cảm biến (3 lần đọc lỗi hoặc 5 giây không có số mới) | Tắt cả sò và sưởi |
| Nước ≥ 31°C | Cấm sưởi, sò chạy tối đa |
| Nước ≤ 20°C | Cấm làm lạnh, sưởi chạy tối đa |
| AUTO, nước > nhiệt độ đặt + 1,5°C | Cấm sưởi |
| AUTO, nước < nhiệt độ đặt − 1,5°C | Cấm làm lạnh |

- **Tham số mặc định:** Kp = 60, Ki = 0,01, Kd = 0. Bộ này được chọn bằng cách mô phỏng bể 4,5 L (sò làm lạnh khoảng 20 W, sưởi khoảng 8 W). Kết quả mô phỏng: hạ từ 30°C về 26°C không vọt lố quá 0,1°C; khi phòng nóng lên 32°C thì nước lệch tối đa khoảng 0,5°C.
- **Chế độ:**
  - AUTO: PID tự điều khiển.
  - MANUAL: đặt % tay cho sò hoặc sưởi, tự về AUTO sau 30 phút.
  - OFF: tắt cả hai.
- **PWM:** tần số 200 Hz, độ phân giải 10 bit. Để tần số thấp vì gate nối qua điện trở 2K2 nên MOSFET chuyển mạch chậm, tần số cao sẽ làm MOSFET nóng.

### Chỉnh PID (mục "Tham số PID" trên web)
1. Đặt Ki = 0, Kd = 0. Tăng Kp đến khi nhiệt độ dao động nhẹ quanh điểm đặt, rồi giảm Kp xuống còn khoảng 60%.
2. Tăng Ki từ 0,005 để xóa sai lệch còn lại. Nếu vọt lố nhiều thì giảm Ki.
3. Kd thường để 0, vì nước đổi nhiệt rất chậm và DS18B20 đo theo bước 0,0625°C làm đạo hàm bị nhiễu.
4. So sánh các bộ tham số bằng ô **"Sai số bám (AUTO)"** và **"% thời gian trong ±0,5°C"** ở mục Thống kê.

---

## 3. Nạp firmware

1. Arduino IDE → Boards Manager → **esp32 by Espressif** (bản 3.x hoặc 2.x đều được) → board **ESP32S3 Dev Module**.
2. Library Manager → cài **PubSubClient** (Nick O'Leary) và **ArduinoJson** (bản 7). Không cần thư viện servo, OLED hay DS18B20.
3. Mở `firmware/smart_aquarium_pid/smart_aquarium_pid.ino`, sửa `WIFI_SSID`, `WIFI_PASS`, `TOPIC_BASE` rồi nạp.
4. Nên kiểm tra từng phần trước bằng các sketch `test_*`.

**Lệnh qua Serial Monitor (115200):**

| Lệnh | Tác dụng |
|---|---|
| `status` | In trạng thái |
| `auto` · `manual` · `off` | Đổi chế độ |
| `sp 26.5` | Đặt nhiệt độ |
| `cool 50` · `heat 30` | Chạy tay sò / sưởi theo % |
| `kp 60` · `ki 0.01` · `kd 0` · `db 0.1` | Chỉnh tham số PID |
| `feed` | Cho ăn ngay |
| `sched 07:00,17:30` | Đặt lịch cho ăn |
| `pidreset` | Xóa khâu tích phân |
| `ds` | Dò lại cảm biến |
| `oled ssd1306` · `oled sh1106` · `oled reset` | Chọn chip / khởi động lại OLED |

Màn OLED hiển thị: giờ và trạng thái kết nối (W+ Wi-Fi, M+ MQTT, T+ đã có giờ) · nhiệt độ chữ to · nhiệt độ đặt và chế độ · % sò / sưởi · đầu ra PID · số lần ăn và giờ ăn kế tiếp · cảnh báo hoặc IP.

---

## 4. Backend (Render + Neon) và web (Vercel)

1. **Neon:** tạo database, lấy chuỗi *Pooled connection* (có `?sslmode=require`).
2. **Render:** New → Blueprint → chọn repo (dùng file `render.yaml`) → nhập `DATABASE_URL`. Bảng được tạo tự động. Nếu database đang có dữ liệu của bản relay cũ, backend chỉ **thêm cột mới**, không xóa dữ liệu cũ.
3. **Vercel:** deploy thư mục gốc (web tĩnh `index.html`). Sửa `DEFAULT_API` trong `index.html` thành URL Render của nhóm, hoặc nhập URL ở mục Cài đặt trên web.
4. `TOPIC_BASE` phải trùng nhau ở cả 3 nơi: firmware, biến `MQTT_TOPIC_BASE` trên Render, và `TOPIC_DEFAULT` trong web.

**Test không cần phần cứng:** chạy `cd backend && npm install && node device_sim.js`. Chương trình giả lập ESP32 với đúng giao thức MQTT, kèm mô hình nhiệt của bể và bộ PID giống firmware.

### Giao thức MQTT (`<TOPIC_BASE>/…`)

| Topic | Hướng | Nội dung |
|---|---|---|
| `telemetry` | ESP32 → | Mỗi 2 giây gửi: `temp, sp, mode, cool, heat, pid_u/p/i/d, kp, ki, kd, db, cool_max, heat_max, feed_today, feed_count, next_feed, schedule, time, time_src, alarm, ds_ok, oled, rssi, seq, ts, buf_count, safe_mode…`. Khi mất mạng thì đệm 1 bản ghi/phút (tối đa 4 giờ), có mạng lại thì gửi bù kèm `buffered:true` |
| `command` | → ESP32 | `{"device": …}`, danh sách lệnh ở bảng dưới |
| `ack` | ESP32 → | `{id, device, ok, msg}` |
| `event` | ESP32 → | `feed, alarm, recovered, mode, setpoint, pid, schedule, time_sync, boot…` |
| `status` | ESP32 → | `online` / `offline` (retain, LWT) |

Các lệnh gửi qua topic `command`:

| `device` | Tham số | Tác dụng |
|---|---|---|
| `mode` | `action`: AUTO / MANUAL / OFF | Đổi chế độ |
| `setpoint` | `value` (20–32) | Đặt nhiệt độ mong muốn |
| `pid` | `kp, ki, kd, db, cool_max, heat_max` | Đổi tham số PID |
| `pid_reset` | – | Xóa khâu tích phân |
| `cool` / `heat` | `value` (0–100) | Chạy tay, chuyển MANUAL 30 phút |
| `feed` | – | Cho ăn ngay |
| `schedule` | `slots` (vd `"07:00,17:30"`) | Đặt lịch cho ăn |
| `time` | `epoch` | Đặt giờ từ trình duyệt |
| `oled` | `action`: SSD1306 / SH1106 / RESET | Chọn chip / khởi động lại OLED |
| `ping` | – | Kiểm tra thiết bị còn trả lời |

### REST API (backend)

| Endpoint | Chức năng |
|---|---|
| `GET /api/health`, `/api/config`, `/api/status` | Tình trạng backend, cấu hình MQTT cho web, trạng thái mới nhất |
| `GET /api/history?hours=` | Lịch sử: nhiệt độ TB/min/max, nhiệt độ đặt, % sò, % sưởi |
| `GET /api/stats?hours=` | Thống kê: sai số bám PID, % thời gian trong ±0,5°C, điện năng sò/sưởi, độ trễ lệnh, tỉ lệ nhận gói |
| `GET /api/feeds`, `/api/energy/daily`, `/api/events`, `/api/commands`, `/api/availability` | Dữ liệu cho biểu đồ và bảng |
| `POST /api/command` | Gửi lệnh (cùng định dạng MQTT); thêm `wait_ack_s` để chờ xác nhận |
| `GET/PUT /api/settings` | Công suất sò, công suất sưởi, giá điện (`cool_w, heat_w, tariff_vnd_per_kwh`) |
| `GET /api/export/{telemetry,events,commands,availability}.csv` | Xuất CSV |
| `POST /api/chat` | Trợ lý dữ liệu: hỏi về nhiệt độ, sai số PID, cho ăn, điện năng, cảnh báo; ra lệnh bằng lời ("đặt nhiệt độ 26", "làm lạnh 40%"…), phải bấm xác nhận mới gửi |

---

## 5. Đã kiểm tra

- Tất cả 5 sketch biên dịch không lỗi, không cảnh báo với **ESP32 core 3.0.7**, board ESP32-S3. Firmware chính chiếm 75% flash và 14% RAM. Phần code riêng cho core 2.x đã đối chiếu với file header chính thức của core 2.0.17.
- Chạy backend thật với PostgreSQL + broker Mosquitto + chương trình giả lập thiết bị:
  - Mọi lệnh đều nhận được ACK, độ trễ khoảng 15–45 ms.
  - Lệnh sai (Kp = 999, lịch `25:00`) bị từ chối đúng.
  - Ghi database, thống kê, CSV và trợ lý dữ liệu đều trả kết quả đúng.
- Kiểm tra web tự động bằng Chromium:
  - Tất cả nút điều khiển hoạt động, lịch sử tải được từ database, không có lỗi JavaScript.
  - Giao diện điện thoại không bị tràn ngang.
- **Chưa chạy trên board thật.** Cần nạp các sketch `test_*` để xác nhận từng phần cứng trước khi chạy firmware chính.
