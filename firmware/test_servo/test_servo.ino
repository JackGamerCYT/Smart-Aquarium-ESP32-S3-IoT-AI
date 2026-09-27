/*
 * TEST SERVO CHO ĂN – cổng "SERVO 1" (theo sơ đồ):
 *   chân 3 (tín hiệu) -> GPIO7 · chân 2 (VCC) -> 5V · chân 1 -> GND
 *   (Cổng SERVO 2 / GPIO6 giờ dùng cho DS18B20 – KHÔNG test servo ở đó.)
 *   !! Sơ đồ đang nối VCC servo vào 3V3 của DevKit -> phải chuyển sang 5V (LM2596).
 * Lặp mãi: 0° (3s) -> 90° (3s) -> 180° (3s) -> quét chậm về 0° -> nghỉ 3s.
 * Không cần thư viện. Chạy được ESP32 core 2.x và 3.x.
 */
const int SERVO_PIN = 7;
const uint8_t SERVO_CH = 2;             // kênh LEDC (chỉ dùng cho core 2.x)
const uint16_t PULSE_MIN = 500, PULSE_MAX = 2400;
const unsigned long GIU_MS = 3000;

void servoWriteDeg(int deg) {
  deg = constrain(deg, 0, 180);
  uint32_t us = map(deg, 0, 180, PULSE_MIN, PULSE_MAX);
  uint32_t duty = us * ((1UL << 14) - 1) / 20000UL;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(SERVO_PIN, duty);
#else
  ledcWrite(SERVO_CH, duty);
#endif
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== TEST SERVO CHO AN (GPIO7) ===");
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(SERVO_PIN, 50, 14);
#else
  ledcSetup(SERVO_CH, 50, 14);
  ledcAttachPin(SERVO_PIN, SERVO_CH);
#endif
  servoWriteDeg(0);
  delay(1000);
}

void loop() {
  const int goc[] = { 0, 90, 180 };
  for (int g : goc) { servoWriteDeg(g); Serial.printf("Servo: %d do\n", g); delay(GIU_MS); }
  Serial.println("Servo: quet cham 180 -> 0");
  for (int g = 180; g >= 0; g -= 2) { servoWriteDeg(g); delay(20); }
  delay(GIU_MS);
}
