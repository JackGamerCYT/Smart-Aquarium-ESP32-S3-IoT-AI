# Firmware – ESP32-S3 (v3.2.0)

## Cài đặt
1. Arduino IDE → *Preferences* → Board URL: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. *Boards Manager* → cài **esp32 by Espressif** (2.0.x hoặc 3.x) → chọn **ESP32S3 Dev Module**
   (PSRAM: *OPI PSRAM* cho N8R8/N16R8 · Flash size theo board)
3. *Library Manager* cài: `PubSubClient`, `ArduinoJson` (v7), `OneWire`, `DallasTemperature`, `ESP32Servo`, `RTClib` (Adafruit).
   **Màn LCD không cần thư viện** – driver đã viết sẵn trong firmware. (Không cần U8g2 / LiquidCrystal_I2C / Adafruit GFX.)
4. Wi-Fi `HO TRO SINH VIEN`, topic `smartaquarium_node2026` đã điền sẵn. Giữ `DB_ENABLED = false` (backend Render ghi database).
5. Cắm USB vào cổng **UART/COM** → Upload → Serial Monitor **115200**.

## Thư mục
| Sketch | Dùng khi |
|---|---|
| `smart_aquarium_esp32s3/` | Firmware chính |
| `test_lcd/` | Kiểm tra riêng màn LCD (tô kín ô, hiện chữ 4 dòng) |
| `test_tim_chan_i2c/` | Tự dò LCD/RTC đang thật sự cắm ở chân nào, đo điện dây SDA/SCL |
| `test_ds18b20/` | Web báo **Mất cảm biến / ERR**: kiểm tra trở kéo, dò chân, đọc 20 lần đếm lỗi |

## Sơ đồ chân (giữ nguyên mạch cũ)
| Chức năng | GPIO | Ghi chú |
|---|---|---|
| LCD + DS3231 – SDA / SCL | **8 / 9** | LCD cấp **5V** (qua module chuyển mức BSS138 nếu có), DS3231 cấp 3V3 |
| DS18B20 | 4 | ĐỎ→3V3 · ĐEN→GND · VÀNG→GPIO4 · **trở 4.7k giữa VÀNG và ĐỎ** (kéo nội chỉ dùng tạm) |
| DS3231 SQW | 7 | Tùy chọn |
| Relay sò / bơm | 19 / 18 | Active-LOW |
| Quạt MOSFET / Servo / Còi | 15 / 13 / 12 | |
| Nút BOOT / Cho ăn | 0 / 16 | Giữ BOOT 2 s = Self-Test |

## Màn LCD I2C
- **LCD 1602 (16x2):** tự lật trang mỗi 3 s — giờ & nhiệt độ → sò/bơm & lịch cho ăn → (cảnh báo nếu có).
- **LCD 2004 (20x4):** hiện đủ trên 1 trang.
- Đổi loại màn **không cần nạp lại**: gõ `lcd 1602` hoặc `lcd 2004` ở Serial, hoặc bấm nút trên web (lưu NVS).

| Hiện tượng | Nguyên nhân | Cách sửa |
|---|---|---|
| Đèn nền sáng, không có chữ / chỉ thấy ô đen | Tương phản | **Vặn biến trở xanh** sau lưng mạch I2C |
| Đèn nền không sáng | Thiếu 5V/GND, hoặc mất jumper LED | Kiểm tra VCC=5V, cắm jumper "LED" sau mạch I2C |
| Serial `[I2C] ... KHONG CO` | Dây / nguồn | SDA→GPIO8, SCL→GPIO9 (hay bị đảo), GND chung |
| Serial `[LCD] KHONG thay LCD` nhưng có `68` | Dây riêng của LCD | Nạp `test_tim_chan_i2c.ino` |
| Chữ bị cắt / sai dòng | Chọn sai kích thước | `lcd 1602` / `lcd 2004` |

## Lệnh Serial Monitor (Newline)
`so on` · `so off` · `bom on` · `bom off` · `an` · `auto` · `test` · `status` · `lcd test` · `lcd 1602` · `lcd 2004` · `i2c`

## Cấu trúc code
| Mục | Hàm chính |
|---|---|
| Thời gian thực | `initTime()`, `handleTimeSync()`, `seedSystemFromRtc()`, `writeRtcFromSystem()` |
| Nhiệt độ | `updateSensor()`, `controlTemperature()`, `setChiller()` |
| Cho ăn | `startFeeding()`, `updateFeeding()` (FSM), `checkSchedule()` |
| LCD (driver tự viết) | `lcdInit()`, `lcdRow()` (chỉ ghi dòng thay đổi), `renderLCD()`, `lcdTestPattern()`, `handleLcdHotplug()` |
| Chẩn đoán I2C | `i2cLineHasPullup()`, `i2cScan()`, `printI2cReport()` |
| IoT | `handleConnectivity()`, `sendTelemetry()`, `mqttCallback()` + `sendAck()`, `publishEvent()` |
| Độ tin cậy | `bufferPush()` / `bufferFlush()` (đệm 240 bản ghi + gửi bù), SAFE MODE sau 30 s mất broker |
| Bảo trì | `runSelfTest()`, `handleSerialCommand()` |

## Kiểm tra đã thực hiện
- Kiểm tra cú pháp C++ (g++ -Wall -Wextra, header giả lập Arduino + ArduinoJson thật): **0 lỗi**.
- Driver LCD chạy trên bộ **mô phỏng HD44780 + PCF8574**: khởi tạo 4-bit đúng, hiển thị đúng cả 16x2 và 20x4.
- ⚠️ **Chưa biên dịch bằng toolchain ESP32 và chưa chạy trên phần cứng thật** → bấm Verify trong Arduino IDE trước khi nạp.
