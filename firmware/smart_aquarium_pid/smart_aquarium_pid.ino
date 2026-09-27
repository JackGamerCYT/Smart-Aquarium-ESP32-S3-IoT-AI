/*
 * ============================================================================
 *  SMART AQUARIUM IoT – ESP32-S3 DevKitC-1 (N16R8)
 *  Firmware v4.0.0 – Điều khiển nhiệt PID bằng 2 MOSFET IRLZ44N
 *  Môn Embedded System Design – GVHD Huỳnh Hoàng Hà – HCMUTE
 * ============================================================================
 *
 *  SƠ ĐỒ CHÂN (đúng sơ đồ hehehehe.pdsprj, KHÔNG thêm linh kiện):
 *    GPIO4  IN1 -> R1 2K2 -> Gate Q1 IRLZ44N -> SL : sò lạnh Peltier 12V   (PWM)
 *    GPIO5  IN2 -> R2 2K2 -> Gate Q2 IRLZ44N -> TO : lõi sưởi 5V (USB)     (PWM)
 *    GPIO6  cổng "SERVO 2" -> DS18B20 (chân 3 DATA, chân 2 VCC, chân 1 GND)
 *    GPIO7  cổng "SERVO 1" -> Servo MG90S cho ăn
 *    GPIO8  SDA / GPIO9 SCL -> OLED 0.96" 0x3C (U2 và cổng TGT)
 *    GPIO0  nút BOOT có sẵn trên DevKit -> bấm = cho ăn ngay
 *
 *  ĐIỀU KHIỂN:
 *    Một bộ PID duy nhất, đầu ra u trong [-100 .. +100] %:
 *      u > 0 -> SƯỞI  (PWM lõi sưởi = u %)
 *      u < 0 -> LÀM LẠNH (PWM sò = |u| %)
 *    => sò và sưởi KHÔNG BAO GIỜ chạy cùng lúc.
 *    Chế độ: AUTO (PID) | MANUAL (đặt % tay, tự về AUTO sau 30 phút) | OFF.
 *    An toàn: mất cảm biến -> tắt hết; ≥ 31°C -> cấm sưởi, làm lạnh tối đa;
 *             ≤ 20°C -> cấm làm lạnh, sưởi tối đa; tăng công suất có giới hạn
 *             tốc độ (chống sụt nguồn 5V khi bật sưởi).
 *
 *  KẾT NỐI: Wi-Fi + MQTT (HiveMQ). Backend Render nghe MQTT và ghi Postgres
 *  (Neon); Web dashboard (Vercel) đọc MQTT realtime + REST lịch sử.
 *
 *  THƯ VIỆN (Library Manager): PubSubClient (Nick O'Leary), ArduinoJson (v7).
 *  Không cần thư viện servo / OLED / DS18B20 – đã tự viết trong file này.
 *  Board: "ESP32S3 Dev Module". Chạy được ESP32 Arduino core 2.x và 3.x.
 * ============================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Preferences.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>
#include "driver/gpio.h"

// ============================================================================
// 1. CHÂN & CẤU HÌNH
// ============================================================================
#define PIN_COOL      4    // IN1 -> Q1 -> SL (sò lạnh 12V)
#define PIN_HEAT      5    // IN2 -> Q2 -> TO (lõi sưởi 5V)
#define PIN_DS18B20   6    // cổng SERVO 2 -> DS18B20 DATA
#define PIN_SERVO     7    // cổng SERVO 1 -> servo cho ăn
#define PIN_SDA       8
#define PIN_SCL       9
#define PIN_BTN       0    // nút BOOT trên DevKit

const char* FW_VERSION = "4.0.0";
const char* WIFI_SSID  = "HO TRO SINH VIEN";   // Wi-Fi 2.4 GHz
const char* WIFI_PASS  = "12345678@";
const char* MQTT_HOST  = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER  = "";
const char* MQTT_PASS  = "";
const char* TOPIC_BASE = "smartaquarium_node2026";  // PHẢI trùng backend (MQTT_TOPIC_BASE) và web
const char* TZ_INFO    = "ICT-7";                   // UTC+7

// ---- PWM MOSFET ----
// Gate nối qua R 2K2 nên MOSFET chuyển mạch chậm -> để tần số thấp cho MOSFET đỡ nóng.
const uint32_t PWM_FREQ_HZ = 200;
const uint8_t  PWM_RES     = 10;               // 0..1023
const uint32_t PWM_FULL    = 1UL << PWM_RES;   // 1024 = bật hẳn
const uint8_t  CH_COOL = 0, CH_HEAT = 1;       // core 2.x: kênh 0,1 dùng chung timer 0
const uint8_t  CH_SERVO = 2;                   // core 2.x: kênh 2 dùng timer 1 (50 Hz)

// ---- PID mặc định (sửa được từ web, lưu NVS) ----
const float DEF_SETPOINT = 26.0f;   // °C
const float DEF_KP = 60.0f;         // % / °C  (sai 1.7°C -> 100%)
const float DEF_KI = 0.01f;         // % / (°C·s)
const float DEF_KD = 0.0f;          // %·s / °C
const float DEF_DEADBAND = 0.10f;   // °C – sai số nhỏ hơn mức này coi như đạt
const float DEF_COOL_MAX = 100.0f;  // % tối đa cho sò
const float DEF_HEAT_MAX = 100.0f;  // % tối đa cho sưởi

const float    I_ZONE           = 2.0f;    // chỉ tích phân khi |sai số| < 2°C (chống vọt lố khi mới bật)
const float    U_MIN_PCT        = 3.0f;    // |u| nhỏ hơn -> tắt (tránh xung quá ngắn)
const float    SLEW_UP_PCT_S    = 20.0f;   // tăng công suất tối đa 20 %/s (giảm thì tức thời)
const float    TEMP_HOT         = 31.0f;   // ≥: cấm sưởi, làm lạnh tối đa
const float    TEMP_COLD        = 20.0f;   // ≤: cấm làm lạnh, sưởi tối đa
const float    AUTO_CUTOFF      = 1.5f;    // AUTO: sưởi tắt nếu T > SP+1.5, sò tắt nếu T < SP-1.5
const float    TEMP_VALID_MIN   = 0.0f, TEMP_VALID_MAX = 50.0f;
const unsigned long CONTROL_MS  = 1000;
const unsigned long SENSOR_MS   = 1000;
const unsigned long DS_CONV_MS  = 800;     // 12 bit = 750 ms
const unsigned long SENSOR_STALE_MS = 5000;
const unsigned long MANUAL_TIMEOUT_MS = 30UL * 60 * 1000;

// ---- Cho ăn ----
const int  SERVO_OPEN_DEG     = 110;
const unsigned long SERVO_HOLD_MS = 1000;
const uint16_t SERVO_US_MIN = 500, SERVO_US_MAX = 2400;
const int  MAX_SLOTS          = 4;
const int  MAX_FEEDS_PER_DAY  = 6;
const int  MIN_FEED_GAP_MIN   = 30;   // lịch bỏ qua nếu vừa cho ăn tay < 30 phút
const int  FEED_GRACE_MIN     = 30;   // mất điện qua giờ ăn <= 30 phút -> cho ăn bù

// ---- Chu kỳ ----
const unsigned long TELEMETRY_MS  = 2000;
const unsigned long BUFFER_MS     = 60000;   // mất MQTT: 1 bản ghi/phút vào bộ đệm
const unsigned long WIFI_RETRY_MS = 10000;
const unsigned long MQTT_RETRY_MS = 5000;
const unsigned long SAFE_MODE_MS  = 30000;   // mất broker > 30 s -> SAFE MODE (vẫn tự điều khiển)
const unsigned long OLED_MS       = 500;

// ============================================================================
// 2. BIẾN TOÀN CỤC
// ============================================================================
WiFiClient   net;
PubSubClient mqtt(net);
Preferences  prefs;
String deviceId, tTelemetry, tCommand, tStatus, tEvent, tAck;

enum Mode { MODE_AUTO, MODE_MANUAL, MODE_OFF };
Mode mode = MODE_AUTO;
unsigned long manualSinceMs = 0;

struct PidCfg { float sp, kp, ki, kd, db, coolMax, heatMax; } cfg;
float pidP = 0, pidI = 0, pidD = 0, pidU = 0, pidPrevT = NAN, pidDFilt = 0;
float manCool = 0, manHeat = 0;      // % đặt tay (MANUAL)
float coolPct = 0, heatPct = 0;      // % đang xuất ra MOSFET

float  temp = NAN;
bool   sensorFault = true;
unsigned long lastTempOkMs = 0;
int    sensorErrCnt = 0;
bool   dsPresent = false, everTemp = false;
uint32_t dsReadOk = 0, dsReadFail = 0;

enum Alarm { AL_NONE, AL_SENSOR, AL_HOT, AL_COLD, AL_TIME };
Alarm alarmCode = AL_NONE;

bool timeValid = false, everNtp = false;
volatile bool ntpSyncFlag = false;
enum TimeSrc { SRC_NONE, SRC_NTP, SRC_WEB };
TimeSrc timeSrc = SRC_NONE;

struct Slot { uint8_t h, m; };
Slot slots[MAX_SLOTS]; int slotCount = 0;
enum FeedState { FEED_IDLE, FEED_OPEN, FEED_HOLD, FEED_CLOSE };
FeedState feedState = FEED_IDLE;
int servoPos = 0; unsigned long feedStepMs = 0; String feedSource;
uint16_t feedToday = 0; uint32_t feedTotal = 0, feedDayKey = 0, lastSchedFed = 0, lastFeedEpoch = 0;

bool oledOk = false; uint8_t oledColOff = 0;   // 0 = SSD1306 (0.96"), 2 = SH1106 (1.3")
String oledShadow[8];

uint32_t seq = 0;
unsigned long lastWifiTryMs = 0, lastMqttTryMs = 0, mqttLostSinceMs = 0;
bool safeMode = false;

struct BufRec { uint32_t ts; int16_t t10; int16_t sp10; int8_t cool, heat; uint8_t mode; };
const int BUF_MAX = 240;             // 4 giờ @ 1 bản ghi/phút
BufRec ring[BUF_MAX]; int ringHead = 0, ringCount = 0;

// ============================================================================
// 3. TIỆN ÍCH
// ============================================================================
const char* modeName(Mode m) { return m == MODE_AUTO ? "AUTO" : m == MODE_MANUAL ? "MANUAL" : "OFF"; }
const char* alarmName(Alarm a) {
  switch (a) { case AL_SENSOR: return "SENSOR_FAULT"; case AL_HOT: return "TEMP_HIGH";
               case AL_COLD: return "TEMP_LOW"; case AL_TIME: return "TIME_INVALID"; default: return "NONE"; }
}
const char* srcName(TimeSrc s) { return s == SRC_NTP ? "NTP" : s == SRC_WEB ? "WEB" : "NONE"; }

bool getLocal(struct tm &t) {
  if (!timeValid) return false;
  time_t e = time(nullptr);
  if (e < 1704067200) return false;       // < 2024
  localtime_r(&e, &t);
  return true;
}
String fmtTime(bool date) {
  struct tm t;
  if (!getLocal(t)) return date ? "----/--/-- --:--:--" : "--:--:--";
  char b[24];
  strftime(b, sizeof(b), date ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S", &t);
  return String(b);
}
uint64_t epochMs() {
  struct timeval tv; gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}
void onNtpSync(struct timeval*) { ntpSyncFlag = true; }

// ---- PWM (LEDC) cho cả core 2.x và 3.x ----
void pwmAttach(int pin, uint8_t ch, uint32_t freq, uint8_t res) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch; ledcAttach(pin, freq, res);
#else
  ledcSetup(ch, freq, res); ledcAttachPin(pin, ch);
#endif
}
void pwmWrite(int pin, uint8_t ch, uint32_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch; ledcWrite(pin, duty);
#else
  (void)pin; ledcWrite(ch, duty);
#endif
}

// ============================================================================
// 4. MQTT: sự kiện
// ============================================================================
void publishEvent(const String &type, const String &detail) {
  Serial.printf("[EVENT] %s: %s\n", type.c_str(), detail.c_str());
  if (!mqtt.connected()) return;
  JsonDocument d;
  d["type"] = type; d["detail"] = detail;
  if (timeValid) d["ts"] = epochMs();
  String out; serializeJson(d, out);
  mqtt.publish(tEvent.c_str(), out.c_str());
}

// ============================================================================
// 5. NVS
// ============================================================================
String scheduleToString() {
  String s;
  for (int i = 0; i < slotCount; i++) {
    char b[6]; snprintf(b, sizeof(b), "%02d:%02d", slots[i].h, slots[i].m);
    if (i) s += ",";
    s += b;
  }
  return s;
}

bool parseSchedule(const String &in) {
  Slot tmp[MAX_SLOTS]; int n = 0, start = 0;
  String s = in; s.trim();
  if (!s.length()) { slotCount = 0; return true; }
  while (start < (int)s.length()) {
    if (n >= MAX_SLOTS) return false;
    int comma = s.indexOf(',', start); if (comma < 0) comma = s.length();
    String it = s.substring(start, comma); it.trim();
    int c = it.indexOf(':');
    if (c < 1) return false;
    int h = it.substring(0, c).toInt(), m = it.substring(c + 1).toInt();
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    tmp[n].h = h; tmp[n].m = m; n++;
    start = comma + 1;
  }
  for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
    if (tmp[j].h * 60 + tmp[j].m < tmp[i].h * 60 + tmp[i].m) { Slot x = tmp[i]; tmp[i] = tmp[j]; tmp[j] = x; }
  memcpy(slots, tmp, sizeof(Slot) * n); slotCount = n;
  return true;
}

void loadSettings() {
  prefs.begin("aqpid", false);
  cfg.sp      = prefs.getFloat("sp", DEF_SETPOINT);
  cfg.kp      = prefs.getFloat("kp", DEF_KP);
  cfg.ki      = prefs.getFloat("ki", DEF_KI);
  cfg.kd      = prefs.getFloat("kd", DEF_KD);
  cfg.db      = prefs.getFloat("db", DEF_DEADBAND);
  cfg.coolMax = prefs.getFloat("coolMax", DEF_COOL_MAX);
  cfg.heatMax = prefs.getFloat("heatMax", DEF_HEAT_MAX);
  if (!parseSchedule(prefs.getString("sched", "07:00,17:30"))) parseSchedule("07:00,17:30");
  lastSchedFed  = prefs.getUInt("lastSched", 0);
  lastFeedEpoch = prefs.getUInt("lastFeed", 0);
  feedDayKey    = prefs.getUInt("feedDay", 0);
  feedToday     = prefs.getUShort("feedTod", 0);
  feedTotal     = prefs.getUInt("feedTot", 0);
  oledColOff    = prefs.getUChar("oledOff", 0) ? 2 : 0;
}

void savePid() {
  prefs.putFloat("sp", cfg.sp); prefs.putFloat("kp", cfg.kp); prefs.putFloat("ki", cfg.ki);
  prefs.putFloat("kd", cfg.kd); prefs.putFloat("db", cfg.db);
  prefs.putFloat("coolMax", cfg.coolMax); prefs.putFloat("heatMax", cfg.heatMax);
}

void saveFeedCounters() {
  prefs.putUInt("lastSched", lastSchedFed); prefs.putUInt("lastFeed", lastFeedEpoch);
  prefs.putUInt("feedDay", feedDayKey); prefs.putUShort("feedTod", feedToday); prefs.putUInt("feedTot", feedTotal);
}

// ============================================================================
// 6. DS18B20 – driver 1-Wire tự viết, KHÔNG cần điện trở kéo lên
//    Dây DATA chỉ có trở kéo nội ~45k của ESP32 nên lên mức 1 chậm:
//    - lúc chắc chắn cảm biến không kéo dây -> ESP32 ĐẨY THẲNG lên 3.3V (push-pull)
//    - chỉ thả dây (open-drain) khi cảm biến cần trả lời (presence, đọc bit)
//    - thời điểm lấy mẫu bit tự dò; mọi gói đều kiểm CRC
// ============================================================================
static portMUX_TYPE owMux = portMUX_INITIALIZER_UNLOCKED;
const gpio_num_t OW_PIN = (gpio_num_t)PIN_DS18B20;
const uint8_t OW_SAMPLE_US[] = { 12, 9, 15, 18, 22, 26, 30, 7 };
uint8_t owSampleIdx = 0; bool owSampleLocked = false;
bool convPending = false; unsigned long convStartMs = 0, lastSensorMs = 0;

inline void owHigh()    { gpio_set_level(OW_PIN, 1); gpio_set_direction(OW_PIN, GPIO_MODE_INPUT_OUTPUT); }
inline void owLowPP()   { gpio_set_level(OW_PIN, 0); }
inline void owToOD()    { gpio_set_direction(OW_PIN, GPIO_MODE_INPUT_OUTPUT_OD); }
inline void owRelease() { gpio_set_level(OW_PIN, 1); }
inline int  owIn()      { return gpio_get_level(OW_PIN); }

void owBegin() { gpio_reset_pin(OW_PIN); gpio_set_pull_mode(OW_PIN, GPIO_PULLUP_ONLY); owHigh(); }

bool owReset() {
  owHigh(); delayMicroseconds(10);
  owLowPP(); delayMicroseconds(490);
  portENTER_CRITICAL(&owMux);
  owHigh(); delayMicroseconds(2);
  owToOD(); owRelease();
  delayMicroseconds(68);
  bool present = owIn() == 0;
  portEXIT_CRITICAL(&owMux);
  delayMicroseconds(420);
  owHigh();
  return present;
}
void owWriteBit(uint8_t b) {
  portENTER_CRITICAL(&owMux);
  owLowPP(); delayMicroseconds(b ? 5 : 65);
  gpio_set_level(OW_PIN, 1);
  delayMicroseconds(b ? 65 : 5);
  portEXIT_CRITICAL(&owMux);
}
uint8_t owReadBit() {
  uint8_t us = OW_SAMPLE_US[owSampleIdx];
  portENTER_CRITICAL(&owMux);
  owLowPP(); owToOD(); delayMicroseconds(3);
  owRelease(); delayMicroseconds(us);
  uint8_t r = owIn();
  portEXIT_CRITICAL(&owMux);
  delayMicroseconds(us < 62 ? 65 - us : 3);
  owHigh(); delayMicroseconds(2);
  return r;
}
void owWrite(uint8_t v) { for (int i = 0; i < 8; i++) owWriteBit((v >> i) & 1); }
uint8_t owRead() { uint8_t v = 0; for (int i = 0; i < 8; i++) v |= owReadBit() << i; return v; }
uint8_t owCrc8(const uint8_t *d, int n) {
  uint8_t c = 0;
  while (n--) { uint8_t b = *d++; for (int i = 0; i < 8; i++) { uint8_t mix = (c ^ b) & 1; c >>= 1; if (mix) c ^= 0x8C; b >>= 1; } }
  return c;
}
void owNextSample() {
  if (owSampleLocked) { static uint8_t fails = 0; if (++fails < 5) return; fails = 0; owSampleLocked = false; }
  owSampleIdx = (owSampleIdx + 1) % sizeof(OW_SAMPLE_US);
}
bool dsReadRom() {
  for (uint8_t tries = 0; tries < sizeof(OW_SAMPLE_US) * 2; tries++) {
    if (!owReset()) return false;
    owWrite(0x33);
    uint8_t rom[8]; for (int i = 0; i < 8; i++) rom[i] = owRead();
    if (rom[0] == 0x28 && owCrc8(rom, 7) == rom[7]) { owSampleLocked = true; return true; }
    owNextSample();
  }
  return false;
}
bool dsStartConversion() {
  if (!owReset()) return false;
  owWrite(0xCC); owWrite(0x44);
  owHigh();                                 // giữ dây 3.3V cấp điện khi cảm biến đo
  return true;
}
#define DS_ERR -127.0f
float dsReadTemp() {
  for (int a = 0; a < 3; a++) {
    if (!owReset()) { dsReadFail++; return DS_ERR; }
    owWrite(0xCC); owWrite(0xBE);
    uint8_t sp[9]; for (int i = 0; i < 9; i++) sp[i] = owRead();
    bool allFF = true; for (int i = 0; i < 9; i++) if (sp[i] != 0xFF) allFF = false;
    if (!allFF && owCrc8(sp, 8) == sp[8]) {
      owSampleLocked = true; dsReadOk++;
      return (int16_t)((sp[1] << 8) | sp[0]) / 16.0f;
    }
    owNextSample();
  }
  dsReadFail++;
  return DS_ERR;
}
void dsScan(bool verbose) {
  owBegin();
  bool present = owReset();
  dsPresent = present && dsReadRom();
  if (!verbose) return;
  if (dsPresent) Serial.printf("[DS18B20] OK tren GPIO%d (lay mau %u us)\n", PIN_DS18B20, OW_SAMPLE_US[owSampleIdx]);
  else if (present) Serial.printf("[DS18B20] Co tra loi tren GPIO%d nhung doc loi -> tu thu lai\n", PIN_DS18B20);
  else Serial.printf("[DS18B20] KHONG thay cam bien o GPIO%d (cong SERVO 2: chan3 DATA, chan2 VCC, chan1 GND)\n", PIN_DS18B20);
}

void updateSensor(unsigned long now) {
  static unsigned long lastScan = 0;
  if ((sensorFault || !dsPresent) && !convPending && now - lastScan >= 10000) {
    lastScan = now;
    bool had = dsPresent;
    dsScan(false);
    if (dsPresent && !had) Serial.println("[DS18B20] Da tim lai cam bien");
  }
  if (!convPending && now - lastSensorMs >= SENSOR_MS) {
    lastSensorMs = now;
    if (dsStartConversion()) { convPending = true; convStartMs = now; }
    else sensorErrCnt++;
  }
  if (convPending && now - convStartMs >= DS_CONV_MS) {
    convPending = false;
    float t = dsReadTemp();
    bool bad = t == DS_ERR || t == 85.0f || t < TEMP_VALID_MIN || t > TEMP_VALID_MAX;
    if (bad) sensorErrCnt++;
    else {
      sensorErrCnt = 0; temp = t; lastTempOkMs = now;
      if (sensorFault) { sensorFault = false; if (everTemp) publishEvent("info", "DS18B20 hoat dong lai"); }
      everTemp = true;
    }
  }
  if (!sensorFault && (sensorErrCnt >= 3 || now - lastTempOkMs > SENSOR_STALE_MS)) {
    sensorFault = true;
    publishEvent("alarm", "Mat cam bien DS18B20 -> tat so va suoi");
  }
}

// ============================================================================
// 7. PID + MOSFET
// ============================================================================
void resetPid() { pidI = 0; pidP = pidD = pidU = 0; pidPrevT = NAN; pidDFilt = 0; }

/** Một bước PID. e = SP - T; u > 0 sưởi, u < 0 làm lạnh. */
float pidStep(float T, float dt) {
  float e = cfg.sp - T;
  float eDb = fabsf(e) <= cfg.db ? 0 : (e > 0 ? e - cfg.db : e + cfg.db);
  pidP = cfg.kp * eDb;
  float dRaw = isnan(pidPrevT) ? 0 : -(T - pidPrevT) / dt;     // đạo hàm theo giá trị đo (không giật khi đổi SP)
  pidPrevT = T;
  pidDFilt = 0.7f * pidDFilt + 0.3f * dRaw;
  pidD = cfg.kd * pidDFilt;
  float iNew = constrain(pidI + (fabsf(e) < I_ZONE ? cfg.ki * eDb * dt : 0.0f), -cfg.coolMax, cfg.heatMax);
  float uTry = pidP + pidD + iNew;
  bool satHi = uTry > cfg.heatMax && eDb > 0, satLo = uTry < -cfg.coolMax && eDb < 0;
  if (!satHi && !satLo) pidI = iNew;                            // chống bão hòa tích phân
  pidU = constrain(pidP + pidD + pidI, -cfg.coolMax, cfg.heatMax);
  return pidU;
}

void writeOutputs() {
  uint32_t dc = coolPct >= 99.95f ? PWM_FULL : (uint32_t)(coolPct * PWM_FULL / 100.0f);
  uint32_t dh = heatPct >= 99.95f ? PWM_FULL : (uint32_t)(heatPct * PWM_FULL / 100.0f);
  pwmWrite(PIN_COOL, CH_COOL, dc);
  pwmWrite(PIN_HEAT, CH_HEAT, dh);
}

void allOff() { coolPct = heatPct = 0; writeOutputs(); }

void updateAlarm() {
  if (millis() < 8000) return;                         // chờ cảm biến đọc lần đầu
  Alarm a = AL_NONE;
  if (sensorFault) a = AL_SENSOR;
  else if (temp >= TEMP_HOT) a = AL_HOT;
  else if (temp <= TEMP_COLD) a = AL_COLD;
  else if (!timeValid && slotCount > 0) a = AL_TIME;
  if (a != alarmCode) {
    alarmCode = a;
    publishEvent(a == AL_NONE ? "recovered" : "alarm", String("Canh bao: ") + alarmName(a));
  }
}

void controlStep(unsigned long now) {
  static unsigned long last = 0;
  float dt = last ? (now - last) / 1000.0f : CONTROL_MS / 1000.0f;
  last = now;
  if (dt <= 0) dt = 1;

  if (mode == MODE_MANUAL && now - manualSinceMs >= MANUAL_TIMEOUT_MS) {
    mode = MODE_AUTO; resetPid();
    publishEvent("mode", "MANUAL het 30 phut -> AUTO");
  }

  float coolT = 0, heatT = 0;
  if (mode == MODE_OFF || sensorFault) {
    resetPid();
  } else {
    if (mode == MODE_AUTO) {
      float u = pidStep(temp, dt);
      if (u >= U_MIN_PCT) heatT = u;
      else if (u <= -U_MIN_PCT) coolT = -u;
      if (temp > cfg.sp + AUTO_CUTOFF) heatT = 0;
      if (temp < cfg.sp - AUTO_CUTOFF) coolT = 0;
    } else {
      coolT = manCool; heatT = manHeat;
    }
    if (temp >= TEMP_HOT)  { heatT = 0; coolT = cfg.coolMax; }
    if (temp <= TEMP_COLD) { coolT = 0; heatT = cfg.heatMax; }
    coolT = constrain(coolT, 0.0f, cfg.coolMax);
    heatT = constrain(heatT, 0.0f, cfg.heatMax);
    if (coolT > 0 && heatT > 0) heatT = 0;             // khóa chéo tuyệt đối
  }

  // Giảm: tức thời. Tăng: tối đa SLEW_UP_PCT_S %/s. Đổi chiều: cái kia phải về 0 trước.
  float step = SLEW_UP_PCT_S * dt;
  coolPct = coolT < coolPct ? coolT : min(coolT, coolPct + step);
  heatPct = heatT < heatPct ? heatT : min(heatT, heatPct + step);
  if (coolPct > 0 && heatPct > 0) { if (coolT > 0) heatPct = 0; else coolPct = 0; }
  writeOutputs();
  updateAlarm();
}

// ============================================================================
// 8. SERVO CHO ĂN (LEDC 50 Hz, không cần thư viện)
// ============================================================================
void servoWriteDeg(int deg) {
  deg = constrain(deg, 0, 180);
  uint32_t us = map(deg, 0, 180, SERVO_US_MIN, SERVO_US_MAX);
  pwmWrite(PIN_SERVO, CH_SERVO, us * ((1UL << 14) - 1) / 20000UL);
}
void servoStop() { pwmWrite(PIN_SERVO, CH_SERVO, 0); }   // ngừng xung -> servo nghỉ, không rung

bool startFeeding(const String &src) {
  if (feedState != FEED_IDLE) return false;
  if (feedToday >= MAX_FEEDS_PER_DAY) { publishEvent("feed_reject", "Vuot gioi han " + String(MAX_FEEDS_PER_DAY) + " lan/ngay"); return false; }
  feedSource = src; servoPos = 0; servoWriteDeg(0);
  feedState = FEED_OPEN; feedStepMs = millis();
  return true;
}

void updateFeeding(unsigned long now) {
  switch (feedState) {
    case FEED_IDLE: return;
    case FEED_OPEN:
      if (now - feedStepMs >= 20) { feedStepMs = now; servoPos += 5; servoWriteDeg(servoPos);
        if (servoPos >= SERVO_OPEN_DEG) feedState = FEED_HOLD; }
      break;
    case FEED_HOLD:
      if (now - feedStepMs >= SERVO_HOLD_MS) { feedState = FEED_CLOSE; feedStepMs = now; }
      break;
    case FEED_CLOSE:
      if (now - feedStepMs >= 20) { feedStepMs = now; servoPos -= 5; servoWriteDeg(max(servoPos, 0));
        if (servoPos <= 0) {
          servoStop(); feedState = FEED_IDLE;
          feedToday++; feedTotal++;
          if (timeValid) lastFeedEpoch = (uint32_t)time(nullptr);
          saveFeedCounters();
          publishEvent("feed", feedSource + " - hom nay " + String(feedToday) + " lan");
        } }
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
    if ((uint32_t)slotE <= lastSchedFed) continue;
    lastSchedFed = (uint32_t)slotE;
    if (lastFeedEpoch && (uint32_t)nowE - lastFeedEpoch < (uint32_t)MIN_FEED_GAP_MIN * 60) {
      saveFeedCounters(); publishEvent("feed_skip", "Vua cho an < 30 phut, bo qua lich"); continue;
    }
    char b[6]; snprintf(b, sizeof(b), "%02d:%02d", slots[i].h, slots[i].m);
    if (!startFeeding(String("LICH ") + b)) saveFeedCounters();
    return;
  }
}

String nextFeed() {
  if (!slotCount) return "OFF";
  struct tm t;
  if (!getLocal(t)) return "--:--";
  int nowMin = t.tm_hour * 60 + t.tm_min, idx = 0;
  for (int i = 0; i < slotCount; i++) if (slots[i].h * 60 + slots[i].m > nowMin) { idx = i; break; }
  char b[6]; snprintf(b, sizeof(b), "%02d:%02d", slots[idx].h, slots[idx].m);
  return String(b);
}

// ============================================================================
// 9. OLED 128x64 (SSD1306 / SH1106) – driver chữ tự viết
// ============================================================================
static const uint8_t FONT5X7[] = {   // ASCII 0x20..0x7E, 5 byte/ký tự (font chuẩn 5x7)
  0x00, 0x00, 0x00, 0x00, 0x00,  // ' '
  0x00, 0x00, 0x5F, 0x00, 0x00,  // '!'
  0x00, 0x07, 0x00, 0x07, 0x00,  // '"'
  0x14, 0x7F, 0x14, 0x7F, 0x14,  // '#'
  0x24, 0x2A, 0x7F, 0x2A, 0x12,  // '$'
  0x23, 0x13, 0x08, 0x64, 0x62,  // '%'
  0x36, 0x49, 0x56, 0x20, 0x50,  // '&'
  0x00, 0x08, 0x07, 0x03, 0x00,  // "'"
  0x00, 0x1C, 0x22, 0x41, 0x00,  // '('
  0x00, 0x41, 0x22, 0x1C, 0x00,  // ')'
  0x2A, 0x1C, 0x7F, 0x1C, 0x2A,  // '*'
  0x08, 0x08, 0x3E, 0x08, 0x08,  // '+'
  0x00, 0x80, 0x70, 0x30, 0x00,  // ','
  0x08, 0x08, 0x08, 0x08, 0x08,  // '-'
  0x00, 0x00, 0x60, 0x60, 0x00,  // '.'
  0x20, 0x10, 0x08, 0x04, 0x02,  // '/'
  0x3E, 0x51, 0x49, 0x45, 0x3E,  // '0'
  0x00, 0x42, 0x7F, 0x40, 0x00,  // '1'
  0x72, 0x49, 0x49, 0x49, 0x46,  // '2'
  0x21, 0x41, 0x49, 0x4D, 0x33,  // '3'
  0x18, 0x14, 0x12, 0x7F, 0x10,  // '4'
  0x27, 0x45, 0x45, 0x45, 0x39,  // '5'
  0x3C, 0x4A, 0x49, 0x49, 0x31,  // '6'
  0x41, 0x21, 0x11, 0x09, 0x07,  // '7'
  0x36, 0x49, 0x49, 0x49, 0x36,  // '8'
  0x46, 0x49, 0x49, 0x29, 0x1E,  // '9'
  0x00, 0x00, 0x14, 0x00, 0x00,  // ':'
  0x00, 0x40, 0x34, 0x00, 0x00,  // ';'
  0x00, 0x08, 0x14, 0x22, 0x41,  // '<'
  0x14, 0x14, 0x14, 0x14, 0x14,  // '='
  0x00, 0x41, 0x22, 0x14, 0x08,  // '>'
  0x02, 0x01, 0x59, 0x09, 0x06,  // '?'
  0x3E, 0x41, 0x5D, 0x59, 0x4E,  // '@'
  0x7C, 0x12, 0x11, 0x12, 0x7C,  // 'A'
  0x7F, 0x49, 0x49, 0x49, 0x36,  // 'B'
  0x3E, 0x41, 0x41, 0x41, 0x22,  // 'C'
  0x7F, 0x41, 0x41, 0x41, 0x3E,  // 'D'
  0x7F, 0x49, 0x49, 0x49, 0x41,  // 'E'
  0x7F, 0x09, 0x09, 0x09, 0x01,  // 'F'
  0x3E, 0x41, 0x41, 0x51, 0x73,  // 'G'
  0x7F, 0x08, 0x08, 0x08, 0x7F,  // 'H'
  0x00, 0x41, 0x7F, 0x41, 0x00,  // 'I'
  0x20, 0x40, 0x41, 0x3F, 0x01,  // 'J'
  0x7F, 0x08, 0x14, 0x22, 0x41,  // 'K'
  0x7F, 0x40, 0x40, 0x40, 0x40,  // 'L'
  0x7F, 0x02, 0x1C, 0x02, 0x7F,  // 'M'
  0x7F, 0x04, 0x08, 0x10, 0x7F,  // 'N'
  0x3E, 0x41, 0x41, 0x41, 0x3E,  // 'O'
  0x7F, 0x09, 0x09, 0x09, 0x06,  // 'P'
  0x3E, 0x41, 0x51, 0x21, 0x5E,  // 'Q'
  0x7F, 0x09, 0x19, 0x29, 0x46,  // 'R'
  0x26, 0x49, 0x49, 0x49, 0x32,  // 'S'
  0x03, 0x01, 0x7F, 0x01, 0x03,  // 'T'
  0x3F, 0x40, 0x40, 0x40, 0x3F,  // 'U'
  0x1F, 0x20, 0x40, 0x20, 0x1F,  // 'V'
  0x3F, 0x40, 0x38, 0x40, 0x3F,  // 'W'
  0x63, 0x14, 0x08, 0x14, 0x63,  // 'X'
  0x03, 0x04, 0x78, 0x04, 0x03,  // 'Y'
  0x61, 0x59, 0x49, 0x4D, 0x43,  // 'Z'
  0x00, 0x7F, 0x41, 0x41, 0x41,  // '['
  0x02, 0x04, 0x08, 0x10, 0x20,  // '\\'
  0x00, 0x41, 0x41, 0x41, 0x7F,  // ']'
  0x04, 0x02, 0x01, 0x02, 0x04,  // '^'
  0x40, 0x40, 0x40, 0x40, 0x40,  // '_'
  0x00, 0x03, 0x07, 0x08, 0x00,  // '`'
  0x20, 0x54, 0x54, 0x78, 0x40,  // 'a'
  0x7F, 0x28, 0x44, 0x44, 0x38,  // 'b'
  0x38, 0x44, 0x44, 0x44, 0x28,  // 'c'
  0x38, 0x44, 0x44, 0x28, 0x7F,  // 'd'
  0x38, 0x54, 0x54, 0x54, 0x18,  // 'e'
  0x00, 0x08, 0x7E, 0x09, 0x02,  // 'f'
  0x18, 0xA4, 0xA4, 0x9C, 0x78,  // 'g'
  0x7F, 0x08, 0x04, 0x04, 0x78,  // 'h'
  0x00, 0x44, 0x7D, 0x40, 0x00,  // 'i'
  0x20, 0x40, 0x40, 0x3D, 0x00,  // 'j'
  0x7F, 0x10, 0x28, 0x44, 0x00,  // 'k'
  0x00, 0x41, 0x7F, 0x40, 0x00,  // 'l'
  0x7C, 0x04, 0x78, 0x04, 0x78,  // 'm'
  0x7C, 0x08, 0x04, 0x04, 0x78,  // 'n'
  0x38, 0x44, 0x44, 0x44, 0x38,  // 'o'
  0xFC, 0x18, 0x24, 0x24, 0x18,  // 'p'
  0x18, 0x24, 0x24, 0x18, 0xFC,  // 'q'
  0x7C, 0x08, 0x04, 0x04, 0x08,  // 'r'
  0x48, 0x54, 0x54, 0x54, 0x24,  // 's'
  0x04, 0x04, 0x3F, 0x44, 0x24,  // 't'
  0x3C, 0x40, 0x40, 0x20, 0x7C,  // 'u'
  0x1C, 0x20, 0x40, 0x20, 0x1C,  // 'v'
  0x3C, 0x40, 0x30, 0x40, 0x3C,  // 'w'
  0x44, 0x28, 0x10, 0x28, 0x44,  // 'x'
  0x4C, 0x90, 0x90, 0x90, 0x7C,  // 'y'
  0x44, 0x64, 0x54, 0x4C, 0x44,  // 'z'
  0x00, 0x08, 0x36, 0x41, 0x00,  // '{'
  0x00, 0x00, 0x77, 0x00, 0x00,  // '|'
  0x00, 0x41, 0x36, 0x08, 0x00,  // '}'
  0x02, 0x01, 0x02, 0x04, 0x02,  // '~'
};
const uint8_t GLYPH_DEG[5] = { 0x06, 0x09, 0x09, 0x06, 0x00 };
const uint8_t* glyphOf(uint8_t c) {
  if (c == 0xDF) return GLYPH_DEG;
  if (c < 0x20 || c > 0x7E) c = '?';
  return &FONT5X7[(c - 0x20) * 5];
}
const uint8_t OLED_ADDR = 0x3C;
bool i2cProbe(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }
void oledCmd(uint8_t c) { Wire.beginTransmission(OLED_ADDR); Wire.write(0x00); Wire.write(c); Wire.endTransmission(); }
void oledData(const uint8_t *d, int n) {
  while (n > 0) { int k = n > 30 ? 30 : n;
    Wire.beginTransmission(OLED_ADDR); Wire.write(0x40); for (int i = 0; i < k; i++) Wire.write(d[i]); Wire.endTransmission();
    d += k; n -= k; }
}
void oledPos(uint8_t page, uint8_t col) { col += oledColOff; oledCmd(0xB0 | page); oledCmd(0x00 | (col & 0x0F)); oledCmd(0x10 | (col >> 4)); }
void oledClear() {
  uint8_t z[132]; memset(z, 0, sizeof(z));
  for (uint8_t p = 0; p < 8; p++) { oledCmd(0xB0 | p); oledCmd(0x00); oledCmd(0x10); oledData(z, 132); }
  for (auto &r : oledShadow) r = "";
}
bool oledInit() {
  if (!i2cProbe(OLED_ADDR)) return false;
  delay(50);
  const uint8_t seq[] = { 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x02,
                          0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6 };
  for (uint8_t c : seq) oledCmd(c);
  oledClear();
  oledCmd(0xAF);
  return true;
}
void oledRow(uint8_t row, const String &text) {
  if (!oledOk || row > 7) return;
  String t = text.substring(0, 21);
  while (t.length() < 21) t += ' ';
  if (t == oledShadow[row]) return;
  uint8_t buf[128]; memset(buf, 0, sizeof(buf));
  for (int i = 0; i < 21; i++) memcpy(&buf[i * 6], glyphOf((uint8_t)t[i]), 5);
  oledPos(row, 0); oledData(buf, 128);
  oledShadow[row] = t;
}
void oledBig(uint8_t row, const String &text) {           // chữ x2, chiếm 2 dòng
  String key = "#BIG#" + text;
  if (!oledOk || row > 6 || key == oledShadow[row]) return;
  uint8_t top[128], bot[128]; memset(top, 0, 128); memset(bot, 0, 128);
  int x = 0;
  for (unsigned i = 0; i < text.length() && x + 12 <= 128; i++) {
    const uint8_t *g = glyphOf((uint8_t)text[i]);
    for (int c = 0; c < 5; c++) {
      uint16_t v = 0;
      for (int b = 0; b < 8; b++) if (g[c] & (1 << b)) v |= 3u << (b * 2);
      for (int k = 0; k < 2; k++) { top[x] = v & 0xFF; bot[x] = v >> 8; x++; }
    }
    x += 2;
  }
  oledPos(row, 0); oledData(top, 128);
  oledPos(row + 1, 0); oledData(bot, 128);
  oledShadow[row] = key; oledShadow[row + 1] = key;
}

void renderOled() {
  if (!oledOk) return;
  char b[32];
  String flags = String(WiFi.status() == WL_CONNECTED ? "W+" : "W-") + (mqtt.connected() ? "M+" : "M-") + (timeValid ? "T+" : "T-");
  oledRow(0, fmtTime(false) + "  " + flags);
  if (sensorFault) oledBig(1, "LOI CB");
  else { snprintf(b, sizeof(b), "%.2f%cC", temp, (char)0xDF); oledBig(1, b); }
  snprintf(b, sizeof(b), "Dat %.1f%c  %s", cfg.sp, (char)0xDF, modeName(mode)); oledRow(3, b);
  snprintf(b, sizeof(b), "Lanh %3d%%  Nong %3d%%", (int)roundf(coolPct), (int)roundf(heatPct)); oledRow(4, b);
  snprintf(b, sizeof(b), "u%+5.0f I%+5.1f", pidU, pidI); oledRow(5, b);
  oledRow(6, "An " + String(feedToday) + "/" + String(MAX_FEEDS_PER_DAY) + "  Ke " + nextFeed());
  if (alarmCode != AL_NONE) oledRow(7, String("! ") + alarmName(alarmCode));
  else if (feedState != FEED_IDLE) oledRow(7, "Dang cho ca an...");
  else if (safeMode) oledRow(7, "SAFE MODE (offline)");
  else oledRow(7, WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "Chua co Wi-Fi");
}

// ============================================================================
// 10. MQTT: telemetry, bộ đệm, lệnh
// ============================================================================
void fillCommon(JsonDocument &d) {
  d["dev"] = deviceId;
  d["fw"] = FW_VERSION;
}

void sendTelemetry() {
  JsonDocument d;
  fillCommon(d);
  d["seq"] = ++seq;
  if (timeValid) d["ts"] = epochMs();
  if (sensorFault) d["temp"] = nullptr; else d["temp"] = roundf(temp * 100) / 100.0f;
  d["sp"]       = cfg.sp;
  d["mode"]     = modeName(mode);
  d["cool"]     = roundf(coolPct * 10) / 10.0f;
  d["heat"]     = roundf(heatPct * 10) / 10.0f;
  d["pid_u"]    = roundf(pidU * 10) / 10.0f;
  d["pid_p"]    = roundf(pidP * 10) / 10.0f;
  d["pid_i"]    = roundf(pidI * 10) / 10.0f;
  d["pid_d"]    = roundf(pidD * 10) / 10.0f;
  d["kp"] = cfg.kp; d["ki"] = cfg.ki; d["kd"] = cfg.kd; d["db"] = cfg.db;
  d["cool_max"] = cfg.coolMax; d["heat_max"] = cfg.heatMax;
  d["man_cool"] = manCool; d["man_heat"] = manHeat;
  if (mode == MODE_MANUAL) d["manual_left_s"] = (MANUAL_TIMEOUT_MS - (millis() - manualSinceMs)) / 1000;
  d["feed_today"] = feedToday;
  d["feed_count"] = feedTotal;
  d["feeding"]    = feedState != FEED_IDLE;
  d["next_feed"]  = nextFeed();
  d["schedule"]   = scheduleToString();
  d["time"]       = fmtTime(true);
  d["time_src"]   = srcName(timeSrc);
  d["alarm"]      = alarmName(alarmCode);
  d["ds_ok"]      = dsPresent && !sensorFault;
  d["oled"]       = oledOk;
  d["rssi"]       = WiFi.RSSI();
  d["uptime"]     = millis() / 1000;
  d["heap"]       = ESP.getFreeHeap();
  d["buf_count"]  = ringCount;
  d["safe_mode"]  = safeMode;
  String out; serializeJson(d, out);
  bool ok = mqtt.publish(tTelemetry.c_str(), out.c_str());
  static unsigned long lastLog = 0;
  if (!ok || millis() - lastLog >= 10000) {
    lastLog = millis();
    Serial.printf("[TEL] %s T=%s SP=%.1f %s lanh=%.0f%% nong=%.0f%% u=%.1f (%u byte)\n", ok ? "OK" : "LOI",
                  sensorFault ? "ERR" : String(temp, 2).c_str(), cfg.sp, modeName(mode), coolPct, heatPct, pidU, (unsigned)out.length());
  }
}

void bufferSample() {
  if (!timeValid || sensorFault) return;
  BufRec &r = ring[(ringHead + ringCount) % BUF_MAX];
  r.ts = (uint32_t)time(nullptr); r.t10 = (int16_t)roundf(temp * 10); r.sp10 = (int16_t)roundf(cfg.sp * 10);
  r.cool = (int8_t)roundf(coolPct); r.heat = (int8_t)roundf(heatPct); r.mode = (uint8_t)mode;
  if (ringCount < BUF_MAX) ringCount++; else ringHead = (ringHead + 1) % BUF_MAX;   // đầy: bỏ bản ghi cũ nhất
}

void flushBuffer() {
  int n = 0;
  while (ringCount > 0 && n < 10 && mqtt.connected()) {       // mỗi vòng loop gửi tối đa 10 bản ghi
    BufRec &r = ring[ringHead];
    JsonDocument d;
    fillCommon(d);
    d["ts"] = (uint64_t)r.ts * 1000ULL; d["temp"] = r.t10 / 10.0f; d["sp"] = r.sp10 / 10.0f;
    d["cool"] = r.cool; d["heat"] = r.heat; d["mode"] = modeName((Mode)r.mode);
    d["buffered"] = true;
    String out; serializeJson(d, out);
    if (!mqtt.publish(tTelemetry.c_str(), out.c_str())) break;
    ringHead = (ringHead + 1) % BUF_MAX; ringCount--; n++;
    if (ringCount == 0) publishEvent("recovered", "Da gui bu xong du lieu luc mat mang");
  }
}

void sendAck(const String &id, const String &dev, bool ok, const String &msg) {
  if (!mqtt.connected() || !id.length()) return;
  JsonDocument a;
  a["id"] = id; a["device"] = dev; a["ok"] = ok; a["msg"] = msg;
  String out; serializeJson(a, out);
  mqtt.publish(tAck.c_str(), out.c_str());
}

void setMode(Mode m, const char* why) {
  if (m == mode) return;
  mode = m;
  if (m == MODE_MANUAL) { manualSinceMs = millis(); manCool = coolPct; manHeat = heatPct; }
  if (m != MODE_MANUAL) { manCool = manHeat = 0; }
  resetPid();
  publishEvent("mode", String(modeName(m)) + " (" + why + ")");
}

bool setPidParams(JsonVariantConst d, String &msg) {
  PidCfg c = cfg;
  if (!d["kp"].isNull()) c.kp = d["kp"].as<float>();
  if (!d["ki"].isNull()) c.ki = d["ki"].as<float>();
  if (!d["kd"].isNull()) c.kd = d["kd"].as<float>();
  if (!d["db"].isNull()) c.db = d["db"].as<float>();
  if (!d["cool_max"].isNull()) c.coolMax = d["cool_max"].as<float>();
  if (!d["heat_max"].isNull()) c.heatMax = d["heat_max"].as<float>();
  if (c.kp < 0 || c.kp > 500 || c.ki < 0 || c.ki > 10 || c.kd < 0 || c.kd > 5000 || c.db < 0 || c.db > 2 ||
      c.coolMax < 0 || c.coolMax > 100 || c.heatMax < 0 || c.heatMax > 100) {
    msg = "Tham so ngoai gioi han (Kp 0-500, Ki 0-10, Kd 0-5000, DB 0-2, max 0-100)";
    return false;
  }
  cfg = c; savePid(); resetPid();
  char b[120];
  snprintf(b, sizeof(b), "PID Kp=%.2f Ki=%.4f Kd=%.1f DB=%.2f lanh<=%.0f%% nong<=%.0f%%", cfg.kp, cfg.ki, cfg.kd, cfg.db, cfg.coolMax, cfg.heatMax);
  msg = b;
  return true;
}

void handleCommand(JsonDocument &doc) {
  String id  = doc["id"] | "";
  String dev = doc["device"] | "";
  String act = doc["action"] | "";
  act.toUpperCase();
  bool ok = true; String msg = "OK";
  Serial.printf("[CMD] id=%s device=%s action=%s\n", id.c_str(), dev.c_str(), act.c_str());

  if (dev == "mode") {
    if (act == "AUTO") setMode(MODE_AUTO, "web");
    else if (act == "MANUAL") setMode(MODE_MANUAL, "web");
    else if (act == "OFF") setMode(MODE_OFF, "web");
    else { ok = false; msg = "action: AUTO | MANUAL | OFF"; }
    if (ok) msg = String("Che do ") + modeName(mode) + (mode == MODE_MANUAL ? " (tu ve AUTO sau 30 phut)" : "");
  } else if (dev == "setpoint") {
    float v = doc["value"] | NAN;
    if (isnan(v) || v < 20 || v > 32) { ok = false; msg = "Nhiet do dat phai 20-32 C"; }
    else { cfg.sp = v; prefs.putFloat("sp", v); msg = "Nhiet do dat " + String(v, 1) + " C"; publishEvent("setpoint", msg); }
  } else if (dev == "pid") {
    ok = setPidParams(doc.as<JsonVariantConst>(), msg);
    if (ok) publishEvent("pid", msg);
  } else if (dev == "pid_reset") {
    resetPid(); msg = "Da xoa tich phan PID";
  } else if (dev == "cool" || dev == "heat") {
    float v = doc["value"] | NAN;
    if (isnan(v) || v < 0 || v > 100) { ok = false; msg = "Gia tri 0-100 %"; }
    else {
      setMode(MODE_MANUAL, "dat tay");
      manualSinceMs = millis();
      if (dev == "cool") { manCool = v; if (v > 0) manHeat = 0; }
      else               { manHeat = v; if (v > 0) manCool = 0; }
      msg = String(dev == "cool" ? "So lanh " : "Loi suoi ") + String(v, 0) + "% (MANUAL 30 phut)";
      if (sensorFault) { msg += " - CANH BAO: mat cam bien nen dang bi khoa"; }
    }
  } else if (dev == "feed") {
    if (lastFeedEpoch && timeValid && (uint32_t)time(nullptr) - lastFeedEpoch < 60) { ok = false; msg = "Vua cho an < 60 s"; }
    else if (!startFeeding("WEB")) { ok = false; msg = feedState != FEED_IDLE ? "Dang cho an" : "Vuot gioi han so lan/ngay"; }
    else msg = "Dang cho ca an";
  } else if (dev == "schedule") {
    String sl = doc["slots"] | "";
    if (parseSchedule(sl)) { prefs.putString("sched", scheduleToString()); msg = "Lich: " + (slotCount ? scheduleToString() : String("TAT")); publishEvent("schedule", msg); }
    else { ok = false; msg = "Lich sai, vd 07:00,17:30 (toi da 4 moc)"; }
  } else if (dev == "time") {
    uint32_t e = doc["epoch"] | 0UL;
    if (e > 1704067200UL) {
      struct timeval tv = { (time_t)e, 0 }; settimeofday(&tv, nullptr);
      timeValid = true; if (!everNtp) timeSrc = SRC_WEB;
      msg = "Da dat gio " + fmtTime(true); publishEvent("time_sync", "WEB " + fmtTime(true));
    } else { ok = false; msg = "Epoch khong hop le"; }
  } else if (dev == "oled") {
    if (act == "SH1106" || act == "SSD1306") {
      oledColOff = act == "SH1106" ? 2 : 0; prefs.putUChar("oledOff", oledColOff);
      if (oledOk) oledClear();
      msg = "OLED chip " + act;
    } else if (act == "RESET") { oledOk = oledInit(); ok = oledOk; msg = ok ? "OLED khoi dong lai OK" : "Khong thay OLED 0x3C"; }
    else { ok = false; msg = "action: SSD1306 | SH1106 | RESET"; }
  } else if (dev == "ping") {
    msg = "pong";
  } else {
    ok = false; msg = "Lenh khong ho tro: " + dev;
  }
  sendAck(id, dev, ok, msg);
  controlStep(millis());          // áp dụng ngay, không chờ chu kỳ
  sendTelemetry();
}

void mqttCallback(char*, byte* payload, unsigned int len) {
  JsonDocument doc;
  // ép kiểu const -> ArduinoJson COPY chuỗi (PubSubClient dùng lại bộ đệm khi publish trong callback)
  if (deserializeJson(doc, (const char*)payload, len)) { Serial.println("[CMD] JSON loi"); return; }
  handleCommand(doc);
}

void handleConnectivity(unsigned long now) {
  static bool wifiUp = false, mqttUp = false;
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiUp) { wifiUp = false; Serial.println("[WIFI] Mat Wi-Fi"); }
    if (now - lastWifiTryMs >= WIFI_RETRY_MS) {
      lastWifiTryMs = now;
      Serial.printf("[WIFI] Ket noi \"%s\" (status=%d)\n", WIFI_SSID, (int)WiFi.status());
      WiFi.disconnect(); delay(50); WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
  } else {
    if (!wifiUp) { wifiUp = true; Serial.printf("[WIFI] OK IP %s RSSI %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI()); }
    if (!mqtt.connected()) {
      if (mqttUp) { mqttUp = false; Serial.printf("[MQTT] Mat broker (state=%d)\n", mqtt.state()); }
      if (now - lastMqttTryMs >= MQTT_RETRY_MS) {
        lastMqttTryMs = now;
        const char* u = strlen(MQTT_USER) ? MQTT_USER : nullptr;
        const char* p = strlen(MQTT_PASS) ? MQTT_PASS : nullptr;
        if (mqtt.connect(deviceId.c_str(), u, p, tStatus.c_str(), 1, true, "offline")) {
          mqttUp = true;
          mqtt.publish(tStatus.c_str(), "online", true);
          mqtt.subscribe(tCommand.c_str(), 1);
          Serial.printf("[MQTT] OK %s topic %s/*\n", MQTT_HOST, TOPIC_BASE);
          publishEvent("boot", String("FW ") + FW_VERSION + " online");
        } else Serial.printf("[MQTT] That bai state=%d\n", mqtt.state());
      }
    } else mqtt.loop();
  }

  if (mqtt.connected()) {
    mqttLostSinceMs = 0;
    if (safeMode) { safeMode = false; publishEvent("recovered", "Het SAFE MODE"); }
  } else {
    if (!mqttLostSinceMs) mqttLostSinceMs = now ? now : 1;
    if (!safeMode && now - mqttLostSinceMs >= SAFE_MODE_MS) { safeMode = true; Serial.println("[SAFE] Mat broker > 30 s -> dieu khien cuc bo, dem du lieu"); }
  }
}

// ============================================================================
// 11. NÚT BOOT, SERIAL
// ============================================================================
void handleButton(unsigned long now) {
  static bool prev = HIGH; static unsigned long tDown = 0;
  bool v = digitalRead(PIN_BTN);
  if (prev == HIGH && v == LOW) tDown = now;
  if (prev == LOW && v == HIGH && now - tDown >= 50) {
    Serial.println("[NUT] BOOT -> cho an");
    if (!startFeeding("NUT")) Serial.println("[NUT] Khong cho an duoc (dang an / vuot gioi han)");
  }
  prev = v;
}

void printStatus() {
  Serial.println("---------------- TRANG THAI ----------------");
  Serial.printf(" Nhiet do : %s  (dat %.2f)  che do %s\n", sensorFault ? "LOI" : String(temp, 2).c_str(), cfg.sp, modeName(mode));
  Serial.printf(" Lanh %.1f%%  Nong %.1f%%  | u=%.1f P=%.1f I=%.1f D=%.1f\n", coolPct, heatPct, pidU, pidP, pidI, pidD);
  Serial.printf(" Kp=%.2f Ki=%.4f Kd=%.1f DB=%.2f lanh<=%.0f nong<=%.0f\n", cfg.kp, cfg.ki, cfg.kd, cfg.db, cfg.coolMax, cfg.heatMax);
  Serial.printf(" Gio %s (%s) | lich %s | an %u hom nay, ke %s\n", fmtTime(true).c_str(), srcName(timeSrc),
                scheduleToString().c_str(), feedToday, nextFeed().c_str());
  Serial.printf(" DS18B20 %s (ok %lu / loi %lu) | OLED %s | canh bao %s\n", dsPresent ? "CO" : "KHONG",
                (unsigned long)dsReadOk, (unsigned long)dsReadFail, oledOk ? "CO" : "KHONG", alarmName(alarmCode));
  Serial.println("--------------------------------------------");
}

void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c != '\n' && c != '\r') { if (line.length() < 60) line += c; continue; }
    line.trim();
    if (!line.length()) continue;
    // Chuyển lệnh Serial thành JSON giống lệnh web, dùng chung handleCommand()
    String cmd = line; line = "";
    int sp = cmd.indexOf(' ');
    String a = sp < 0 ? cmd : cmd.substring(0, sp), b = sp < 0 ? "" : cmd.substring(sp + 1);
    a.toLowerCase(); b.trim();
    JsonDocument d;
    if (a == "status" || a == "s") { printStatus(); continue; }
    else if (a == "help" || a == "?") {
      Serial.println("Lenh: status | auto | manual | off | sp 26.5 | cool 50 | heat 30 | kp 25 | ki 0.02 | kd 0 | db 0.1\n"
                     "      feed | sched 07:00,17:30 | pidreset | ds | oled ssd1306|sh1106|reset");
      continue;
    }
    else if (a == "auto" || a == "manual" || a == "off") { d["device"] = "mode"; d["action"] = a; }
    else if (a == "sp") { d["device"] = "setpoint"; d["value"] = b.toFloat(); }
    else if (a == "cool" || a == "heat") { d["device"] = a; d["value"] = b.toFloat(); }
    else if (a == "kp" || a == "ki" || a == "kd" || a == "db") { d["device"] = "pid"; d[a] = b.toFloat(); }
    else if (a == "feed") d["device"] = "feed";
    else if (a == "sched") { d["device"] = "schedule"; d["slots"] = b; }
    else if (a == "pidreset") d["device"] = "pid_reset";
    else if (a == "oled") { d["device"] = "oled"; d["action"] = b; }
    else if (a == "ds") { dsScan(true); continue; }
    else { Serial.println("Lenh khong ro, go 'help'"); continue; }
    handleCommand(d);
  }
}

// ============================================================================
// 12. SETUP / LOOP
// ============================================================================
void setup() {
  // MOSFET TẮT ngay lập tức, trước mọi thứ
  pinMode(PIN_COOL, OUTPUT); digitalWrite(PIN_COOL, LOW);
  pinMode(PIN_HEAT, OUTPUT); digitalWrite(PIN_HEAT, LOW);
  pinMode(PIN_BTN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== SMART AQUARIUM PID v%s ===\n", FW_VERSION);

  pwmAttach(PIN_COOL, CH_COOL, PWM_FREQ_HZ, PWM_RES);
  pwmAttach(PIN_HEAT, CH_HEAT, PWM_FREQ_HZ, PWM_RES);
  allOff();
  pwmAttach(PIN_SERVO, CH_SERVO, 50, 14);
  servoStop();

  loadSettings();
  Serial.printf("[CFG] SP=%.1f Kp=%.2f Ki=%.4f Kd=%.1f lich=%s\n", cfg.sp, cfg.kp, cfg.ki, cfg.kd, scheduleToString().c_str());

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  oledOk = oledInit();
  Serial.printf("[OLED] 0x3C %s\n", oledOk ? "OK" : "KHONG THAY (SDA=GPIO8, SCL=GPIO9)");
  oledRow(2, "  SMART AQUARIUM");
  oledRow(4, "  PID v" + String(FW_VERSION));

  dsScan(true);

  uint8_t mac[6]; WiFi.macAddress(mac);
  char idb[20]; snprintf(idb, sizeof(idb), "aq-%02X%02X%02X", mac[3], mac[4], mac[5]);
  deviceId = idb;
  String base = TOPIC_BASE;
  tTelemetry = base + "/telemetry"; tCommand = base + "/command"; tStatus = base + "/status";
  tEvent = base + "/event"; tAck = base + "/ack";

  setenv("TZ", TZ_INFO, 1); tzset();
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiTryMs = millis();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1536);
  mqtt.setKeepAlive(30);
  mqtt.setCallback(mqttCallback);
  Serial.println("Go 'help' de xem lenh Serial.");
}

void loop() {
  unsigned long now = millis();
  static unsigned long lastCtrl = 0, lastTel = 0, lastBuf = 0, lastOled = 0, lastSched = 0;

  handleSerial();
  handleButton(now);
  updateSensor(now);
  updateFeeding(now);

  if (ntpSyncFlag) {
    ntpSyncFlag = false;
    bool first = !everNtp;
    everNtp = true; timeValid = true; timeSrc = SRC_NTP;
    if (first) publishEvent("time_sync", "NTP " + fmtTime(true));
  }

  if (now - lastCtrl >= CONTROL_MS) { lastCtrl = now; controlStep(now); }
  if (now - lastSched >= 1000) { lastSched = now; checkSchedule(); }

  handleConnectivity(now);
  if (mqtt.connected()) {
    if (ringCount) flushBuffer();
    if (now - lastTel >= TELEMETRY_MS) { lastTel = now; sendTelemetry(); }
  } else if (now - lastBuf >= BUFFER_MS) { lastBuf = now; bufferSample(); }

  if (now - lastOled >= OLED_MS) { lastOled = now; renderOled(); }
}
