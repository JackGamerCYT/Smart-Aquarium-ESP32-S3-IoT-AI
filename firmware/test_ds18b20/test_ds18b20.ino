/*
 * TEST CẢM BIẾN DS18B20 – ESP32-S3 (chẩn đoán "Mất cảm biến" / ERR / -127°C)
 * ---------------------------------------------------------------------------
 * Thư viện: OneWire, DallasTemperature (đã cài cho firmware chính).
 * Nạp, mở Serial Monitor 115200.
 *
 * Nối dây DS18B20 (loại dây chống nước 3 sợi):
 *   ĐỎ   (VCC)  -> 3V3
 *   ĐEN  (GND)  -> GND
 *   VÀNG (DATA) -> GPIO4
 *   Trở 4.7kΩ nối giữa VÀNG và ĐỎ (DATA lên 3V3)  <-- RẤT NÊN CÓ
 *
 * Chương trình kiểm tra:
 *   B1. Dây DATA có trở kéo lên ngoài không (4.7k)?
 *   B2. Có cảm biến trả lời trên GPIO4 không (xung "presence") – thử cả có/không trở kéo nội
 *   B3. Dò các chân khác: cảm biến có bị cắm nhầm chân không?
 *   B4. Đọc 20 lần, đếm số lần lỗi -> biết dây chập chờn hay ổn định
 */
#include <OneWire.h>
#include <DallasTemperature.h>

const int DS_PIN = 4;
// Chân an toàn để dò (không đụng relay 18/19, I2C 8/9, còi 12, servo 13, quạt 15, UART 43/44)
const int CAND[] = { 1, 2, 4, 5, 6, 7, 10, 11, 14, 16, 17, 21, 38, 39, 40, 41, 42, 47, 48 };

bool hasExternalPullup(int pin) {
  pinMode(pin, INPUT_PULLDOWN);
  delay(5);
  int hi = 0;
  for (int i = 0; i < 10; i++) { hi += digitalRead(pin); delayMicroseconds(200); }
  pinMode(pin, INPUT);
  return hi >= 9;
}

bool presenceOn(int pin, bool internalPullup) {
  OneWire ow(pin);
  if (internalPullup) pinMode(pin, INPUT_PULLUP);
  delay(2);
  bool ok = false;
  for (int i = 0; i < 3 && !ok; i++) ok = ow.reset();
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n================ TEST DS18B20 ================");

  // ---------- B1 ----------
  bool ext = hasExternalPullup(DS_PIN);
  Serial.printf("\n[B1] Tro keo len ngoai tren GPIO%d: %s\n", DS_PIN,
                ext ? "CO (da gan 4.7k hoac tuong duong) -> tot" : "KHONG CO -> dang dua vao tro keo NOI ~45k (yeu, de mat cam bien)");

  // ---------- B2 ----------
  bool pNo  = presenceOn(DS_PIN, false);
  bool pInt = presenceOn(DS_PIN, true);
  Serial.printf("[B2] Cam bien tra loi tren GPIO%d: khong keo noi = %s | co keo noi = %s\n",
                DS_PIN, pNo ? "CO" : "KHONG", pInt ? "CO" : "KHONG");

  // ---------- B3 ----------
  int foundOther = -1;
  if (!pNo && !pInt) {
    Serial.println("[B3] Do cac chan khac (co keo noi)...");
    for (int p : CAND) {
      if (p == DS_PIN) continue;
      if (presenceOn(p, true)) { Serial.printf("   ✔ Thay DS18B20 o GPIO%d !\n", p); foundOther = p; }
    }
    if (foundOther < 0) Serial.println("   Khong thay DS18B20 o chan nao.");
  }

  // ---------- B4 ----------
  int errs = 0, ok = 0, n = 0;
  if (pNo || pInt) {
    pinMode(DS_PIN, INPUT_PULLUP);
    OneWire ow(DS_PIN);
    DallasTemperature ds(&ow);
    ds.begin();
    pinMode(DS_PIN, INPUT_PULLUP);                    // bật lại kéo nội sau begin()
    n = ds.getDeviceCount();
    DeviceAddress a;
    Serial.printf("[B4] So cam bien: %d", n);
    if (n && ds.getAddress(a, 0)) {
      Serial.print(" | dia chi ROM: ");
      for (int i = 0; i < 8; i++) Serial.printf("%02X", a[i]);
      Serial.printf(" | che do: %s", ds.isParasitePowerMode() ? "KY SINH (day DO chua noi 3V3!)" : "cap nguon rieng (dung)");
    }
    Serial.println();
    ds.setResolution(11);
    for (int i = 0; i < 20; i++) {
      ds.requestTemperatures();
      float t = ds.getTempCByIndex(0);
      bool bad = (t == DEVICE_DISCONNECTED_C) || (t == 85.0f);
      if (bad) errs++; else ok++;
      Serial.printf("   lan %2d: %s\n", i + 1, bad ? (t == 85.0f ? "85.0 (loi: nguon yeu / doc qua som)" : "-127 (mat tin hieu)") : String(t, 2).c_str());
      delay(300);
    }
  }

  // ---------- KẾT LUẬN ----------
  Serial.println("\n================ KET LUAN ================");
  if ((pNo || pInt) && errs == 0) {
    Serial.printf("✔ DS18B20 hoat dong tot (%d/20 lan doc OK).", ok);
    Serial.println(ext ? "" : " Van NEN gan tro 4.7k de chay lau dai khong mat.");
    Serial.println("  Neu firmware chinh van bao ERR -> nap lai firmware v3.2.0 moi nhat.");
  } else if ((pNo || pInt) && errs > 0) {
    Serial.printf("⚠ Cam bien CHAP CHON: %d/20 lan loi.\n", errs);
    Serial.println("  -> Nguyen nhan so 1: THIEU TRO 4.7k. Gan tro 4.7k giua day VANG (DATA) va DO (3V3).");
    Serial.println("  -> Khong co 4.7k: dung tam 2 tro 10k mac song song (=5k), hoac 1 tro 2.2k–10k bat ky.");
    Serial.println("  -> Kiem tra moi han / day cam breadboard long.");
  } else if (foundOther >= 0) {
    Serial.printf("✘ Ban cam day VANG (DATA) vao GPIO%d, khong phai GPIO4. Cam lai vao GPIO4.\n", foundOther);
  } else {
    Serial.println("✘ Khong co cam bien nao tra loi:");
    Serial.println("  1. Do: VCC(3V3) - GND - DATA dung mau? (DO=3V3, DEN=GND, VANG=DATA)");
    Serial.println("     Mot so cam bien mau khac: DO=VCC, VANG/TRANG=DATA, DEN/XANH=GND.");
    Serial.println("  2. Cam nguoc VCC/GND lau co the lam CHAY cam bien (cam bien nong khi cham tay).");
    Serial.println("  3. Gan tro 4.7k DATA-3V3 roi thu lai – keo noi thuong KHONG du voi day dai 1 m.");
  }
  Serial.println("\n(Nhan RESET de do lai)");
}

void loop() {}
