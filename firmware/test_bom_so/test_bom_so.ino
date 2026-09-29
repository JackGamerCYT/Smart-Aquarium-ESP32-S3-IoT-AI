/*
 * TEST BƠM + SÒ LẠNH (2 MOSFET IRLZ44N) – ESP32-S3
 *   Q1 (kênh C1) = GPIO4 -> ĐỘNG CƠ BƠM NƯỚC
 *   Q2 (kênh C2) = GPIO5 -> SÒ LẠNH Peltier 12V
 *
 * DÙNG ĐỂ KIỂM TRA ĐÚNG CHÂN: gõ "1" -> phải là BƠM chạy. Nếu SÒ chạy thì
 * trong firmware chính đổi PIN_PUMP = 5, PIN_COOL = 4.
 *
 * Mặc định: TỰ ĐỘNG – Q1 bật 3s / tắt 3s, rồi Q2 bật 3s / tắt 3s, lặp mãi.
 * Gõ lệnh ở Serial Monitor (115200, "Newline") để GIỮ trạng thái mà đo:
 *   1  -> giữ Q1 BẬT      2  -> giữ Q2 BẬT
 *   0  -> tắt hết          a  -> quay lại chế độ tự động
 *
 * Cách đo (que đen ở GND):
 *   Chân GPIO4/5      : BẬT ~3.3V, TẮT 0V
 *   Chân G của MOSFET : BẬT ~3.3V, TẮT 0V
 *   2 đầu tải         : BẬT ~ điện áp nguồn của tải (12V với sò), TẮT 0V
 */

const int PIN_Q1 = 4;
const int PIN_Q2 = 5;
const unsigned long CHU_KY_MS = 3000;

bool autoMode = true;
int  buoc = 0;                 // 0: Q1 bật, 1: Q1 tắt, 2: Q2 bật, 3: Q2 tắt
unsigned long tBuoc = 0;

void datMosfet(bool q1, bool q2) {
  digitalWrite(PIN_Q1, q1 ? HIGH : LOW);
  digitalWrite(PIN_Q2, q2 ? HIGH : LOW);
  Serial.printf("Q1 BOM (GPIO%d) = %s | Q2 SO (GPIO%d) = %s\n",
                PIN_Q1, q1 ? "BAT" : "tat", PIN_Q2, q2 ? "BAT" : "tat");
}

void chayBuoc() {
  switch (buoc) {
    case 0: Serial.print("[AUTO] "); datMosfet(true,  false); break;
    case 1: Serial.print("[AUTO] "); datMosfet(false, false); break;
    case 2: Serial.print("[AUTO] "); datMosfet(false, true);  break;
    case 3: Serial.print("[AUTO] "); datMosfet(false, false); break;
  }
  tBuoc = millis();
}

void docLenh() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r' || c == ' ') continue;
    switch (c) {
      case '1': autoMode = false; Serial.print("[GIU] "); datMosfet(true,  false); break;
      case '2': autoMode = false; Serial.print("[GIU] "); datMosfet(false, true);  break;
      case '0': autoMode = false; Serial.print("[GIU] "); datMosfet(false, false); break;
      case 'a': case 'A':
        autoMode = true; buoc = 0;
        Serial.println("-> Che do TU DONG");
        chayBuoc();
        break;
      default:
        Serial.println("Lenh: 1 = giu BOM | 2 = giu SO | 0 = tat het | a = tu dong");
    }
  }
}

void setup() {
  pinMode(PIN_Q1, OUTPUT); digitalWrite(PIN_Q1, LOW);
  pinMode(PIN_Q2, OUTPUT); digitalWrite(PIN_Q2, LOW);

  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== TEST BOM + SO: Q1=GPIO4 (bom, C1), Q2=GPIO5 (so lanh, C2) ===");
  Serial.println("Lenh: 1 = giu BOM | 2 = giu SO | 0 = tat het | a = tu dong");
  chayBuoc();
}

void loop() {
  docLenh();
  if (autoMode && millis() - tBuoc >= CHU_KY_MS) {
    buoc = (buoc + 1) % 4;
    chayBuoc();
  }
}
