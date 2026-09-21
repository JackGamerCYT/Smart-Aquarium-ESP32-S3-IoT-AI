/*
 * ============================================================================
 *  SMART AQUARIUM IoT – ESP32-S3 DevKitC-1 (N8R8 / N16R8)
 *  Firmware v3.2.0 – RTC DS3231 + HiveMQ + backend Render ghi Postgres
 * ============================================================================
 *  v3.2 – ĐỔI MÀN HÌNH OLED -> LCD I2C (1602 hoặc 2004 + mạch PCF8574):
 *   - Driver LCD viết sẵn trong file này -> KHÔNG cần cài thư viện LCD nào.
 *   - Tự dò địa chỉ PCF8574 (0x27 / 0x3F và cả dải 0x20–0x27, 0x38–0x3F).
 *   - Chọn 16x2 hoặc 20x4 lúc chạy (lưu NVS): gõ "lcd 1602" / "lcd 2004" ở Serial
 *     hoặc bấm nút trên web, không cần nạp lại.
 *   - LCD 16x2: tự lật 2–3 trang mỗi 3 s. LCD 20x4: hiện đủ trên 1 trang.
 *   - Chỉ ghi lại dòng nào thay đổi -> không nhấp nháy, không làm chậm loop.
 *   - Cắm lại LCD khi đang chạy: tự nhận lại sau ≤ 5 s. Lệnh "lcd test": tô kín ô.
 *   - Giữ từ v3.1: quét bus I2C + đo điện dây SDA/SCL lúc khởi động, I2C 100 kHz,
 *     sửa lỗi gói gửi bù bị mất nhiệt độ.
 *
 *  Thư viện (Library Manager): PubSubClient, ArduinoJson (v7), OneWire,
 *  DallasTemperature, ESP32Servo, RTClib (Adafruit).
 *  -> KHÔNG cần U8g2 / Adafruit GFX / LiquidCrystal_I2C.
 * ============================================================================
 *  Thay đổi chính so với v1:
 *   - Thêm RTC DS3231 (I2C chung bus với LCD) + đồng bộ NTP -> lịch cho ăn
 *     vẫn chạy đúng giờ khi MẤT Wi-Fi / mất điện (pin CR2032 giữ giờ).
 *   - GIỮ NGUYÊN chân Relay/Còi như mạch cũ (GPIO19, GPIO18, GPIO12) vì đã chạy ổn.
 *   - Toàn bộ vòng lặp NON-BLOCKING: DS18B20 đọc bất đồng bộ, servo & còi
 *     chạy bằng state machine -> MQTT không bị rớt khi đang cho ăn.
 *   - Sửa lỗi manualOverrideChiller không bao giờ tự trả về AUTO.
 *   - Lớp an toàn: lỗi cảm biến -> tắt chiller + báo động; quá nóng -> ép bật;
 *     bơm tắt -> khóa chiller (không có dòng nước qua water block).
 *   - Quạt tản nhiệt tách riêng (MOSFET) + chạy trễ 60 s sau khi tắt sò.
 *   - Lưu cấu hình / lịch / bộ đếm vào NVS (Preferences) -> không mất khi reset.
 *   - MQTT: topic riêng theo nhóm, LWT online/offline, buffer 768 byte.
 *   - v2.1: lệnh web có phản hồi ACK (topic …/ack).
 *   - v2.2: ghi lịch sử lên API Vercel (/api/ingest → Postgres Neon) theo LÔ
 *     bằng task FreeRTOS riêng trên core 0 + hàng đợi (không làm chậm loop);
 *     hỗ trợ HiveMQ public (1883) hoặc HiveMQ Cloud (TLS 8883 + user/pass).
 *
 *  Board: esp32 by Espressif 2.0.x hoặc 3.x  ->  "ESP32S3 Dev Module"
 * ============================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_system.h>

// ============================================================================
// 1. SƠ ĐỒ CHÂN – GIỮ NGUYÊN CHÂN MẠCH CŨ + thêm chân mới cho RTC/quạt/nút
//    Lưu ý: GPIO19 trùng USB D- → nạp code qua cổng "UART/COM", không dùng cổng "USB" (OTG).
//    Chân mới (7, 15, 16) không đụng chân cũ; không nối thì firmware vẫn chạy.
// ============================================================================
#define PIN_DS18B20        4    // 1-Wire. Có trở 4.7k lên 3V3 là tốt nhất
#define DS18B20_INTERNAL_PULLUP 1   // 1 = bật trở kéo NỘI của ESP32 (dùng khi KHÔNG gắn trở 4.7k, dây ngắn ≤ 1 m)
#define PIN_I2C_SDA        8    // LCD PCF8574 0x27/0x3F + DS3231 0x68 (+ EEPROM 0x57)
#define PIN_I2C_SCL        9
#define PIN_RTC_SQW        7    // (MỚI) DS3231 SQW – xung 1 Hz; không nối vẫn chạy
#define PIN_RELAY_CHILLER  19   // IN1: Relay 1 -> Sò Peltier + quạt (như cũ)
#define PIN_RELAY_PUMP     18   // IN2: Relay 2 -> Máy bơm 12V (như cũ)
#define PIN_FAN_MOSFET     15   // (tùy chọn, MỚI) quạt riêng qua IRLZ44N – quạt đang chung relay thì bỏ trống
#define PIN_SERVO_FEED     13   // MG90S PWM 50 Hz
#define PIN_BUZZER         12   // Còi active (như cũ)
#define PIN_BTN_TEST       0    // Nút BOOT: giữ 2 s -> Self-Test
#define PIN_BTN_FEED       16   // (MỚI, tùy chọn) nút cho ăn → GND

// Relay module Active-LOW như mạch cũ (LOW = đóng)
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ============================================================================
// 1b. LCD I2C (HD44780 + PCF8574) – 1602 hoặc 2004, chọn lúc chạy (NVS "lcdRows")
//     Đấu dây: GND->GND · VCC->5V (LCD cần 5V mới đủ tương phản) · SDA->GPIO8 · SCL->GPIO9
//     Có đèn nền mà KHÔNG thấy chữ -> vặn biến trở xanh sau lưng LCD (tương phản).
// ============================================================================
#define LCD_DEFAULT_ROWS 2          // 2 = LCD 1602 (16x2), 4 = LCD 2004 (20x4)
uint8_t lcdAddr   = 0;
uint8_t lcdRows   = LCD_DEFAULT_ROWS;
uint8_t lcdCols   = 16;
bool    lcdPresent   = false;
bool    lcdBacklight = true;
String  lcdShadow[4];               // nội dung đang hiện -> chỉ ghi dòng thay đổi
String  i2cList    = "";            // vd "27,57,68"
int     dsCount    = 0;             // số cảm biến DS18B20 tìm thấy
unsigned long lastLcdProbeMs = 0;

// ============================================================================
// 2. CẤU HÌNH
// ============================================================================
const char* FW_VERSION   = "3.2.0";
const char* WIFI_SSID    = "HO TRO SINH VIEN"; // Wi-Fi 2.4 GHz
const char* WIFI_PASS    = "12345678@";
// ---- MQTT: chọn 1 trong 2 ----
// (A) HiveMQ public:  MQTT_USE_TLS=false, host "broker.hivemq.com", port 1883, user/pass rỗng
// (B) HiveMQ Cloud :  MQTT_USE_TLS=true,  host "xxxxxxxx.s1.eu.hivemq.cloud", port 8883, user/pass của ESP32
const bool  MQTT_USE_TLS = false;
const char* MQTT_HOST    = "broker.hivemq.com";
const int   MQTT_PORT    = 1883;
const char* MQTT_USER    = "";                  // HiveMQ Cloud → Access Management → tài khoản "esp32"
const char* MQTT_PASS    = "";
// Đổi "nhomXX" thành mã nhóm – broker công cộng, topic trùng = người lạ điều khiển được bể!
const char* TOPIC_BASE   = "smartaquarium_node2026";   // PHẢI trùng TOPIC trong index.html

const char* TZ_INFO      = "ICT-7";            // Việt Nam UTC+7, không DST
const char* NTP_1        = "pool.ntp.org";
const char* NTP_2        = "time.google.com";

// ---- Ghi database: KHÔNG cần nữa vì backend trên Render tự nghe MQTT và ghi Postgres.
//      Chỉ bật khi muốn ESP32 tự POST HTTPS (kiến trúc cũ, không có backend).
const bool  DB_ENABLED        = false;
const char* API_BASE_URL      = "https://YOUR-APP.vercel.app";   // <--- domain PRODUCTION, không có "/" cuối
const char* DEVICE_KEY        = "";                              // để trống = không dùng khóa (API mở)
const unsigned long DB_TELEMETRY_PERIOD_MS = 60000;              // 1 bản ghi/phút
const int   DB_BATCH_MAX      = 20;                              // gộp tối đa 20 bản ghi / 1 request HTTPS

// Nhiệt độ (mặc định – có thể đổi từ Web, lưu NVS)
float SP_ON  = 27.0;   // >= bật chiller
float SP_OFF = 25.5;   // <= tắt chiller
const float TEMP_ALARM_HIGH = 30.0;   // ép bật chiller + còi
const float TEMP_ALARM_LOW  = 22.0;   // ép tắt chiller + còi
const float TEMP_VALID_MIN  = 0.0;
const float TEMP_VALID_MAX  = 50.0;

// Thời gian
const unsigned long SENSOR_PERIOD_MS     = 1000;
const unsigned long DS18B20_CONV_MS      = 400;       // 11-bit = 375 ms
const unsigned long TELEMETRY_PERIOD_MS  = 2000;
const unsigned long LCD_PERIOD_MS        = 500;
const unsigned long CHILLER_MIN_OFF_MS   = 60000UL;   // chống đóng/ngắt relay liên tục
const unsigned long FAN_POSTRUN_MS       = 60000UL;   // quạt chạy thêm sau khi tắt sò
const unsigned long MANUAL_TIMEOUT_MS    = 30UL * 60 * 1000; // MANUAL tự về AUTO
const unsigned long PUMP_RESUME_DELAY_MS = 5000;      // bơm chạy lại sau khi hộc đóng
const unsigned long WIFI_RETRY_MS        = 60000;   // Wi-Fi trường vào chậm → đừng ngắt quá sớm
const unsigned long MQTT_RETRY_MS        = 5000;
const unsigned long RTC_WRITE_PERIOD_MS  = 6UL * 3600 * 1000;  // ghi giờ NTP -> RTC
const unsigned long RTC_RESEED_PERIOD_MS = 10UL * 60 * 1000;   // offline: RTC -> hệ thống

// Cho ăn
const int  MAX_SLOTS          = 4;
const int  FEED_GRACE_MIN     = 30;   // khởi động lại trễ <= 30 phút vẫn cho ăn bù
const int  MIN_FEED_GAP_MIN   = 30;   // 2 lần cho ăn cách nhau tối thiểu 30 phút
const int  MAX_FEEDS_PER_DAY  = 6;    // chống cho ăn quá tay
const int  SERVO_OPEN_DEG     = 110;
const unsigned long SERVO_HOLD_MS = 1000;

// ============================================================================
// 3. ĐỐI TƯỢNG & TRẠNG THÁI
// ============================================================================
WiFiClient        mqttPlain;
WiFiClientSecure  mqttSecure;
PubSubClient      mqtt;                        // client gắn trong setup() theo MQTT_USE_TLS
OneWire           oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);
Servo             feedServo;
RTC_DS3231        rtc;
Preferences       prefs;

String topicTelemetry, topicCommand, topicStatus, topicEvent, topicAck, deviceId;

// Nhiệt độ
float currentTemp  = NAN;
bool  sensorFault  = true;
int   sensorErrCnt = 0;
bool  convPending  = false;
unsigned long convStartMs = 0, lastSensorMs = 0;

// Cơ cấu chấp hành
bool chillerOn = false, pumpWanted = true, pumpOn = true, fanOn = false;
unsigned long chillerOffAtMs = 0;
bool autoMode = true;
bool manualChillerReq = false;   // trạng thái người dùng yêu cầu ở chế độ MANUAL
unsigned long manualSinceMs = 0;

// Báo động
enum AlarmCode { ALM_NONE, ALM_SENSOR, ALM_HIGH, ALM_LOW, ALM_TIME };
AlarmCode alarmCode = ALM_NONE, lastAlarm = ALM_NONE;
bool alarmMuted = false;
unsigned long lastAlarmBeepMs = 0;

// Đo đạc / an toàn khi mất mạng
uint32_t telemetrySeq = 0;              // số thứ tự gói → web tính tỉ lệ mất gói
bool safeMode = false;                  // mất broker > 30 s: tự quản lý cục bộ
unsigned long mqttLostAtMs = 0;
uint32_t bufferedSent = 0;

// Bộ đệm khi mất mạng: 1 bản ghi/phút, giữ 240 phút gần nhất
struct BufRec { uint32_t t; float temp; uint8_t flags; uint8_t feedToday; };
const int  BUF_MAX = 240;
BufRec  buf[BUF_MAX];
int     bufHead = 0, bufCount = 0;
unsigned long lastBufMs = 0;

// Thời gian
bool rtcPresent = false;
bool dbActive = false;          // DB chỉ chạy khi DB_ENABLED và API_BASE_URL đã sửa
bool timeValid  = false;
enum TimeSrc { SRC_NONE, SRC_RTC, SRC_NTP, SRC_WEB };
TimeSrc timeSrc = SRC_NONE;
volatile bool rtcTickFlag  = false;
volatile bool ntpSyncFlag  = false;
unsigned long lastTickMs = 0, lastRtcWriteMs = 0, lastReseedMs = 0;
bool everNtp = false;

// Lịch cho ăn
struct FeedSlot { uint8_t h; uint8_t m; };
FeedSlot slots[MAX_SLOTS];
int      slotCount = 0;
uint32_t lastSchedFed = 0;   // epoch của slot gần nhất đã xử lý
uint32_t lastFeedEpoch = 0;
uint32_t feedDayKey = 0;
uint16_t feedToday = 0;
uint32_t feedTotal = 0;

// State machine cho ăn
enum FeedState { FEED_IDLE, FEED_OPEN, FEED_HOLD, FEED_CLOSE, FEED_RESUME };
FeedState feedState = FEED_IDLE;
int  servoPos = 0;
unsigned long feedStepMs = 0;
String feedSource = "";

// Còi non-blocking
int  buzzRemain = 0; unsigned long buzzOnMs = 0, buzzOffMs = 0, buzzNextMs = 0; bool buzzState = false;

// Kết nối / chu kỳ
unsigned long lastWifiTryMs = 0, lastMqttTryMs = 0, lastTelemetryMs = 0, lastLcdMs = 0;
bool selfTestRequested = false;

// Nút nhấn
bool btnFeedPrev = HIGH, btnTestPrev = HIGH;
unsigned long btnFeedChangeMs = 0, btnTestDownMs = 0;

// ============================================================================
// 4. TIỆN ÍCH
// ============================================================================
void IRAM_ATTR onRtcSqw() { rtcTickFlag = true; }
void onNtpSync(struct timeval *tv) { ntpSyncFlag = true; }

void beep(int count, unsigned long onMs = 80, unsigned long offMs = 80) {
  buzzRemain = count; buzzOnMs = onMs; buzzOffMs = offMs;
  buzzNextMs = millis(); buzzState = false;
}

void updateBuzzer(unsigned long now) {
  if (buzzRemain <= 0 || now < buzzNextMs) return;
  if (!buzzState) { digitalWrite(PIN_BUZZER, HIGH); buzzState = true;  buzzNextMs = now + buzzOnMs; }
  else            { digitalWrite(PIN_BUZZER, LOW);  buzzState = false; buzzNextMs = now + buzzOffMs; buzzRemain--; }
}

bool getLocal(struct tm &t) {
  if (!timeValid) return false;
  time_t e = time(nullptr);
  localtime_r(&e, &t);
  return true;
}

String fmtTime(bool withDate) {
  struct tm t;
  if (!getLocal(t)) return withDate ? "----------" : "--:--:--";
  char b[24];
  if (withDate) strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &t);
  else          strftime(b, sizeof(b), "%H:%M:%S", &t);
  return String(b);
}

const char* srcName(TimeSrc s) {
  switch (s) { case SRC_RTC: return "RTC"; case SRC_NTP: return "NTP"; case SRC_WEB: return "WEB"; default: return "NONE"; }
}
const char* alarmName(AlarmCode a) {
  switch (a) { case ALM_SENSOR: return "SENSOR_FAULT"; case ALM_HIGH: return "TEMP_HIGH";
               case ALM_LOW: return "TEMP_LOW"; case ALM_TIME: return "TIME_INVALID"; default: return "NONE"; }
}

// ============================================================================
// 4b. GHI DATABASE – TASK FreeRTOS RIÊNG (core 0) + HÀNG ĐỢI + GỬI THEO LÔ
//     loop() chỉ đẩy JSON vào queue (không chờ). dbTask gom tối đa
//     DB_BATCH_MAX bản ghi rồi POST 1 lần lên API_BASE_URL/api/ingest.
//     Mất Wi-Fi: dữ liệu nằm chờ trong queue (40 bản ghi).
// ============================================================================
struct DbJob { char table[12]; char* json; };
QueueHandle_t dbQueue = nullptr;
volatile uint32_t dbOk = 0, dbFail = 0, dbDropped = 0;
unsigned long lastDbTelemetryMs = 0;

void dbEnqueue(const char* table, JsonDocument &row) {
  if (!dbActive || dbQueue == nullptr || deviceId.length() < 3) return;
  row["device_id"] = deviceId;
  if (timeValid) {                                   // gắn giờ thực của thiết bị (UTC)
    time_t e = time(nullptr); struct tm u; gmtime_r(&e, &u);
    char iso[24]; strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &u);
    row["created_at"] = iso;
  }
  String out; serializeJson(row, out);
  DbJob job; strncpy(job.table, table, sizeof(job.table) - 1); job.table[sizeof(job.table) - 1] = 0;
  job.json = strdup(out.c_str());
  if (job.json == nullptr) { dbDropped++; return; }
  if (xQueueSend(dbQueue, &job, 0) != pdTRUE) { free(job.json); dbDropped++; }
}

void dbTask(void*) {
  WiFiClientSecure tls;
  tls.setInsecure();              // bài học: bỏ kiểm tra chứng chỉ. Sản phẩm thật: tls.setCACert(rootCA)
  HTTPClient http;
  http.setReuse(true);            // giữ kết nối HTTPS giữa các lô → đỡ tốn thời gian bắt tay TLS
  DbJob batch[DB_BATCH_MAX];
  const String url = String(API_BASE_URL) + "/api/ingest";

  for (;;) {
    int n = 0;
    if (xQueueReceive(dbQueue, &batch[n], portMAX_DELAY) != pdTRUE) continue;
    n++;
    // gom thêm bản ghi đến trong 2 s (sự kiện thường đi thành chùm)
    while (n < DB_BATCH_MAX && xQueueReceive(dbQueue, &batch[n], pdMS_TO_TICKS(2000)) == pdTRUE) n++;

    String body; body.reserve(n * 260 + 16);
    body = "{\"items\":[";
    for (int i = 0; i < n; i++) {
      if (i > 0) body += ",";
      body += "{\"t\":\""; body += batch[i].table; body += "\",\"r\":"; body += batch[i].json; body += "}";
    }
    body += "]}";

    while (WiFi.status() != WL_CONNECTED) vTaskDelay(pdMS_TO_TICKS(2000));
    bool done = false;
    for (int attempt = 0; attempt < 3 && !done; attempt++) {
      if (!http.begin(tls, url)) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
      http.setTimeout(10000);
      http.addHeader("Content-Type", "application/json");
      if (strlen(DEVICE_KEY)) http.addHeader("x-device-key", DEVICE_KEY);
      int code = http.POST(body);
      if (code == 200) { done = true; dbOk += n; }
      else {
        Serial.printf("[DB] HTTP %d %s\n", code, code > 0 ? http.getString().c_str() : "(mat ket noi)");
        if (code >= 400 && code < 500) { http.end(); break; }   // sai khóa / dữ liệu: không thử lại
        vTaskDelay(pdMS_TO_TICKS(3000 * (attempt + 1)));
      }
      http.end();
    }
    if (!done) dbFail += n;
    for (int i = 0; i < n; i++) free(batch[i].json);
  }
}

void dbLogTelemetry() {
  JsonDocument r;
  if (sensorFault) r["temp"] = nullptr; else r["temp"] = round(currentTemp * 10) / 10.0;
  r["chiller"] = chillerOn; r["pump"] = pumpOn; r["fan"] = fanOn;
  r["mode"] = autoMode ? "AUTO" : "MANUAL";
  r["feed_today"] = feedToday; r["feed_total"] = feedTotal;
  r["alarm"] = alarmName(alarmCode);
  if (rtcPresent) r["rtc_temp"] = round(rtc.getTemperature() * 10) / 10.0;
  r["rssi"] = WiFi.RSSI();
  r["time_src"] = srcName(timeSrc);
  dbEnqueue("telemetry", r);
}

void publishEvent(const String &type, const String &detail) {
  Serial.printf("[EVENT] %s: %s\n", type.c_str(), detail.c_str());
  { JsonDocument r; r["type"] = type.substring(0, 24); r["detail"] = detail.substring(0, 200); dbEnqueue("events", r); }
  if (!mqtt.connected()) return;
  JsonDocument d;
  d["type"] = type; d["detail"] = detail; d["time"] = fmtTime(true);
  String out; serializeJson(d, out);
  mqtt.publish(topicEvent.c_str(), out.c_str());
}

// ============================================================================
// 5. THỜI GIAN THỰC: RTC DS3231 <-> ĐỒNG HỒ HỆ THỐNG <-> NTP
// ============================================================================
void setSystemTime(time_t e) { struct timeval tv = { e, 0 }; settimeofday(&tv, nullptr); }

bool seedSystemFromRtc() {
  if (!rtcPresent) return false;
  DateTime d = rtc.now();
  if (d.year() < 2024 || d.year() > 2099) return false;
  struct tm t = {};
  t.tm_year = d.year() - 1900; t.tm_mon = d.month() - 1; t.tm_mday = d.day();
  t.tm_hour = d.hour(); t.tm_min = d.minute(); t.tm_sec = d.second(); t.tm_isdst = -1;
  setSystemTime(mktime(&t));      // RTC lưu GIỜ ĐỊA PHƯƠNG (UTC+7)
  return true;
}

void writeRtcFromSystem() {
  if (!rtcPresent) return;
  time_t e = time(nullptr);
  struct tm t; localtime_r(&e, &t);
  rtc.adjust(DateTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec));
  lastRtcWriteMs = millis();
}

void initTime() {
  setenv("TZ", TZ_INFO, 1); tzset();
  rtcPresent = rtc.begin(&Wire);
  if (!rtcPresent) { Serial.println("[RTC] Khong tim thay DS3231 (0x68)!"); return; }

  rtc.disable32K();
  rtc.writeSqwPinMode(DS3231_SquareWave1Hz);
  pinMode(PIN_RTC_SQW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_RTC_SQW), onRtcSqw, FALLING);

  if (rtc.lostPower()) {
    Serial.println("[RTC] Mat nguon pin -> gio khong hop le, cho NTP/Web");
  } else if (seedSystemFromRtc()) {
    timeValid = true; timeSrc = SRC_RTC; lastReseedMs = millis();
    Serial.printf("[RTC] Gio tu DS3231: %s\n", fmtTime(true).c_str());
  }
}

void handleTimeSync(unsigned long now) {
  if (ntpSyncFlag) {                          // cờ đặt từ task SNTP -> xử lý I2C ở loop
    ntpSyncFlag = false;
    bool first = !everNtp;
    everNtp = true; timeValid = true; timeSrc = SRC_NTP;
    if (first || now - lastRtcWriteMs >= RTC_WRITE_PERIOD_MS) {
      writeRtcFromSystem();
      publishEvent("time_sync", String("NTP -> RTC ") + fmtTime(true));
    }
    lastReseedMs = now;
  }
  // Offline lâu: đồng hồ thạch anh ESP32 trôi -> lấy lại giờ từ TCXO DS3231 (±2 ppm)
  if (rtcPresent && timeValid && WiFi.status() != WL_CONNECTED &&
      now - lastReseedMs >= RTC_RESEED_PERIOD_MS) {
    if (seedSystemFromRtc()) timeSrc = SRC_RTC;
    lastReseedMs = now;
  }
}

// ============================================================================
// 6. NVS
// ============================================================================
String scheduleToString() {
  String s;
  for (int i = 0; i < slotCount; i++) {
    char b[6]; snprintf(b, sizeof(b), "%02d:%02d", slots[i].h, slots[i].m);
    if (i > 0) s += ",";
    s += b;
  }
  return s;
}

bool parseSchedule(const String &str) {
  FeedSlot tmp[MAX_SLOTS]; int n = 0; int start = 0;
  String s = str; s.trim();
  if (s.length() == 0) { slotCount = 0; return true; }       // chuỗi rỗng = tắt lịch
  while (start < (int)s.length() && n < MAX_SLOTS) {
    int comma = s.indexOf(',', start); if (comma < 0) comma = s.length();
    String item = s.substring(start, comma); item.trim();
    int colon = item.indexOf(':');
    if (colon < 1) return false;
    int h = item.substring(0, colon).toInt(), m = item.substring(colon + 1).toInt();
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    tmp[n].h = h; tmp[n].m = m; n++;
    start = comma + 1;
  }
  for (int i = 0; i < n; i++)                                  // sắp xếp tăng dần
    for (int j = i + 1; j < n; j++)
      if (tmp[j].h * 60 + tmp[j].m < tmp[i].h * 60 + tmp[i].m) { FeedSlot x = tmp[i]; tmp[i] = tmp[j]; tmp[j] = x; }
  memcpy(slots, tmp, sizeof(FeedSlot) * n); slotCount = n;
  return true;
}

void loadSettings() {
  prefs.begin("aquarium", false);
  if (!parseSchedule(prefs.getString("sched", "07:00,17:30"))) parseSchedule("07:00,17:30");
  SP_ON         = prefs.getFloat("spOn", SP_ON);
  SP_OFF        = prefs.getFloat("spOff", SP_OFF);
  lastSchedFed  = prefs.getUInt("lastSched", 0);
  lastFeedEpoch = prefs.getUInt("lastFeed", 0);
  feedDayKey    = prefs.getUInt("feedDay", 0);
  feedToday     = prefs.getUShort("feedTod", 0);
  feedTotal     = prefs.getUInt("feedTot", 0);
  lcdRows       = prefs.getUChar("lcdRows", LCD_DEFAULT_ROWS) == 4 ? 4 : 2;
  lcdCols       = lcdRows == 4 ? 20 : 16;
}

void saveFeedCounters() {
  prefs.putUInt("lastSched", lastSchedFed);
  prefs.putUInt("lastFeed", lastFeedEpoch);
  prefs.putUInt("feedDay", feedDayKey);
  prefs.putUShort("feedTod", feedToday);
  prefs.putUInt("feedTot", feedTotal);
}

// ============================================================================
// 7. CƠ CẤU CHẤP HÀNH
// ============================================================================
void applyOutputs(unsigned long now) {
  // Quạt: chạy khi sò chạy + chạy trễ để xả nhiệt mặt nóng
  fanOn = chillerOn || (chillerOffAtMs != 0 && now - chillerOffAtMs < FAN_POSTRUN_MS);
  digitalWrite(PIN_RELAY_CHILLER, chillerOn ? RELAY_ON : RELAY_OFF);
  digitalWrite(PIN_RELAY_PUMP,    pumpOn    ? RELAY_ON : RELAY_OFF);
  digitalWrite(PIN_FAN_MOSFET,    fanOn     ? HIGH : LOW);
  static int lastC = -1, lastP = -1;               // in ra Serial mỗi khi relay đổi trạng thái
  if (lastC != chillerOn || lastP != pumpOn) {
    lastC = chillerOn; lastP = pumpOn;
    Serial.printf("[RELAY] IN1 So lanh (GPIO%d) = %s | IN2 Bom (GPIO%d) = %s\n",
                  PIN_RELAY_CHILLER, chillerOn ? "ON (LOW)" : "OFF (HIGH)", PIN_RELAY_PUMP, pumpOn ? "ON (LOW)" : "OFF (HIGH)");
  }
}

void setChiller(bool on, unsigned long now, const char* reason) {
  if (on == chillerOn) return;
  static unsigned long lastWhy = 0;
  if (on && !pumpWanted) {                                               // khóa liên động
    if (now - lastWhy > 5000) { lastWhy = now; Serial.println("[RELAY] Khong bat so: BOM dang OFF"); }
    return;
  }
  if (on && chillerOffAtMs != 0 && now - chillerOffAtMs < CHILLER_MIN_OFF_MS) {
    if (now - lastWhy > 5000) { lastWhy = now; Serial.printf("[RELAY] Khong bat so: cho nghi them %lus\n", (CHILLER_MIN_OFF_MS - (now - chillerOffAtMs)) / 1000); }
    return;
  }
  chillerOn = on;
  if (!on) chillerOffAtMs = now;
  applyOutputs(now);
  beep(on ? 1 : 2);
  publishEvent("chiller", String(on ? "ON (" : "OFF (") + reason + ")");
}

void controlTemperature(unsigned long now) {
  // MANUAL tự hết hạn -> AUTO (sửa lỗi v1)
  if (!autoMode && now - manualSinceMs >= MANUAL_TIMEOUT_MS) {
    autoMode = true; publishEvent("mode", "AUTO (het han MANUAL)");
  }

  // Lớp an toàn luôn ưu tiên cao nhất
  if (sensorFault) {                     // mất cảm biến: AUTO tắt sò; MANUAL (người dùng bấm) vẫn cho chạy
    alarmCode = ALM_SENSOR;
    setChiller(autoMode ? false : manualChillerReq, now, autoMode ? "loi cam bien" : "MANUAL (khong co cam bien)");
    return;
  }
  if (!pumpWanted) { setChiller(false, now, "bom tat"); }

  if (currentTemp >= TEMP_ALARM_HIGH)      { alarmCode = ALM_HIGH; setChiller(true,  now, "qua nong"); return; }
  else if (currentTemp <= TEMP_ALARM_LOW)  { alarmCode = ALM_LOW;  setChiller(false, now, "qua lanh"); return; }
  else alarmCode = (!timeValid && slotCount > 0) ? ALM_TIME : ALM_NONE;

  if (!autoMode) { setChiller(manualChillerReq, now, "MANUAL"); return; }
  if (currentTemp >= SP_ON)       setChiller(true,  now, "AUTO");
  else if (currentTemp <= SP_OFF) setChiller(false, now, "AUTO");
}

// ============================================================================
// 8. ĐỌC DS18B20 BẤT ĐỒNG BỘ
// ============================================================================
/** Mất cảm biến: mỗi 10 s dò lại bus 1-Wire (cắm lại dây là tự nhận, không cần reset). */
void retryDs18b20(unsigned long now) {
  static unsigned long lastRetry = 0;
  if (!sensorFault || now - lastRetry < 10000) return;
  lastRetry = now;
#if DS18B20_INTERNAL_PULLUP
  pinMode(PIN_DS18B20, INPUT_PULLUP);
#endif
  ds18b20.begin();
#if DS18B20_INTERNAL_PULLUP
  pinMode(PIN_DS18B20, INPUT_PULLUP);
#endif
  ds18b20.setResolution(11);
  ds18b20.setWaitForConversion(false);
  dsCount = ds18b20.getDeviceCount();
  Serial.printf("[DS18B20] Mat cam bien -> do lai GPIO%d: %d cam bien%s\n", PIN_DS18B20, dsCount,
                dsCount ? "" : " (kiem tra day VANG->GPIO4, DO->3V3, DEN->GND, tro 4.7k VANG-DO)");
}

void updateSensor(unsigned long now) {
  retryDs18b20(now);
  if (!convPending && now - lastSensorMs >= SENSOR_PERIOD_MS) {
    ds18b20.requestTemperatures();          // không chờ (setWaitForConversion(false))
    convPending = true; convStartMs = now; lastSensorMs = now;
  }
  if (convPending && now - convStartMs >= DS18B20_CONV_MS) {
    convPending = false;
    float t = ds18b20.getTempCByIndex(0);
    bool bad = (t == DEVICE_DISCONNECTED_C) || (t == 85.0f) || t < TEMP_VALID_MIN || t > TEMP_VALID_MAX;
    if (bad) {
      if (++sensorErrCnt >= 3 && !sensorFault) { sensorFault = true; publishEvent("alarm", "DS18B20 loi / mat ket noi"); }
    } else {
      sensorErrCnt = 0; currentTemp = t;
      if (sensorFault) { sensorFault = false; publishEvent("info", "DS18B20 hoat dong lai"); }
    }
    controlTemperature(now);
  }
}

// ============================================================================
// 9. CHO ĂN – STATE MACHINE + LỊCH THEO RTC
// ============================================================================
bool startFeeding(const String &source) {
  if (feedState != FEED_IDLE) return false;
  if (feedToday >= MAX_FEEDS_PER_DAY) { publishEvent("feed_reject", "Vuot gioi han/ngay"); beep(3, 40, 60); return false; }
  feedSource = source;
  pumpOn = false; applyOutputs(millis());          // tạm dừng bơm, không hút mất thức ăn
  feedServo.attach(PIN_SERVO_FEED, 500, 2400);
  servoPos = 0; feedServo.write(servoPos);
  feedState = FEED_OPEN; feedStepMs = millis();
  beep(1);
  return true;
}

void updateFeeding(unsigned long now) {
  switch (feedState) {
    case FEED_IDLE: return;
    case FEED_OPEN:                                         // quét từ từ -> chống sụt áp
      if (now - feedStepMs >= 20) { feedStepMs = now; servoPos += 5; feedServo.write(servoPos);
        if (servoPos >= SERVO_OPEN_DEG) feedState = FEED_HOLD; }
      break;
    case FEED_HOLD:
      if (now - feedStepMs >= SERVO_HOLD_MS) { feedState = FEED_CLOSE; feedStepMs = now; }
      break;
    case FEED_CLOSE:
      if (now - feedStepMs >= 20) { feedStepMs = now; servoPos -= 5; feedServo.write(max(servoPos, 0));
        if (servoPos <= 0) { feedServo.detach(); feedState = FEED_RESUME; } }
      break;
    case FEED_RESUME:
      if (now - feedStepMs >= PUMP_RESUME_DELAY_MS) {
        pumpOn = pumpWanted; applyOutputs(now);
        feedState = FEED_IDLE;
        feedToday++; feedTotal++;
        if (timeValid) lastFeedEpoch = (uint32_t)time(nullptr);
        saveFeedCounters();
        beep(2, 120, 80);
        publishEvent("feed", feedSource + " – hom nay " + String(feedToday) + " lan");
      }
      break;
  }
}

void checkSchedule() {
  struct tm t;
  if (!getLocal(t)) return;

  uint32_t dayKey = (t.tm_year + 1900) * 10000UL + (t.tm_mon + 1) * 100UL + t.tm_mday;
  if (dayKey != feedDayKey) { feedDayKey = dayKey; feedToday = 0; saveFeedCounters(); }

  if (feedState != FEED_IDLE) return;
  time_t nowE = time(nullptr);
  for (int i = 0; i < slotCount; i++) {
    struct tm s = t; s.tm_hour = slots[i].h; s.tm_min = slots[i].m; s.tm_sec = 0; s.tm_isdst = -1;
    time_t slotE = mktime(&s);
    if (nowE < slotE || nowE >= slotE + FEED_GRACE_MIN * 60) continue;
    if ((uint32_t)slotE <= lastSchedFed) continue;          // slot này đã xử lý
    lastSchedFed = (uint32_t)slotE;
    if (lastFeedEpoch != 0 && (uint32_t)nowE - lastFeedEpoch < (uint32_t)MIN_FEED_GAP_MIN * 60) {
      saveFeedCounters();
      publishEvent("feed_skip", "Vua cho an thu cong < 30 phut");
      continue;
    }
    char b[6]; snprintf(b, sizeof(b), "%02d:%02d", slots[i].h, slots[i].m);
    if (!startFeeding(String("LICH ") + b)) saveFeedCounters();
    return;
  }
}

String nextFeedString() {
  struct tm t;
  if (slotCount == 0) return "OFF";
  if (!getLocal(t)) return "--:--";
  int nowMin = t.tm_hour * 60 + t.tm_min;
  int idx = 0;                                   // không còn slot hôm nay -> slot đầu ngày mai
  for (int i = 0; i < slotCount; i++) {
    if (slots[i].h * 60 + slots[i].m > nowMin) { idx = i; break; }
  }
  char b[6]; snprintf(b, sizeof(b), "%02d:%02d", slots[idx].h, slots[idx].m);
  return String(b);
}

// ============================================================================
// 10. LCD I2C + CHẨN ĐOÁN I2C
// ============================================================================
bool i2cProbe(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }

/** Gọi TRƯỚC Wire.begin(): kéo xuống yếu (~45k) mà vẫn đọc HIGH => có trở kéo lên của module
 *  => module có nguồn và dây đã cắm đúng vào chân này. */
bool i2cLineHasPullup(int pin) {
  pinMode(pin, INPUT_PULLDOWN);
  delay(5);
  int hi = 0;
  for (int i = 0; i < 10; i++) { hi += digitalRead(pin); delayMicroseconds(200); }
  pinMode(pin, INPUT);
  return hi >= 9;
}

String i2cScan() {
  String s; int n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (!i2cProbe(a)) continue;
    char b[6]; snprintf(b, sizeof(b), "%s%02X", n++ ? "," : "", a);
    s += b;
  }
  return s;
}

bool isLcdAddr(uint8_t a) { return (a >= 0x20 && a <= 0x27) || (a >= 0x38 && a <= 0x3F); }

void printI2cReport() {
  Serial.printf("[I2C] SDA=GPIO%d SCL=GPIO%d -> thiet bi: %s\n", PIN_I2C_SDA, PIN_I2C_SCL,
                i2cList.length() ? i2cList.c_str() : "KHONG CO");
  if (!i2cList.length()) {
    Serial.println("[I2C] Khong thay thiet bi nao -> loi DAY, khong phai loi code:");
    Serial.println("      - LCD: VCC->5V, GND chung ESP32 | RTC: VCC->3V3");
    Serial.println("      - SDA -> GPIO8, SCL -> GPIO9 (hay bi DAO nguoc 2 day nay)");
    return;
  }
  bool lcd = false;
  for (uint8_t a = 0x20; a <= 0x3F; a++) {
    if (!isLcdAddr(a)) continue;
    char h[3]; snprintf(h, sizeof(h), "%02X", a);
    if (i2cList.indexOf(h) >= 0) lcd = true;
  }
  Serial.printf("      LCD(27/3F): %s | DS3231(68): %s | EEPROM(57): %s\n", lcd ? "CO" : "KHONG",
                i2cList.indexOf("68") >= 0 ? "CO" : "KHONG", i2cList.indexOf("57") >= 0 ? "CO" : "KHONG");
}

// ---- Driver HD44780 qua PCF8574: P0=RS P1=RW P2=EN P3=đèn nền P4..P7=D4..D7 ----
#define LCD_RS 0x01
#define LCD_EN 0x04
#define LCD_BL 0x08

void lcdNibble(uint8_t hiNibble, uint8_t mode) {       // 3 byte/1 lần truyền: data -> EN=1 -> EN=0
  uint8_t v = (hiNibble & 0xF0) | mode | (lcdBacklight ? LCD_BL : 0);
  Wire.beginTransmission(lcdAddr);
  Wire.write(v); Wire.write(v | LCD_EN); Wire.write(v);
  Wire.endTransmission();
}
void lcdSend(uint8_t b, uint8_t mode) { lcdNibble(b & 0xF0, mode); lcdNibble((b << 4) & 0xF0, mode); delayMicroseconds(40); }
void lcdCmd(uint8_t c)  { lcdSend(c, 0); if (c <= 0x03) delay(2); }       // clear/home cần ~1.6 ms

uint8_t lcdFindAddr() {
  const uint8_t pref[] = { 0x27, 0x3F };                // 2 địa chỉ hay gặp nhất
  for (uint8_t a : pref) if (i2cProbe(a)) return a;
  for (uint8_t a = 0x20; a <= 0x3F; a++) if (isLcdAddr(a) && i2cProbe(a)) return a;
  return 0;
}

const char* lcdSizeName() { return lcdRows == 4 ? "2004" : "1602"; }

String lcdStatus() {
  if (!lcdPresent) return "NONE";
  char b[20]; snprintf(b, sizeof(b), "LCD%s@0x%02X", lcdSizeName(), lcdAddr);
  return String(b);
}

/** Dò địa chỉ PCF8574 rồi khởi tạo HD44780 ở chế độ 4-bit. Gọi lại được bất kỳ lúc nào. */
bool lcdInit() {
  lcdAddr = lcdFindAddr();
  if (!lcdAddr) { lcdPresent = false; return false; }
  lcdCols = lcdRows == 4 ? 20 : 16;
  Wire.beginTransmission(lcdAddr); Wire.write(lcdBacklight ? LCD_BL : 0); Wire.endTransmission();
  delay(50);                                            // LCD cần >40 ms sau khi có nguồn
  lcdNibble(0x30, 0); delay(5);                         // trình tự reset chuẩn HD44780
  lcdNibble(0x30, 0); delayMicroseconds(150);
  lcdNibble(0x30, 0); delayMicroseconds(150);
  lcdNibble(0x20, 0); delayMicroseconds(150);           // vào chế độ 4-bit
  lcdCmd(0x28);                                         // 4-bit, 2 dòng logic (2004 cũng dùng), font 5x8
  lcdCmd(0x0C);                                         // bật hiển thị, tắt con trỏ
  lcdCmd(0x06);                                         // tự tăng địa chỉ
  lcdCmd(0x01);                                         // xóa màn
  for (auto &r : lcdShadow) r = "";
  lcdPresent = true;
  return true;
}

/** Ghi 1 dòng (tự cắt / đệm khoảng trắng). Chỉ gửi I2C khi nội dung khác lần trước. */
void lcdRow(uint8_t row, const String &text) {
  if (!lcdPresent || row >= lcdRows) return;
  String t = text.substring(0, lcdCols);
  while ((int)t.length() < lcdCols) t += ' ';
  if (t == lcdShadow[row]) return;
  const uint8_t base[4] = { 0x00, 0x40, 0x14, 0x54 };  // địa chỉ DDRAM đầu mỗi dòng
  lcdCmd(0x80 | base[row]);
  for (int i = 0; i < lcdCols; i++) lcdSend((uint8_t)t[i], LCD_RS);
  lcdShadow[row] = t;
}

void lcdSplash(const String &l1, const String &l2) {
  if (!lcdPresent) return;
  for (uint8_t r = 0; r < lcdRows; r++) lcdRow(r, r == 0 ? l1 : r == 1 ? l2 : String(""));
}

/** Tô kín mọi ô 1.5 s rồi hiện thông tin – kiểm tra màn + biến trở tương phản. */
void lcdTestPattern() {
  if (!lcdPresent) { Serial.println("[LCD] Khong co LCD tren bus -> kiem tra day"); return; }
  String full; for (int i = 0; i < lcdCols; i++) full += (char)0xFF;   // 0xFF = ô tô đen
  for (uint8_t r = 0; r < lcdRows; r++) lcdRow(r, full);
  delay(1500);
  char ad[16]; snprintf(ad, sizeof(ad), "Dia chi 0x%02X", lcdAddr);
  lcdSplash("LCD OK " + String(lcdSizeName()), ad);
  if (lcdRows == 4) { lcdRow(2, "Mo chu? vat bien tro"); lcdRow(3, "xanh sau lung LCD"); }
  delay(2000);
  Serial.printf("[LCD] Test xong: %s. Chi thay o den/khong thay chu -> vat bien tro tuong phan\n", lcdStatus().c_str());
}

void setLcdSize(uint8_t rows) {
  lcdRows = rows == 4 ? 4 : 2;
  prefs.putUChar("lcdRows", lcdRows);
  if (lcdInit()) lcdTestPattern();
  Serial.printf("[LCD] Kich thuoc = %s (da luu, khoi dong lai van giu)\n", lcdSizeName());
  publishEvent("lcd", String("Kich thuoc ") + lcdSizeName() + " -> " + lcdStatus());
}

/** Cắm/rút LCD lúc đang chạy: kiểm tra mỗi 5 s. */
void handleLcdHotplug(unsigned long now) {
  if (now - lastLcdProbeMs < 5000) return;
  lastLcdProbeMs = now;
  if (lcdPresent) {
    if (!i2cProbe(lcdAddr)) {
      lcdPresent = false;
      Serial.println("[LCD] Mat ket noi LCD -> kiem tra day SDA/SCL/VCC");
      publishEvent("alarm", "LCD mat ket noi I2C");
    }
  } else if (lcdFindAddr()) {
    if (lcdInit()) {
      i2cList = i2cScan();
      Serial.printf("[LCD] Da nhan LCD: %s\n", lcdStatus().c_str());
      publishEvent("info", String("LCD ket noi: ") + lcdStatus());
    }
  }
}

String tempText() {                                     // "26.5°C" hoặc "ERR   "
  if (sensorFault) return "ERR";
  char b[10]; snprintf(b, sizeof(b), "%.1f%cC", currentTemp, (char)0xDF);   // 0xDF = dấu độ trong ROM LCD
  return String(b);
}
const char* modeText() { return safeMode ? "SAFE" : (autoMode ? "AUTO" : "MANU"); }

void renderLCD(unsigned long now) {
  char b[24];
  String flags = String(WiFi.status() == WL_CONNECTED ? "W+" : "W-") + (mqtt.connected() ? "M+" : "M-") + (rtcPresent ? "R+" : "R-");

  if (lcdRows == 4) {                                   // ---- LCD 2004: 1 trang đủ thông tin ----
    lcdRow(0, fmtTime(false) + "  " + flags);
    lcdRow(1, "Nuoc: " + tempText() + "  " + modeText());
    snprintf(b, sizeof(b), "Lanh:%s Bom:%s Q:%s", chillerOn ? "ON" : "--", pumpOn ? "ON" : "--", fanOn ? "ON" : "--");
    lcdRow(2, b);
    if (alarmCode != ALM_NONE)       lcdRow(3, String("! ") + alarmName(alarmCode));
    else if (feedState != FEED_IDLE) lcdRow(3, "Dang cho ca an...");
    else { snprintf(b, sizeof(b), "An:%u Ke:%s %s", (unsigned)feedToday, nextFeedString().c_str(), srcName(timeSrc)); lcdRow(3, b); }
    return;
  }

  // ---- LCD 1602: lật trang mỗi 3 s; có cảnh báo / đang cho ăn thì thêm trang 3 ----
  static uint8_t page = 0;
  static unsigned long pageMs = 0;
  bool special = alarmCode != ALM_NONE || feedState != FEED_IDLE;
  uint8_t pages = special ? 3 : 2;
  if (now - pageMs >= 3000) { pageMs = now; page = (page + 1) % pages; }
  if (page >= pages) page = 0;

  if (page == 0) {
    lcdRow(0, fmtTime(false) + " " + flags);
    lcdRow(1, "Nuoc " + tempText() + " " + modeText());
  } else if (page == 1) {
    snprintf(b, sizeof(b), "LANH:%s  BOM:%s", chillerOn ? "ON" : "--", pumpOn ? "ON" : "--");
    lcdRow(0, b);
    snprintf(b, sizeof(b), "An %u/ng Ke %s", (unsigned)feedToday, nextFeedString().c_str());
    lcdRow(1, b);
  } else {
    if (alarmCode != ALM_NONE) { lcdRow(0, "!! CANH BAO !!"); lcdRow(1, alarmName(alarmCode)); }
    else                       { lcdRow(0, "DANG CHO CA AN"); lcdRow(1, "Bom tam dung..."); }
  }
}

// Dòng chữ cho màn Self-Test: dòng 1 = tiêu đề, các dòng dưới cuộn kết quả mới nhất
String stBuf[3];
void stPrint(const String &s) {
  Serial.print("  "); Serial.println(s.c_str());
  if (!lcdPresent) return;
  int n = lcdRows - 1;                                  // số dòng dành cho kết quả
  for (int i = 0; i < n - 1; i++) stBuf[i] = stBuf[i + 1];
  stBuf[n - 1] = s;
  lcdRow(0, "=== SELF-TEST ===");
  for (int i = 0; i < n; i++) lcdRow(i + 1, stBuf[i]);
}

// ============================================================================
// 11. SELF-TEST (chế độ bảo trì – cho phép blocking)
// ============================================================================
void runSelfTest() {
  Serial.println("\n[SELF-TEST] Bat dau");
  bool okTemp, okRtc;
  bool prevChiller = chillerOn;
  for (auto &x : stBuf) x = "";
  stPrint("Bat dau...");

  digitalWrite(PIN_BUZZER, HIGH); delay(100); digitalWrite(PIN_BUZZER, LOW);
  stPrint("1.Coi          OK"); delay(300);

  digitalWrite(PIN_RELAY_CHILLER, RELAY_ON); delay(800); digitalWrite(PIN_RELAY_CHILLER, RELAY_OFF);
  stPrint("2.Relay so lanh IN1"); delay(300);

  digitalWrite(PIN_RELAY_PUMP, RELAY_OFF); delay(800); digitalWrite(PIN_RELAY_PUMP, RELAY_ON);
  digitalWrite(PIN_FAN_MOSFET, HIGH); delay(800); digitalWrite(PIN_FAN_MOSFET, LOW);
  stPrint("3.Bom IN2 + Quat");

  feedServo.attach(PIN_SERVO_FEED, 500, 2400);
  for (int p = 0; p <= SERVO_OPEN_DEG; p += 5) { feedServo.write(p); delay(20); }
  delay(300);
  for (int p = SERVO_OPEN_DEG; p >= 0; p -= 5) { feedServo.write(p); delay(20); }
  delay(200); feedServo.detach();
  stPrint("4.Servo MG90S");

  ds18b20.setWaitForConversion(true); ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0); ds18b20.setWaitForConversion(false);
  okTemp = !(t == DEVICE_DISCONNECTED_C || t == 85.0f);
  stPrint(String("5.DS18B20 ") + (okTemp ? String(t, 1) + "C" : String("FAIL")));

  okRtc = rtcPresent && !rtc.lostPower();
  stPrint(String("6.RTC ") + (okRtc ? fmtTime(false) : String("FAIL")));

  i2cList = i2cScan();
  stPrint(String("7.I2C ") + (i2cList.length() ? i2cList : String("TRONG")));

  beep(okTemp && okRtc ? 2 : 4);
  chillerOn = prevChiller; applyOutputs(millis());
  publishEvent("self_test", String("DS18B20=") + (okTemp ? "PASS" : "FAIL") + " RTC=" + (okRtc ? "PASS" : "FAIL")
               + " LCD=" + lcdStatus() + " I2C=" + i2cList);
  delay(2000);
}

// ============================================================================
// 11b. ĐỆM DỮ LIỆU KHI MẤT MẠNG + GỬI BÙ
// ============================================================================
void bufferPush(unsigned long now) {
  if (!timeValid) return;
  if (now - lastBufMs < 60000) return;                 // 1 bản ghi/phút
  lastBufMs = now;
  BufRec r;
  r.t = (uint32_t)time(nullptr);
  r.temp = sensorFault ? NAN : currentTemp;
  r.flags = (chillerOn ? 1 : 0) | (pumpOn ? 2 : 0) | (fanOn ? 4 : 0) | (autoMode ? 8 : 0) | (alarmCode != ALM_NONE ? 16 : 0);
  r.feedToday = (uint8_t)min<int>(feedToday, 255);
  buf[bufHead] = r;
  bufHead = (bufHead + 1) % BUF_MAX;
  if (bufCount < BUF_MAX) bufCount++;
}

/** Gửi bù tối đa 5 bản ghi mỗi lần gọi; gói bù có cờ "buffered": true. */
void bufferFlush() {
  int sent = 0;
  while (bufCount > 0 && sent < 5 && mqtt.connected()) {
    int idx = (bufHead - bufCount + BUF_MAX) % BUF_MAX;
    BufRec &r = buf[idx];
    JsonDocument d;
    d["dev"] = deviceId; d["seq"] = ++telemetrySeq; d["buffered"] = true;
    d["ts"] = (double)r.t * 1000.0;
    if (r.temp != r.temp) d["temp"] = nullptr;          // NaN = loi cam bien
    else d["temp"] = round(r.temp * 10) / 10.0;
    d["chiller"] = (bool)(r.flags & 1); d["pump"] = (bool)(r.flags & 2); d["fan"] = (bool)(r.flags & 4);
    d["mode"] = (r.flags & 8) ? "AUTO" : "MANUAL";
    d["alarm"] = (r.flags & 16) ? "ALARM" : "NONE";
    d["feed_today"] = r.feedToday;
    String out; serializeJson(d, out);
    if (!mqtt.publish(topicTelemetry.c_str(), out.c_str())) break;
    bufCount--; sent++; bufferedSent++;
  }
  if (sent) Serial.printf("[BUF] Da gui bu %d ban ghi, con %d\n", sent, bufCount);
}

// ============================================================================
// 12. MQTT
// ============================================================================
void sendTelemetry() {
  JsonDocument d;
  d["dev"]        = deviceId;
  d["seq"]        = ++telemetrySeq;
  if (timeValid) d["ts"] = (double)time(nullptr) * 1000.0;      // epoch ms → backend/web tính độ trễ
  d["safe_mode"]  = safeMode;
  d["buffered"]   = false;
  d["buf_count"]  = bufCount;
  if (sensorFault) d["temp"] = nullptr; else d["temp"] = round(currentTemp * 10) / 10.0;
  d["chiller"]    = chillerOn;
  d["pump"]       = pumpOn;
  d["fan"]        = fanOn;
  d["mode"]       = autoMode ? "AUTO" : "MANUAL";
  d["feed_today"] = feedToday;
  d["feed_count"] = feedTotal;
  d["feeding"]    = feedState != FEED_IDLE;
  d["time"]       = fmtTime(true);
  d["time_src"]   = srcName(timeSrc);
  d["rtc_ok"]     = rtcPresent;
  if (rtcPresent) d["rtc_temp"] = rtc.getTemperature();
  d["next_feed"]  = nextFeedString();
  d["schedule"]   = scheduleToString();
  d["sp_on"]      = SP_ON;
  d["sp_off"]     = SP_OFF;
  d["alarm"]      = alarmName(alarmCode);
  d["rssi"]       = WiFi.RSSI();
  d["uptime"]     = millis() / 1000;
  d["fw"]         = FW_VERSION;
  d["db_ok"]      = dbOk;
  d["db_fail"]    = dbFail + dbDropped;
  d["lcd"]        = lcdStatus();           // "LCD1602@0x27" | "NONE"
  d["lcd_size"]   = lcdSizeName();
  d["i2c"]        = i2cList;               // thiết bị trên bus I2C
  d["ds_count"]   = dsCount;
  String out; serializeJson(d, out);
  bool ok = mqtt.publish(topicTelemetry.c_str(), out.c_str());
  static unsigned long lastLog = 0;                 // in log mỗi 10 s cho đỡ rối Serial
  if (!ok || millis() - lastLog >= 10000) {
    lastLog = millis();
    Serial.printf("[MQTT] %s %s  T=%s  So=%s  Bom=%s  (%u byte)\n", ok ? "SENT OK" : "SEND FAILED!", topicTelemetry.c_str(),
                  sensorFault ? "ERR" : String(currentTemp, 1).c_str(), chillerOn ? "ON" : "OFF", pumpOn ? "ON" : "OFF", (unsigned)out.length());
  }
}

void sendAck(const String &id, const String &device, bool ok, const String &msg) {
  if (!mqtt.connected()) return;
  JsonDocument a;
  a["id"] = id; a["device"] = device; a["ok"] = ok; a["msg"] = msg;
  String out; serializeJson(a, out);
  mqtt.publish(topicAck.c_str(), out.c_str());
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) { Serial.println("[MQTT] JSON loi"); return; }
  String id     = doc["id"] | "";          // Web gửi id để nhận ACK đúng nút
  String device = doc["device"] | "";
  String action = doc["action"] | "";
  unsigned long now = millis();
  bool ok = true; String msg = "OK";
  Serial.printf("[MQTT] id=%s device=%s action=%s\n", id.c_str(), device.c_str(), action.c_str());

  if (device == "chiller") {
    if (autoMode) publishEvent("manual_override", String("Web bat tay so lanh -> MANUAL (") + action + ")");
    autoMode = false; manualSinceMs = now; manualChillerReq = (action == "ON");
    setChiller(manualChillerReq, now, "WEB");
    if (manualChillerReq && !chillerOn) {
      ok = pumpWanted;
      msg = pumpWanted ? "Da nhan – cho so nghi du 60 s moi bat" : "Bom dang tat – khong the bat so";
    } else msg = chillerOn ? "So lanh: ON (MANUAL 30 phut)" : "So lanh: OFF (MANUAL 30 phut)";
  } else if (device == "pump") {
    pumpWanted = (action == "ON");
    if (feedState == FEED_IDLE) pumpOn = pumpWanted;
    if (!pumpWanted) setChiller(false, now, "bom tat");
    applyOutputs(now); beep(1);
    msg = pumpWanted ? "Bom: ON" : "Bom: OFF (so lanh bi khoa)";
  } else if (device == "mode") {
    autoMode = (action != "MANUAL"); manualSinceMs = now; manualChillerReq = chillerOn;
    msg = String("Che do: ") + (autoMode ? "AUTO" : "MANUAL");
  } else if (device == "feed") {
    if (lastFeedEpoch && timeValid && (uint32_t)time(nullptr) - lastFeedEpoch < 60) {
      ok = false; msg = "Vua cho an < 60 s"; publishEvent("feed_reject", msg);
    } else if (!startFeeding("WEB")) {
      ok = false; msg = feedState != FEED_IDLE ? "Dang cho an" : "Vuot gioi han so lan/ngay";
    } else msg = "Dang cho ca an";
  } else if (device == "test") {
    selfTestRequested = true; msg = "Bat dau Self-Test";
  } else if (device == "schedule") {
    String sl = doc["slots"] | "";
    if (parseSchedule(sl)) { prefs.putString("sched", scheduleToString()); publishEvent("schedule", scheduleToString()); msg = "Lich: " + scheduleToString(); }
    else { ok = false; msg = "Lich sai dinh dang, vd: 07:00,17:30"; publishEvent("error", msg); }
  } else if (device == "setpoint") {
    float on = doc["on"] | SP_ON, off = doc["off"] | SP_OFF;
    if (on >= 22 && on <= 30 && off >= 20 && off <= on - 0.5) {
      SP_ON = on; SP_OFF = off; prefs.putFloat("spOn", on); prefs.putFloat("spOff", off);
      msg = String("Nguong ") + String(off, 1) + " - " + String(on, 1);
      publishEvent("setpoint", msg);
    } else { ok = false; msg = "Setpoint khong hop le"; publishEvent("error", msg); }
  } else if (device == "time") {                      // Web gửi giờ trình duyệt khi không có NTP
    uint32_t e = doc["epoch"].as<uint32_t>();
    if (e > 1704067200UL) {
      setSystemTime((time_t)e); timeValid = true;
      if (!everNtp) timeSrc = SRC_WEB;
      writeRtcFromSystem(); publishEvent("time_sync", String("WEB -> RTC ") + fmtTime(true));
      msg = "Da dat gio " + fmtTime(true);
    } else { ok = false; msg = "Epoch khong hop le"; }
  } else if (device == "lcd") {                      // chẩn đoán màn hình từ web
    if (action == "1602" || action == "2004") {
      setLcdSize(action == "2004" ? 4 : 2);
      msg = String("Da doi -> ") + lcdStatus();
    } else if (action == "TEST") {
      lcdTestPattern(); ok = lcdPresent;
      msg = lcdPresent ? "To kin o 1.5 s: " + lcdStatus() : String("Khong thay LCD tren I2C");
    } else if (action == "SCAN") {
      i2cList = i2cScan(); printI2cReport();
      if (!lcdPresent) lcdInit();
      msg = "I2C: " + (i2cList.length() ? i2cList : String("khong co thiet bi")) + " | " + lcdStatus();
    } else if (action == "BL_ON" || action == "BL_OFF") {
      lcdBacklight = (action == "BL_ON");
      if (lcdPresent) { Wire.beginTransmission(lcdAddr); Wire.write(lcdBacklight ? LCD_BL : 0); Wire.endTransmission(); }
      msg = lcdBacklight ? "Den nen: BAT" : "Den nen: TAT";
    } else { ok = false; msg = "action: 1602 | 2004 | TEST | SCAN | BL_ON | BL_OFF"; }
  } else if (device == "alarm" && action == "MUTE") {
    alarmMuted = true; msg = "Da tat coi";
  } else {
    ok = false; msg = "Lenh khong ho tro";
  }

  { JsonDocument r; r["type"] = "command";
    r["detail"] = (device + " " + action + (ok ? " -> OK" : " -> FAIL: ") + (ok ? "" : msg)).substring(0, 200);
    dbEnqueue("events", r); }
  sendAck(id, device, ok, msg);
  sendTelemetry();
}

void handleConnectivity(unsigned long now) {
  static bool wifiWasUp = false;
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasUp) { wifiWasUp = false; Serial.println("[WIFI] Mat ket noi Wi-Fi"); }
    if (now - lastWifiTryMs >= WIFI_RETRY_MS) {
      lastWifiTryMs = now;
      wl_status_t ws = WiFi.status();
      // 1=NO_SSID (sai tên/không phải 2.4GHz) · 4=CONNECT_FAILED (sai mật khẩu) · 6=DISCONNECTED
      Serial.printf("[WIFI] Chua ket noi \"%s\" (status=%d) -> thu lai\n", WIFI_SSID, (int)ws);
      WiFi.disconnect(); delay(100); WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    return;
  }
  if (!wifiWasUp) { wifiWasUp = true; Serial.printf("[WIFI] Da ket noi, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI()); }
  static bool mqttWasUp = false;
  if (!mqtt.connected()) {
    if (mqttWasUp) {
      mqttWasUp = false; mqttLostAtMs = now;
      Serial.printf("[MQTT] Mat ket noi broker (state=%d) -> web se bao OFFLINE\n", mqtt.state());
    }
    if (!safeMode && mqttLostAtMs && now - mqttLostAtMs > 30000) {   // mất broker > 30 s
      safeMode = true; autoMode = true;                              // tự điều khiển cục bộ
      Serial.println("[SAFE] Mat broker > 30 s -> SAFE MODE, tu dieu khien cuc bo (AUTO)");
    }
    if (now - lastMqttTryMs < MQTT_RETRY_MS) return;
    lastMqttTryMs = now;
    const char* user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
    const char* pass = strlen(MQTT_PASS) ? MQTT_PASS : nullptr;
    if (mqtt.connect(deviceId.c_str(), user, pass, topicStatus.c_str(), 1, true, "offline")) {
      Serial.printf("[MQTT] Da ket noi %s:%d  topic=%s/*\n", MQTT_HOST, MQTT_PORT, TOPIC_BASE);
      mqttWasUp = true;
      if (safeMode || mqttLostAtMs) {
        unsigned long downSec = mqttLostAtMs ? (now - mqttLostAtMs) / 1000 : 0;
        publishEvent("recovered", String("Mat ket noi ") + downSec + " s, dem " + bufCount + " ban ghi cho gui bu");
        safeMode = false; mqttLostAtMs = 0;
      }
      mqtt.publish(topicStatus.c_str(), "online", true);
      mqtt.subscribe(topicCommand.c_str());
      publishEvent("boot", String("FW ") + FW_VERSION);
    } else {
      // -2 mạng/TLS · 4 sai user/pass · 5 không có quyền (HiveMQ Cloud)
      Serial.printf("[MQTT] Ket noi that bai, state=%d\n", mqtt.state());
    }
    return;
  }
  mqtt.loop();
}

// ============================================================================
// 13. NÚT NHẤN & BÁO ĐỘNG
// ============================================================================
void handleButtons(unsigned long now) {
  bool f = digitalRead(PIN_BTN_FEED);
  if (f != btnFeedPrev && now - btnFeedChangeMs > 50) {
    btnFeedChangeMs = now; btnFeedPrev = f;
    if (f == LOW) startFeeding("NUT");
  }
  bool t = digitalRead(PIN_BTN_TEST);
  if (t == LOW && btnTestPrev == HIGH) btnTestDownMs = now;
  if (t == LOW && btnTestDownMs && now - btnTestDownMs >= 2000) { btnTestDownMs = 0; selfTestRequested = true; }
  if (t == HIGH) btnTestDownMs = 0;
  btnTestPrev = t;
}

void handleAlarm(unsigned long now) {
  if (alarmCode != lastAlarm) {
    if (alarmCode != ALM_NONE) publishEvent("alarm", alarmName(alarmCode));
    lastAlarm = alarmCode; alarmMuted = false;
  }
  if (alarmCode == ALM_NONE || alarmCode == ALM_TIME || alarmMuted) return;
  if (now - lastAlarmBeepMs >= 10000) { lastAlarmBeepMs = now; beep(3, 150, 100); }
}

// ============================================================================
// 13b. LỆNH QUA SERIAL MONITOR – test relay KHÔNG cần web/MQTT
//      Gõ vào ô Serial (Newline): so on | so off | bom on | bom off | an | auto | test | status
//                                 lcd test | lcd 1602 | lcd 2004 | i2c
// ============================================================================
void handleSerialCommand(unsigned long now) {
  if (!Serial.available()) return;
  String c = Serial.readStringUntil('\n');
  c.trim(); c.toLowerCase();
  if (c.length() == 0) return;
  Serial.printf("[CMD SERIAL] %s\n", c.c_str());
  if (c == "so on" || c == "so off") {
    autoMode = false; manualSinceMs = now; manualChillerReq = (c == "so on");
    setChiller(manualChillerReq, now, "SERIAL");
  } else if (c == "bom on" || c == "bom off") {
    pumpWanted = (c == "bom on");
    if (feedState == FEED_IDLE) pumpOn = pumpWanted;
    if (!pumpWanted) setChiller(false, now, "bom tat");
    applyOutputs(now);
  } else if (c == "an") {
    startFeeding("SERIAL");
  } else if (c == "auto") {
    autoMode = true; Serial.println("  -> AUTO");
  } else if (c == "test") {
    selfTestRequested = true;
  } else if (c == "lcd 1602" || c == "lcd 2004") {
    setLcdSize(c == "lcd 2004" ? 4 : 2);
  } else if (c == "lcd test" || c == "lcd") {
    lcdTestPattern();
  } else if (c == "i2c") {
    i2cList = i2cScan(); printI2cReport();
    if (!lcdPresent && lcdInit()) Serial.printf("[LCD] Da nhan: %s\n", lcdStatus().c_str());
  } else if (c == "status") {
    Serial.printf("  mode=%s so=%s bom=%s quat=%s sensor=%s T=%.1f WiFi=%s MQTT=%s\n", autoMode ? "AUTO" : "MANUAL",
                  chillerOn ? "ON" : "OFF", pumpOn ? "ON" : "OFF", fanOn ? "ON" : "OFF", sensorFault ? "LOI" : "OK",
                  currentTemp, WiFi.status() == WL_CONNECTED ? "OK" : "CHUA", mqtt.connected() ? "OK" : "CHUA");
    Serial.printf("  LCD=%s  RTC=%s  DS18B20=%d cai  I2C=%s\n", lcdStatus().c_str(), rtcPresent ? "OK" : "KHONG",
                  dsCount, i2cList.length() ? i2cList.c_str() : "trong");
  } else {
    Serial.println("  Lenh: so on | so off | bom on | bom off | an | auto | test | status");
    Serial.println("        lcd test | lcd 1602 | lcd 2004 | i2c");
  }
}

// ============================================================================
// 14. SETUP & LOOP
// ============================================================================
void setup() {
  // Đặt mức an toàn cho relay TRƯỚC khi cấu hình OUTPUT (tránh chớp relay lúc boot)
  digitalWrite(PIN_RELAY_CHILLER, RELAY_OFF);
  digitalWrite(PIN_RELAY_PUMP, RELAY_ON);
  pinMode(PIN_RELAY_CHILLER, OUTPUT);
  pinMode(PIN_RELAY_PUMP, OUTPUT);
  pinMode(PIN_FAN_MOSFET, OUTPUT);   digitalWrite(PIN_FAN_MOSFET, LOW);
  pinMode(PIN_BUZZER, OUTPUT);       digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BTN_TEST, INPUT_PULLUP);
  pinMode(PIN_BTN_FEED, INPUT_PULLUP);

  Serial.begin(115200);
  delay(200);
  Serial.printf("\n=== SMART AQUARIUM FW %s ===\n", FW_VERSION);
  Serial.println("Go lenh: so on | so off | bom on | bom off | an | auto | status | lcd test | lcd 1602 | lcd 2004 | i2c");
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char* why = rr == ESP_RST_POWERON ? "Bat nguon" : rr == ESP_RST_SW ? "Reset mem" :
                      rr == ESP_RST_PANIC ? "CRASH (loi code/tran stack)" : rr == ESP_RST_BROWNOUT ? "BROWNOUT – SUT AP NGUON!" :
                      rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ? "WATCHDOG (treo)" :
                      rr == ESP_RST_EXT ? "Nut RESET" : "Khac";
    Serial.printf("[SYS] Ly do khoi dong: %s (%d) | heap trong: %u byte\n", why, (int)rr, (unsigned)ESP.getFreeHeap());
  }

  loadSettings();                       // đọc NVS trước (cần kích thước LCD đã lưu)

  {
    bool puSda = i2cLineHasPullup(PIN_I2C_SDA), puScl = i2cLineHasPullup(PIN_I2C_SCL);
    Serial.printf("[I2C] Dien day: SDA(GPIO%d)=%s | SCL(GPIO%d)=%s\n",
                  PIN_I2C_SDA, puSda ? "co module" : "TRONG", PIN_I2C_SCL, puScl ? "co module" : "TRONG");
    if (!puSda || !puScl)
      Serial.println("[I2C] Day TRONG = chua cam vao chan do hoac LCD/RTC chua co nguon -> nap test_tim_chan_i2c.ino de do");
  }
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);   // 100 kHz: ổn định với dây breadboard
  Wire.setTimeOut(50);                  // I2C lỗi không làm treo loop lâu
  delay(50);                            // LCD cần ~50 ms sau cấp nguồn
  i2cList = i2cScan();
  printI2cReport();
  if (lcdInit()) {
    Serial.printf("[LCD] Tim thay %s. Sai kich thuoc -> go 'lcd 1602' / 'lcd 2004'. Khong thay chu -> vat bien tro\n",
                  lcdStatus().c_str());
    lcdSplash("BE CA SMART", String("FW ") + FW_VERSION);
  } else {
    Serial.println("[LCD] KHONG thay LCD (PCF8574 0x20-0x27 / 0x38-0x3F) -> loi day/nguon. Firmware van chay, tu thu lai moi 5 s");
  }

  initTime();

#if DS18B20_INTERNAL_PULLUP
  pinMode(PIN_DS18B20, INPUT_PULLUP);   // trở kéo nội ~45k thay cho trở 4.7k ngoài
#endif
  ds18b20.begin();
#if DS18B20_INTERNAL_PULLUP
  pinMode(PIN_DS18B20, INPUT_PULLUP);   // bật lại sau begin() cho chắc
#endif
  dsCount = ds18b20.getDeviceCount();
  Serial.printf("[DS18B20] Tim thay %d cam bien tren GPIO%d\n", dsCount, PIN_DS18B20);
  ds18b20.setResolution(11);
  ds18b20.setWaitForConversion(false);

  feedServo.attach(PIN_SERVO_FEED, 500, 2400);   // đưa hộc về 0°
  feedServo.write(0); delay(300); feedServo.detach();

  uint64_t mac = ESP.getEfuseMac();                 // 48-bit MAC → client ID duy nhất (trùng ID = broker đá nhau → offline)
  char id[32]; snprintf(id, sizeof(id), "aquarium-%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  deviceId = id;
  Serial.printf("[SYS] MQTT client ID: %s\n", id);
  topicTelemetry = String(TOPIC_BASE) + "/telemetry";
  topicCommand   = String(TOPIC_BASE) + "/command";
  topicStatus    = String(TOPIC_BASE) + "/status";
  topicEvent     = String(TOPIC_BASE) + "/event";
  topicAck       = String(TOPIC_BASE) + "/ack";

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);                 // tắt modem-sleep → MQTT không bị rớt
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiTryMs = millis();

  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(TZ_INFO, NTP_1, NTP_2);

  if (MQTT_USE_TLS) { mqttSecure.setInsecure(); mqtt.setClient(mqttSecure); }   // HiveMQ Cloud dùng TLS
  else              { mqtt.setClient(mqttPlain); }
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
  mqtt.setSocketTimeout(5);
  mqtt.setKeepAlive(30);                // chịu được loop bận lâu hơn trước khi broker báo offline

  dbActive = DB_ENABLED && strstr(API_BASE_URL, "YOUR-APP") == nullptr && strncmp(API_BASE_URL, "https://", 8) == 0;
  Serial.printf("[DB] %s\n", dbActive ? API_BASE_URL : "TAT (chua sua API_BASE_URL) – MQTT/web van chay binh thuong");
  if (dbActive) {
    dbQueue = xQueueCreate(40, sizeof(DbJob));
    xTaskCreatePinnedToCore(dbTask, "dbTask", 16384, nullptr, 1, nullptr, 0);  // HTTPS/TLS cần stack lớn (8 KB dễ tràn → crash → offline)
  }

  beep(1);
  lastTickMs = millis();
  applyOutputs(millis());
}

void loop() {
  unsigned long now = millis();

  handleConnectivity(now);
  handleTimeSync(now);
  updateSensor(now);
  updateFeeding(now);
  updateBuzzer(now);
  handleButtons(now);

  // Nhịp 1 giây: ưu tiên xung SQW của DS3231, dự phòng bằng millis()
  bool secondTick = false;
  if (rtcTickFlag) { rtcTickFlag = false; secondTick = true; lastTickMs = now; }
  else if (now - lastTickMs >= 1500) { secondTick = true; lastTickMs = now; }
  if (secondTick) {
    checkSchedule();
    applyOutputs(now);          // cập nhật quạt chạy trễ
    handleAlarm(now);
  }

  handleLcdHotplug(now);
  if (lcdPresent && now - lastLcdMs >= LCD_PERIOD_MS) { lastLcdMs = now; renderLCD(now); }

  if (now - lastTelemetryMs >= TELEMETRY_PERIOD_MS) {
    lastTelemetryMs = now;
    if (mqtt.connected()) sendTelemetry();
    else bufferPush(now);                 // mất mạng: cất vào bộ đệm
  }

  static unsigned long lastFlushMs = 0;   // có mạng lại: gửi bù 5 bản ghi/200 ms
  if (bufCount > 0 && mqtt.connected() && now - lastFlushMs >= 200) { lastFlushMs = now; bufferFlush(); }

  if (now - lastDbTelemetryMs >= DB_TELEMETRY_PERIOD_MS) {
    lastDbTelemetryMs = now;
    dbLogTelemetry();
  }

  if (selfTestRequested) { selfTestRequested = false; runSelfTest(); }
  handleSerialCommand(now);

  static unsigned long lastBeat = 0;               // nhịp tim Serial: nhìn là biết đang kẹt ở đâu
  if (now - lastBeat >= 10000) {
    lastBeat = now;
    Serial.printf("[SYS] uptime %lus | WiFi %s | MQTT %s (state=%d) | T=%s | LCD %s | heap %u\n",
                  now / 1000, WiFi.status() == WL_CONNECTED ? "OK" : "CHUA",
                  mqtt.connected() ? "OK" : "CHUA", mqtt.state(),
                  sensorFault ? "ERR" : String(currentTemp, 1).c_str(), lcdStatus().c_str(), (unsigned)ESP.getFreeHeap());
  }
}
