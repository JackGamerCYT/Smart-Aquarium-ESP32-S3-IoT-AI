/*
 * TEST DS18B20 – cắm ở cổng "SERVO 2" (theo sơ đồ):
 *   chân 3 (DATA) -> GPIO6 · chân 2 (VCC) -> 3V3 · chân 1 -> GND
 * KHÔNG cần điện trở kéo lên, KHÔNG cần thư viện (driver 1-Wire tự viết, giống firmware chính).
 * Serial Monitor 115200: mỗi giây in nhiệt độ, đếm số lần đọc OK / lỗi.
 */
#include "driver/gpio.h"

#define PIN_DS18B20 6
bool dsPresent = false;
uint32_t dsReadOk = 0, dsReadFail = 0;

static portMUX_TYPE owMux = portMUX_INITIALIZER_UNLOCKED;
const gpio_num_t OW_PIN = (gpio_num_t)PIN_DS18B20;
const uint8_t OW_SAMPLE_US[] = { 12, 9, 15, 18, 22, 26, 30, 7 };
uint8_t owSampleIdx = 0; bool owSampleLocked = false;

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


void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== TEST DS18B20 tren GPIO6 (cong SERVO 2) ===");
  dsScan(true);
}

void loop() {
  if (!dsPresent) { delay(2000); dsScan(true); return; }
  if (!dsStartConversion()) { Serial.println("Mat cam bien (khong co xung presence)"); dsPresent = false; return; }
  delay(800);                                   // 12 bit: 750 ms
  float t = dsReadTemp();
  if (t == DS_ERR || t == 85.0f) Serial.printf("LOI doc (ok %lu / loi %lu)\n", (unsigned long)dsReadOk, (unsigned long)dsReadFail);
  else Serial.printf("Nhiet do: %.4f C   (ok %lu / loi %lu, lay mau %u us)\n", t, (unsigned long)dsReadOk, (unsigned long)dsReadFail, OW_SAMPLE_US[owSampleIdx]);
  delay(200);
}
