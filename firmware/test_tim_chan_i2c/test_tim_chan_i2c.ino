/*
 * TÌM CHÂN I2C THỰC TẾ CỦA LCD / RTC – ESP32-S3
 * ---------------------------------------------------------------------------
 * Không cần thư viện ngoài. Nạp, mở Serial Monitor 115200, đợi ~10 giây.
 *
 * Chương trình làm 3 việc:
 *   B1. Đo ĐIỆN ÁP dây SDA (GPIO8) và SCL (GPIO9): có trở kéo của module không?
 *       -> biết ngay module có được cấp nguồn và dây có cắm vào đúng chân không.
 *   B2. Thử đúng cặp chân trong firmware (SDA=8, SCL=9) và cặp ĐẢO (SDA=9, SCL=8).
 *   B3. Dò MỌI cặp chân an toàn để tìm xem LCD (0x27/0x3F) và RTC (0x68)
 *       đang thật sự nằm ở chân nào.
 *
 * Không đụng chân relay (18, 19), còi (12), servo (13), quạt (15), DS18B20 (4)
 * -> relay không bị chập chờn khi dò.
 */
#include <Wire.h>

const int FW_SDA = 8, FW_SCL = 9;                 // chân đang dùng trong firmware chính
const int CAND[] = { 1, 2, 5, 6, 7, 8, 9, 10, 11, 14, 16, 17, 21, 38, 39, 40, 41, 42, 47, 48 };
const int NCAND = sizeof(CAND) / sizeof(CAND[0]);
const uint8_t ADDRS[] = { 0x27, 0x3F, 0x20, 0x38, 0x68, 0x57 };

const char* addrName(uint8_t a) {
  switch (a) { case 0x27: case 0x3F: case 0x20: case 0x38: return "LCD (PCF8574)"; case 0x68: return "RTC DS3231"; case 0x57: return "EEPROM (tren module RTC)"; }
  return "?";
}

bool probe(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }

/** Kéo xuống yếu bằng pull-down nội ~45k: nếu vẫn đọc HIGH => có trở kéo lên của module => module có nguồn & dây nối vào chân này. */
bool hasExternalPullup(int pin) {
  pinMode(pin, INPUT_PULLDOWN);
  delay(5);
  int hi = 0;
  for (int i = 0; i < 10; i++) { hi += digitalRead(pin); delayMicroseconds(200); }
  pinMode(pin, INPUT);
  return hi >= 9;
}

int scanPair(int sda, int scl, bool verbose) {
  Wire.begin(sda, scl, 100000);
  Wire.setTimeOut(10);
  int found = 0;
  for (uint8_t a : ADDRS) {
    if (probe(a)) {
      found++;
      Serial.printf("   ✔ SDA=GPIO%-2d SCL=GPIO%-2d -> 0x%02X  %s\n", sda, scl, a, addrName(a));
    }
  }
  if (!found && verbose) Serial.printf("   ✘ SDA=GPIO%d SCL=GPIO%d -> khong co thiet bi nao\n", sda, scl);
  Wire.end();
  return found;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n================ TIM CHAN I2C ================");

  // ---------- B1: đo điện dây ----------
  Serial.println("\n[B1] Kiem tra tro keo len (module co nguon + day cam dung chan?)");
  bool pu8 = hasExternalPullup(FW_SDA), pu9 = hasExternalPullup(FW_SCL);
  Serial.printf("   GPIO%d (SDA): %s\n", FW_SDA, pu8 ? "CO dien tu module -> day da cam" : "KHONG co -> day chua cam / module chua cap nguon");
  Serial.printf("   GPIO%d (SCL): %s\n", FW_SCL, pu9 ? "CO dien tu module -> day da cam" : "KHONG co -> day chua cam / module chua cap nguon");
  if (!pu8 && !pu9)
    Serial.println("   => Ca 2 day deu 'chet': kiem tra VCC(5V)/GND cua LCD truoc – doc chu in tren mach I2C: GND VCC SDA SCL");

  // ---------- B2: đúng chân firmware + chân đảo ----------
  Serial.println("\n[B2] Thu dung chan firmware (SDA=8, SCL=9) va chan DAO (SDA=9, SCL=8)");
  int okNormal  = scanPair(FW_SDA, FW_SCL, true);
  int okSwapped = scanPair(FW_SCL, FW_SDA, true);

  // ---------- B3: dò mọi cặp ----------
  Serial.println("\n[B3] Do tat ca cap chan an toan (mat khoang 10 giay)...");
  int total = 0;
  for (int i = 0; i < NCAND; i++)
    for (int j = 0; j < NCAND; j++)
      if (i != j) total += scanPair(CAND[i], CAND[j], false);
  if (!total) Serial.println("   Khong tim thay LCD/RTC o bat ky cap chan nao.");

  // ---------- KẾT LUẬN ----------
  Serial.println("\n================ KET LUAN ================");
  if (okNormal) {
    Serial.println("✔ Day DUNG chan (SDA=8, SCL=9). Neu LCD sang den ma khong co chu:");
    Serial.println("  VAT BIEN TRO xanh sau lung LCD (tuong phan). Nap test_lcd.ino de kiem tra.");
  } else if (okSwapped) {
    Serial.println("✘ Ban cam NGUOC SDA va SCL. Doi cho 2 day: day dang o GPIO8 <-> day dang o GPIO9.");
  } else if (total) {
    Serial.println("✘ LCD/RTC dang cam o chan KHAC (xem dong ✔ o [B3]).");
    Serial.println("  Cach 1: rut day, cam lai SDA->GPIO8, SCL->GPIO9.");
    Serial.println("  Cach 2: sua PIN_I2C_SDA / PIN_I2C_SCL trong firmware cho khop chan ✔ o tren.");
  } else {
    Serial.println("✘ Khong thay module o dau ca -> loi NGUON / DAY / module hong:");
    Serial.println("  1. LCD phai co MACH I2C (PCF8574, 4 chan GND VCC SDA SCL) han sau lung. LCD tran 16 chan -> code nay khong chay.");
    Serial.println("  2. Do dong ho: VCC-GND cua LCD phai ~5V.");
    Serial.println("  3. Kiem tra thu tu chan in tren mach I2C (GND VCC SDA SCL).");
    Serial.println("  4. Thu LCD rieng, rut RTC ra (RTC loi co the giu bus).");
  }
  Serial.println("\n(Nhan nut RESET de do lai)");
}

void loop() {}
