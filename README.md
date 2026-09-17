# Firmware – ESP32-S3 (v2.2.0)

## Cài đặt
1. Arduino IDE → *Preferences* → Board URL: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. *Boards Manager* → cài **esp32 by Espressif ≥ 3.0** → chọn **ESP32S3 Dev Module**
   (PSRAM: *OPI PSRAM* cho N8R8/N16R8 · Flash size theo board)
3. *Library Manager* cài: `PubSubClient`, `ArduinoJson` (v7), `OneWire`, `DallasTemperature`, `ESP32Servo`, `Adafruit GFX Library`, `Adafruit SH110X` (hoặc `Adafruit SSD1306`), `RTClib`
4. Sửa trong `smart_aquarium_esp32s3.ino`: `WIFI_SSID`, `WIFI_PASS`, `TOPIC_BASE` (đổi `nhomXX`), `OLED_USE_SH1106`, `MQTT_USE_TLS/HOST/PORT/USER/PASS` (HiveMQ public hoặc Cloud), `API_BASE_URL`, `DEVICE_KEY` (hoặc `DB_ENABLED = false`)
5. **Rút jack 5V ngoài** (nếu chưa có diode chống ngược) → cắm USB → Upload → Serial Monitor 115200

## Cấu trúc code
| Mục | Hàm chính |
|---|---|
| Thời gian thực | `initTime()`, `handleTimeSync()`, `seedSystemFromRtc()`, `writeRtcFromSystem()` |
| Nhiệt độ | `updateSensor()`, `controlTemperature()`, `setChiller()` |
| Cho ăn | `startFeeding()`, `updateFeeding()` (FSM), `checkSchedule()` |
| HMI | `renderOLED()`, `beep()`/`updateBuzzer()`, `handleButtons()` |
| IoT | `handleConnectivity()`, `sendTelemetry()`, `mqttCallback()` + `sendAck()`, `publishEvent()` |
| Database | `dbEnqueue()` (loop, không chờ) → `dbTask()` (FreeRTOS core 0, gom lô ≤ 20, HTTPS POST `/api/ingest` trên Vercel), `dbLogTelemetry()` |
| Bảo trì | `runSelfTest()` |

## Kiểm tra đã thực hiện
- Kiểm tra cú pháp C++ (g++ -Wall -Wextra với header giả lập Arduino + ArduinoJson thật): **0 lỗi, 0 cảnh báo**.
- ⚠️ **Chưa biên dịch bằng toolchain ESP32 và chưa chạy trên phần cứng** → nhóm cần Verify trong Arduino IDE và chạy test case `docs/04` §8.

## Lệnh MQTT
Xem `docs/02_DAC_TA_HE_THONG.md` §6.5. Từ v2.1 mỗi lệnh có thể kèm `"id"`; ESP32 trả về topic `…/ack`: `{"id","device","ok","msg"}`.
