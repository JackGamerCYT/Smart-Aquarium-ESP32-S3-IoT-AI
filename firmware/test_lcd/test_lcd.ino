/*
 * TEST LCD I2C (1602 / 2004 + mạch PCF8574) – ESP32-S3
 * ---------------------------------------------------------------------------
 * KHÔNG cần thư viện. Nạp, mở Serial Monitor 115200.
 * Nối dây:  LCD GND -> GND | VCC -> 5V | SDA -> GPIO8 | SCL -> GPIO9
 *
 * Chương trình lặp lại mỗi 6 s:
 *   1. Quét bus I2C, tìm địa chỉ LCD (thường 0x27 hoặc 0x3F)
 *   2. Tô kín mọi ô (thấy hàng ô đen -> LCD sống, dây đúng)
 *   3. Hiện chữ ở cả 4 dòng (LCD 1602 chỉ hiện 2 dòng đầu)
 *
 *  Đèn nền sáng + chỉ thấy ô đen/không thấy gì -> VẶN BIẾN TRỞ xanh sau lưng LCD.
 *  Đèn nền không sáng -> kiểm tra VCC 5V / GND, hoặc jumper "LED" sau mạch I2C.
 */
#include <Wire.h>

#define SDA_PIN 8
#define SCL_PIN 9

uint8_t lcdAddr = 0, lcdRows = 4, lcdCols = 20;       // thử như 2004; màn 1602 vẫn hiện đúng 16 ký tự đầu 2 dòng
bool lcdPresent = false, lcdBacklight = true;
String lcdShadow[4];

bool i2cProbe(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }
bool isLcdAddr(uint8_t a) { return (a >= 0x20 && a <= 0x27) || (a >= 0x38 && a <= 0x3F); }

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

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(SDA_PIN, SCL_PIN, 100000);
  Serial.println("\n=== TEST LCD I2C ===");
}

void loop() {
  Serial.print("[I2C] Tim thay:");
  int n = 0;
  for (uint8_t a = 1; a < 127; a++) if (i2cProbe(a)) { Serial.printf(" 0x%02X", a); n++; }
  Serial.println(n ? "" : " KHONG CO -> kiem tra VCC/GND, SDA=GPIO8, SCL=GPIO9 (thu dao 2 day)");

  if (!lcdInit()) {
    Serial.println("[LCD] Khong thay PCF8574 (0x20-0x27 / 0x38-0x3F) -> loi day/nguon, hoac LCD chua han mach I2C");
    delay(3000);
    return;
  }
  Serial.printf("[LCD] Tim thay LCD o 0x%02X -> dang to kin o...\n", lcdAddr);
  String full; for (int i = 0; i < lcdCols; i++) full += (char)0xFF;
  for (uint8_t r = 0; r < lcdRows; r++) lcdRow(r, full);
  delay(2000);

  char b[24]; snprintf(b, sizeof(b), "LCD OK dia chi 0x%02X", lcdAddr);
  lcdRow(0, b);
  snprintf(b, sizeof(b), "Nhiet do 26.5%cC", (char)0xDF);
  lcdRow(1, b);
  lcdRow(2, "Dong 3 (LCD 2004)");
  lcdRow(3, "Dong 4 (LCD 2004)");
  Serial.println("  -> Man co chu ro? Neu chi thay o den / khong thay gi: vat bien tro tuong phan.");
  Serial.println("  -> Firmware chinh: go 'lcd 1602' hoac 'lcd 2004' cho dung loai man.\n");
  delay(4000);
}
