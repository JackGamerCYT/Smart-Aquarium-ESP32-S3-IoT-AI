# Smart Aquarium v5.0: sò lạnh PID + bơm nước bật/tắt riêng

Module sưởi USB 5W tự giữ nước ở khoảng 26 °C, nên ESP32 **không điều khiển sưởi** nữa. ESP32 chỉ còn làm hai việc bằng 2 MOSFET IRLZ44N: điều khiển **sò lạnh Peltier 12V** bằng PID một phía (chỉ làm lạnh), và **bật/tắt động cơ bơm nước** riêng. Ngoài ra vẫn có cho cá ăn bằng servo, màn OLED, và giám sát/điều khiển từ xa qua MQTT, backend Render + Postgres Neon, dashboard trên Vercel.

> Môn Embedded System Design (GVHD Huỳnh Hoàng Hà, HCMUTE). Linh kiện đúng theo sơ đồ `hehehehe.pdsprj`, **không thêm linh kiện nào**.

```text
smart_aquarium_v5/
├── firmware/
│   ├── smart_aquarium_cool/    firmware chính v5 (sò PID + bơm + MQTT + OLED + cho ăn)
│   ├── test_bom_so/            test Q1 (bơm) và Q2 (sò): bật 3s, tắt 3s; lệnh 1/2/0/a
│   ├── test_ds18b20/           test cảm biến ở GPIO6 (cổng SERVO 2)
│   ├── test_oled/              test OLED 0x3C
│   └── test_servo/             test servo cho ăn ở GPIO7
├── backend/                    Node: nghe MQTT, ghi Postgres, REST API, trợ lý dữ liệu
│   └── device_sim.js           mô phỏng ESP32 v5 (có cả thermostat của module sưởi)
├── index.html                  Web dashboard v7 (deploy lên Vercel)
├── render.yaml · vercel.json · .env.example
```

---

## 1. Nối chân (theo PCB)

| Chân ESP32-S3 | Trên mạch | Nối với |
|---|---|---|
| GPIO4 (IN1) | R1 2K2 → Gate Q1, **kênh C1** | **Động cơ bơm nước** (bật/tắt, khởi động mềm ~1 s) |
| GPIO5 (IN2) | R2 2K2 → Gate Q2, **kênh C2** | **Sò lạnh Peltier 12V** (PWM 200 Hz) |
| GPIO6 | cổng SERVO 2 | **DS18B20**: chân 3 DATA, chân 2 VCC (3V3), chân 1 GND |
| GPIO7 | cổng SERVO 1 | Servo MG90S cho ăn |
| GPIO8 / GPIO9 | U2, TGT | OLED 0.96" 0x3C (SDA / SCL) |
| GPIO0 | nút BOOT trên DevKit | Bấm để cho ăn ngay |
| — | ngõ ra 5V của LM2596 | **Module sưởi USB 5W** (tự giữ ~26 °C, ESP32 không điều khiển) |

**Kiểm tra đúng kênh trước khi chạy firmware chính:** nạp `test_bom_so`, mở Serial 115200, gõ `1`. Phải là **bơm** chạy. Nếu sò chạy thì trong `smart_aquarium_cool.ino` đổi `PIN_PUMP` thành 5 và `PIN_COOL` thành 4.

### ⚠️ Đi dây cho v5

1. **Sò lạnh nằm ở kênh C2 (Q2).** Kênh này trước đây nối tải 5V. Dây (+) của sò phải nối vào **12V**, không phải 5V. Sò kéo khoảng 5 A: hàn dây to thẳng vào chân D của Q2, và **gắn tản nhiệt cho Q2** (IRLZ44N tỏa khoảng 1,5 W khi gate chỉ được kích 3,3V).
2. **Bơm nằm ở kênh C1 (Q1).** Dây (+) của bơm nối vào nguồn đúng điện áp của bơm (5V hay 12V). Cực (−) vào lỗ phía chân D của Q1.
3. **Module sưởi USB** lấy 5V từ ngõ ra LM2596, chung với ESP32 và servo. 5 W ở 5V là khoảng 1 A, cộng ESP32 và servo vẫn dưới mức 3 A của LM2596.
4. Các sửa lỗi của bản trước vẫn áp dụng:
   - Tháo R3/R4 nối tiếp ở chân D, chập domino để chân S xuống thẳng GND.
   - Servo lấy 5V (không lấy 3V3).
   - OLED lấy 3V3 nếu SDA/SCL đo ra 5V.
5. Sò **bắt buộc** có tản nhiệt và quạt ở mặt nóng. Quạt cắm thẳng 12V, chạy liên tục.

---

## 2. Thuật toán điều khiển

```
e = Nhiệt độ đặt − Nhiệt độ đo              (e < 0: nước nóng hơn mức đặt)
u = Kp·e + Ki·∫e dt − Kd·dT/dt   ∈ [−cool_max … 0] %
u < −3%  → sò lạnh PWM = |u|
```
- **Chỉ làm lạnh.** Tích phân bị kẹp trong [−cool_max, 0]. Khi nước lạnh hơn mức đặt (lúc sưởi USB đang làm ấm), tích phân chỉ xả về 0, không tích dương. Vì vậy khi nước nóng lên lại, sò phản ứng ngay.
- **Không "đánh nhau" với sưởi.** Nhiệt độ đặt tối thiểu = nhiệt sưởi USB + 0,5 °C (mặc định 26 + 0,5 = **26,5 °C**, nhiệt đặt mặc định **27 °C**).
  - Nhiệt sưởi USB chỉnh được trên web cho khớp với module thật.
  - Nếu tăng nhiệt sưởi làm mức đặt hiện tại thấp hơn giới hạn, firmware tự nâng mức đặt lên.
- **Vùng chết, chống vọt lố, khâu D theo nhiệt độ đo, tăng công suất tối đa 20%/s:** giữ như v4.
- **Bơm:** BẬT/TẮT riêng, trạng thái được lưu vào bộ nhớ (mất điện bật lại vẫn giữ). Khởi động mềm 0 → 100% trong khoảng 1 s để giảm dòng khởi động.
- **Khóa sò theo bơm** (`cool_need_pump`, mặc định BẬT): tắt bơm thì sò tạm khóa. Nên giữ BẬT nếu sò làm lạnh qua đường nước mà bơm đẩy; tắt đi nếu tấm lạnh của sò ngâm thẳng trong bể.

| Tình huống | Firmware xử lý |
|---|---|
| Sò ở chế độ OFF | Sò tắt hẳn, kể cả khi nước ≥ 31 °C. Bơm không bị ảnh hưởng |
| Mất cảm biến (3 lần đọc lỗi hoặc 5 s không có số mới) | Tắt sò. Bơm vẫn chạy |
| Bơm tắt và đang bật khóa sò theo bơm | Tắt sò (`cool_block = PUMP_OFF`) |
| Nước ≥ 31 °C (AUTO/MANUAL) | Sò chạy tối đa, báo `TEMP_HIGH` |
| Nước ≤ 20 °C | Tắt sò, báo `TEMP_LOW` (module sưởi USB có thể hỏng) |
| AUTO, nước < nhiệt đặt − 1,5 °C | Không làm lạnh |

- **Chế độ sò:** AUTO (PID) · MANUAL (đặt % tay, tự về AUTO sau 30 phút) · OFF.
- **Tham số mặc định:** Kp = 60, Ki = 0,01, Kd = 0, vùng chết 0,1 °C, sò tối đa 100%.

### Chỉnh PID (mục "Tham số PID" trên web)
1. Đặt Ki = 0, Kd = 0. Tăng Kp đến khi nhiệt độ dao động nhẹ quanh điểm đặt, rồi giảm Kp còn khoảng 60%.
2. Tăng Ki từ 0,005 để xóa sai lệch còn lại. Nếu vọt lố nhiều thì giảm Ki.
3. Kd thường để 0.
4. So sánh các bộ tham số bằng ô "Sai số bám (AUTO)" và "% thời gian trong ±0,5 °C".

---

## 3. Nạp firmware

1. Arduino IDE → Boards Manager → **esp32 by Espressif** (bản 3.x hoặc 2.x) → board **ESP32S3 Dev Module**.
2. Library Manager → cài **PubSubClient** (Nick O'Leary) và **ArduinoJson** (bản 7).
3. Chạy `test_bom_so` trước để xác nhận kênh bơm/sò (mục 1).
4. Mở `firmware/smart_aquarium_cool/smart_aquarium_cool.ino`, sửa `WIFI_SSID`, `WIFI_PASS`, `TOPIC_BASE` rồi nạp.

**Lệnh qua Serial Monitor (115200):**

| Lệnh | Tác dụng |
|---|---|
| `status` | In trạng thái |
| `auto` · `manual` · `off` | Đổi chế độ sò lạnh |
| `sp 27` | Đặt nhiệt độ làm lạnh (≥ nhiệt sưởi + 0,5) |
| `cool 50` | Chạy tay sò 50% (MANUAL 30 phút) |
| `pump on` · `pump off` | Bật / tắt bơm |
| `hsp 26` | Khai báo nhiệt sưởi USB |
| `needpump 1` · `needpump 0` | Bật / tắt khóa sò theo bơm |
| `kp 60` · `ki 0.01` · `kd 0` · `db 0.1` | Chỉnh PID |
| `feed` · `sched 07:00,17:30` · `pidreset` · `ds` · `oled …` | Như bản trước |

Màn OLED dòng 5 hiển thị `So xx%  Bom BAT/TAT`.

---

## 4. Backend (Render + Neon) và web (Vercel)

1. **Neon:** tạo database, lấy chuỗi *Pooled connection* (có `?sslmode=require`).
2. **Render:** New → Blueprint → chọn repo (dùng file `render.yaml`) → nhập `DATABASE_URL`. Bảng được tạo tự động. Database cũ (bản relay / v4 PID) dùng tiếp được: backend chỉ **thêm cột `pump` và thông số `pump_w`**, không xóa dữ liệu cũ.
3. **Vercel:** deploy thư mục gốc (web tĩnh `index.html`). Sửa `DEFAULT_API` trong `index.html` thành URL Render của nhóm, hoặc nhập URL ở mục Cài đặt trên web.
4. `TOPIC_BASE` phải trùng nhau ở cả 3 nơi: firmware, biến `MQTT_TOPIC_BASE` trên Render, và `TOPIC_DEFAULT` trong web.

**Test không cần phần cứng:** chạy `cd backend && npm install && node device_sim.js`. Chương trình giả lập ESP32 với đúng giao thức MQTT, kèm mô hình nhiệt của bể và bộ PID giống firmware.

### Giao thức MQTT (`<TOPIC_BASE>/…`)

| Topic | Hướng | Nội dung |
|---|---|---|
| `telemetry` | ESP32 → | Mỗi 2 s: `temp, sp, mode, cool, pump, pump_pct, cool_block, heater_sp, sp_min, sp_max, cool_need_pump, pid_u/p/i/d, kp, ki, kd, db, cool_max, feed_*, alarm, rssi, seq, ts, buf_count, safe_mode…` |
| `command` | → ESP32 | `{"device": …}`, xem bảng dưới |
| `ack` | ESP32 → | `{id, device, ok, msg}` |
| `event` | ESP32 → | `feed, alarm, recovered, mode, setpoint, pid, pump, schedule, time_sync, boot…` |
| `status` | ESP32 → | `online` / `offline` (retain, LWT) |

| `device` | Tham số | Tác dụng |
|---|---|---|
| `mode` | `action`: AUTO / MANUAL / OFF | Chế độ sò lạnh |
| `setpoint` | `value` (nhiệt sưởi + 0,5 … 32) | Nhiệt độ làm lạnh |
| `pid` | `kp, ki, kd, db, cool_max, heater_sp (18–30), cool_need_pump` | Tham số |
| `pid_reset` | – | Xóa tích phân |
| `cool` | `value` (0–100) | Chạy tay sò, MANUAL 30 phút |
| `pump` | `action`: ON / OFF / TOGGLE | Bật / tắt bơm |
| `feed`, `schedule`, `time`, `oled`, `ping` | như bản trước | |

### REST API (backend)

Các endpoint giữ như bản trước. Những chỗ thay đổi:
- `/api/history` trả `cool` và `pump` (% thời gian bơm bật).
- `/api/stats` trả `pump_on_pct`, `pump_runtime_min`, `pump_wh`, `events.pump_switches`.
- `/api/settings` dùng các khóa `cool_w`, `pump_w`, `tariff_vnd_per_kwh`.
- Điện năng không tính module sưởi USB, vì nó tự bật/tắt và ESP32 không biết lúc nào nó chạy.

---

## 5. Đã kiểm tra (v5)

- **Biên dịch:** firmware chính và `test_bom_so` biên dịch với ESP32 core 3.0.7, board ESP32-S3: 0 lỗi, 0 cảnh báo. Firmware chính chiếm 75% flash và 14% RAM.
- **Nâng cấp database:** chạy backend v5 trên database đang có dữ liệu v4. Bảng cũ giữ nguyên, cột `pump` và thông số `pump_w` được thêm tự động.
- **Chạy thật backend + Postgres + Mosquitto + bộ giả lập v5:**
  - Mọi lệnh đều có ACK, độ trễ khoảng 20–45 ms.
  - Tắt bơm thì sò bị khóa (`PUMP_OFF`).
  - Các lệnh sai bị từ chối: nhiệt đặt 26 °C khi sưởi 26 °C, `heater_sp = 35`, `pump MAYBE`, `heat`.
  - Tăng nhiệt sưởi thì mức đặt tự được nâng lên.
- **Kiểm tra web tự động bằng Chromium:**
  - Nút bơm, nút sò, đặt nhiệt, chạy tay, lưu tham số, thống kê, lịch sử và trợ lý đều hoạt động.
  - Không có lỗi JavaScript; giao diện điện thoại không bị tràn ngang.
- **Chưa chạy trên board thật.**
