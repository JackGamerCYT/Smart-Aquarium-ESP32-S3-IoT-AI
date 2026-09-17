/*
 * ============================================================================
 *  SMART AQUARIUM IoT – ESP32-S3 DevKitC-1 (N8R8 / N16R8)
 *  Firmware v2.2.0 – Tuần 6: System Integration + RTC DS3231 + Vercel/Postgres + HiveMQ
 * ============================================================================
 *  Thay đổi chính so với v1:
 *   - Thêm RTC DS3231 (I2C chung bus với OLED) + đồng bộ NTP -> lịch cho ăn
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
 *  Thư viện (Library Manager): PubSubClient, ArduinoJson (v7), OneWire,
 *  DallasTemperature, ESP32Servo, Adafruit GFX, Adafruit SH110X
 *  (hoặc Adafruit SSD1306), RTClib (Adafruit).
 *  Board: esp32 by Espressif >= 3.0  ->  "ESP32S3 Dev Module"
 * ============================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// ============================================================================
// 0. CHỌN DRIVER OLED
//    OLED 1.3" hầu hết dùng chip SH1106 -> để 1. Nếu module ghi SSD1306 -> 0.
//    (Dùng sai driver sẽ bị lệch 2 cột pixel / nhiễu ở mép màn hình.)
// ============================================================================
#define OLED_USE_SH1106 1

#if OLED_USE_SH1106
  #include <Adafruit_SH110X.h>
  Adafruit_SH1106G display(128, 64, &Wire, -1);
  #define OLED_WHITE SH110X_WHITE
  #define OLED_BEGIN() display.begin(0x3C, true)
#else
  #include <Adafruit_SSD1306.h>
  Adafruit_SSD1306 display(128, 64, &Wire, -1);
  #define OLED_WHITE SSD1306_WHITE
  #define OLED_BEGIN() display.begin(SSD1306_SWITCHCAPVCC, 0x3C)
#endif

// ============================================================================
// 1. SƠ ĐỒ CHÂN – GIỮ NGUYÊN CHÂN MẠCH CŨ + thêm chân mới cho RTC/quạt/nút
//    Lưu ý: GPIO19 trùng USB D- → nạp code qua cổng "UART/COM", không dùng cổng "USB" (OTG).
//    Chân mới (7, 15, 16) không đụng chân cũ; không nối thì firmware vẫn chạy.
// ============================================================================
#define PIN_DS18B20        4    // 1-Wire, trở kéo 4.7k lên 3V3 (KHÔNG lên 5V)
#define PIN_I2C_SDA        8    // OLED 0x3C + DS3231 0x68 (+ EEPROM 0x57)
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
// 2. CẤU HÌNH
// ============================================================================
const char* FW_VERSION   = "2.2.0";
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
const char* TOPIC_BASE   = "hcmute/esd/beca-nhomXX";

const char* TZ_INFO      = "ICT-7";            // Việt Nam UTC+7, không DST
const char* NTP_1        = "pool.ntp.org";
const char* NTP_2        = "time.google.com";

// ---- Database qua API Vercel (xem docs/05_WEB_DATABASE.md). DB_ENABLED=false để tắt ----
const bool  DB_ENABLED        = true;
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
const unsigned long OLED_PERIOD_MS       = 500;
const unsigned long CHILLER_MIN_OFF_MS   = 60000UL;   // chống đóng/ngắt relay liên tục
const unsigned long FAN_POSTRUN_MS       = 60000UL;   // quạt chạy thêm sau khi tắt sò
const unsigned long MANUAL_TIMEOUT_MS    = 30UL * 60 * 1000; // MANUAL tự về AUTO
const unsigned long PUMP_RESUME_DELAY_MS = 5000;      // bơm chạy lại sau khi hộc đóng
const unsigned long WIFI_RETRY_MS        = 30000;
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

// Thời gian
bool rtcPresent = false;
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
unsigned long lastWifiTryMs = 0, lastMqttTryMs = 0, lastTelemetryMs = 0, lastOledMs = 0;
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
  if (!DB_ENABLED || dbQueue == nullptr || deviceId.length() < 3) return;
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
}

void setChiller(bool on, unsigned long now, const char* reason) {
  if (on == chillerOn) return;
  if (on && !pumpWanted) return;                                         // khóa liên động
  if (on && chillerOffAtMs != 0 && now - chillerOffAtMs < CHILLER_MIN_OFF_MS) return;
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
  if (sensorFault) { alarmCode = ALM_SENSOR; setChiller(false, now, "loi cam bien"); return; }
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
void updateSensor(unsigned long now) {
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
// 10. OLED
// ============================================================================
void renderOLED() {
  display.clearDisplay();
  display.setTextColor(OLED_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);  display.print(fmtTime(false));
  display.setCursor(62, 0);
  display.print(WiFi.status() == WL_CONNECTED ? "W+" : "W-");
  display.print(mqtt.connected() ? " M+" : " M-");
  display.print(rtcPresent ? " R+" : " R-");
  display.drawLine(0, 9, 127, 9, OLED_WHITE);

  display.setCursor(0, 13);
  if (sensorFault) { display.setTextSize(2); display.print("ERR"); }
  else { display.setTextSize(2); display.print(currentTemp, 1);
         display.setTextSize(1); display.write(247); display.print("C"); }

  display.setTextSize(1);
  display.setCursor(86, 13); display.print(autoMode ? "AUTO" : "MANU");
  display.setCursor(86, 23); display.print(chillerOn ? "LANH:ON" : "LANH:--");

  display.setCursor(0, 33);
  if (alarmCode != ALM_NONE) { display.print("! "); display.print(alarmName(alarmCode)); }
  else if (feedState != FEED_IDLE) display.print("Dang cho ca an...");
  else { display.print("Cho an ke: "); display.print(nextFeedString()); }

  display.drawLine(0, 43, 127, 43, OLED_WHITE);
  display.setCursor(0, 46);
  display.print("Bom:"); display.print(pumpOn ? "ON " : "OFF");
  display.print(" Quat:"); display.print(fanOn ? "ON" : "OFF");
  display.setCursor(0, 56);
  display.print("An:"); display.print(feedToday); display.print("/ngay ");
  display.print(srcName(timeSrc));
  display.display();
}

// ============================================================================
// 11. SELF-TEST (chế độ bảo trì – cho phép blocking)
// ============================================================================
void runSelfTest() {
  Serial.println("\n[SELF-TEST] Bat dau");
  bool okTemp, okRtc;
  bool prevChiller = chillerOn;
  display.clearDisplay(); display.setTextSize(1); display.setTextColor(OLED_WHITE);
  display.setCursor(18, 0); display.println("=== SELF-TEST ==="); display.display();

  digitalWrite(PIN_BUZZER, HIGH); delay(100); digitalWrite(PIN_BUZZER, LOW);
  display.println("1.Coi          OK"); display.display(); delay(300);

  digitalWrite(PIN_RELAY_CHILLER, RELAY_ON); delay(800); digitalWrite(PIN_RELAY_CHILLER, RELAY_OFF);
  display.println("2.Relay Chiller"); display.display(); delay(300);

  digitalWrite(PIN_RELAY_PUMP, RELAY_OFF); delay(800); digitalWrite(PIN_RELAY_PUMP, RELAY_ON);
  digitalWrite(PIN_FAN_MOSFET, HIGH); delay(800); digitalWrite(PIN_FAN_MOSFET, LOW);
  display.println("3.Bom + Quat"); display.display();

  feedServo.attach(PIN_SERVO_FEED, 500, 2400);
  for (int p = 0; p <= SERVO_OPEN_DEG; p += 5) { feedServo.write(p); delay(20); }
  delay(300);
  for (int p = SERVO_OPEN_DEG; p >= 0; p -= 5) { feedServo.write(p); delay(20); }
  delay(200); feedServo.detach();
  display.println("4.Servo MG90S"); display.display();

  ds18b20.setWaitForConversion(true); ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0); ds18b20.setWaitForConversion(false);
  okTemp = !(t == DEVICE_DISCONNECTED_C || t == 85.0f);
  display.print("5.DS18B20 "); display.println(okTemp ? String(t, 1) + "C" : String("FAIL")); display.display();

  okRtc = rtcPresent && !rtc.lostPower();
  display.print("6.RTC "); display.println(okRtc ? fmtTime(false) : String("FAIL")); display.display();

  beep(okTemp && okRtc ? 2 : 4);
  chillerOn = prevChiller; applyOutputs(millis());
  publishEvent("self_test", String("DS18B20=") + (okTemp ? "PASS" : "FAIL") + " RTC=" + (okRtc ? "PASS" : "FAIL"));
  delay(2000);
}

// ============================================================================
// 12. MQTT
// ============================================================================
void sendTelemetry() {
  JsonDocument d;
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
  String out; serializeJson(d, out);
  mqtt.publish(topicTelemetry.c_str(), out.c_str());
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
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiTryMs >= WIFI_RETRY_MS) { lastWifiTryMs = now; WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS); }
    return;
  }
  if (!mqtt.connected()) {
    if (now - lastMqttTryMs < MQTT_RETRY_MS) return;
    lastMqttTryMs = now;
    const char* user = strlen(MQTT_USER) ? MQTT_USER : nullptr;
    const char* pass = strlen(MQTT_PASS) ? MQTT_PASS : nullptr;
    if (mqtt.connect(deviceId.c_str(), user, pass, topicStatus.c_str(), 1, true, "offline")) {
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

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  if (OLED_BEGIN()) {
    display.clearDisplay(); display.cp437(true);
    display.setTextColor(OLED_WHITE); display.setTextSize(1);
    display.setCursor(12, 20); display.println("BE CA SMART AI");
    display.setCursor(30, 36); display.print("FW "); display.println(FW_VERSION);
    display.display();
  } else Serial.println("[OLED] Khong tim thay 0x3C");

  loadSettings();
  initTime();

  ds18b20.begin();
  ds18b20.setResolution(11);
  ds18b20.setWaitForConversion(false);

  feedServo.attach(PIN_SERVO_FEED, 500, 2400);   // đưa hộc về 0°
  feedServo.write(0); delay(300); feedServo.detach();

  uint64_t mac = ESP.getEfuseMac();
  char id[24]; snprintf(id, sizeof(id), "aquarium-%06X", (uint32_t)(mac >> 24) & 0xFFFFFF);
  deviceId = id;
  topicTelemetry = String(TOPIC_BASE) + "/telemetry";
  topicCommand   = String(TOPIC_BASE) + "/command";
  topicStatus    = String(TOPIC_BASE) + "/status";
  topicEvent     = String(TOPIC_BASE) + "/event";
  topicAck       = String(TOPIC_BASE) + "/ack";

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiTryMs = millis();

  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(TZ_INFO, NTP_1, NTP_2);

  if (MQTT_USE_TLS) { mqttSecure.setInsecure(); mqtt.setClient(mqttSecure); }   // HiveMQ Cloud dùng TLS
  else              { mqtt.setClient(mqttPlain); }
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(768);
  mqtt.setSocketTimeout(3);

  if (DB_ENABLED) {
    dbQueue = xQueueCreate(40, sizeof(DbJob));
    xTaskCreatePinnedToCore(dbTask, "dbTask", 8192, nullptr, 1, nullptr, 0);   // loop() chạy core 1
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

  if (now - lastOledMs >= OLED_PERIOD_MS) { lastOledMs = now; renderOLED(); }

  if (now - lastTelemetryMs >= TELEMETRY_PERIOD_MS) {
    lastTelemetryMs = now;
    if (mqtt.connected()) sendTelemetry();
  }

  if (now - lastDbTelemetryMs >= DB_TELEMETRY_PERIOD_MS) {
    lastDbTelemetryMs = now;
    dbLogTelemetry();
  }

  if (selfTestRequested) { selfTestRequested = false; runSelfTest(); }
}
