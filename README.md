# 🐠 BỂ CÁ THÔNG MINH AI (SMART AQUARIUM IOT & AI)

Hệ thống quản lý bể cá thông minh 4.5L sử dụng vi điều khiển **ESP32-S3 DevKit**, cảm biến nhiệt độ DS18B20 chống nước, màn hình OLED 1.3" I2C, Relay Chiller tản nhiệt Peltier, Relay máy bơm lọc nước và Động cơ Servo MG90S cho cá ăn tự động. Đồng bộ dữ liệu hai chiều thời gian thực qua **MQTT Broker (HiveMQ)** và giao diện Web Dashboard triển khai trên **Vercel**.

---

## 📁 CẤU TRÚC THƯ MỤC REPOSITORY (GITHUB)

```text
smart-aquarium-ai/
├── index.html                  # Giao diện Website Dashboard (Tailwind CSS, MQTT.js, Chart.js)
├── vercel.json                 # File cấu hình triển khai tự động trên Vercel
├── README.md                   # Hướng dẫn chi tiết dự án
└── firmware/
    └── smart_aquarium_esp32s3.ino  # Mã nguồn C++ nạp cho vi điều khiển ESP32-S3
```

---

## ⚡ HƯỚNG DẪN ĐẨY CODE LÊN GITHUB & TRUYỀN LÊN VERCEL BẰNG VS CODE

### Bước 1: Chuẩn bị VS Code & Git
1. Mở **Visual Studio Code (VS Code)**.
2. Tạo một thư mục mới tên `smart-aquarium-ai` trên máy tính.
3. Tạo 2 file cốt lõi trong thư mục này:
   * `index.html` (Nội dung mã nguồn Website Dashboard bên dưới)
   * `vercel.json` (Nội dung file cấu hình Vercel)
4. Tạo thư mục `firmware/` và lưu file `smart_aquarium_esp32s3.ino` vào đó.

### Bước 2: Đẩy Repository lên GitHub qua VS Code Terminal
Mở Terminal trong VS Code (`Ctrl + ~` hoặc `Cmd + ~`) và chạy lần lượt các lệnh:

```bash
git init
git add .
git commit -m "Initial commit - Smart Aquarium IoT System"
git branch -M main
git remote add origin https://github.com/YOUR_USERNAME/smart-aquarium-ai.git
git push -u origin main
```
*(Thay `YOUR_USERNAME` bằng tên tài khoản GitHub của bạn).*

### Bước 3: Triển khai Website tự động lên Vercel
1. Truy cập [https://vercel.com](https://vercel.com) và đăng nhập bằng tài khoản GitHub.
2. Nhấn nút **"Add New..."** ➔ chọn **"Project"**.
3. Chọn Repository **`smart-aquarium-ai`** vừa đẩy lên GitHub.
4. Bấm **"Deploy"**. Sau 10–15 giây, Vercel sẽ cấp cho bạn một đường link Web HTTPS công khai dạng:  
   `https://smart-aquarium-ai.vercel.app`

---

## 🔌 HƯỚNG DẪN NẠP CODE CHO ESP32-S3 BẰNG ARDUINO IDE

1. Mở phần mềm **Arduino IDE**.
2. Vào **File** ➔ **Preferences**, thêm URL sau vào mục *Additional Boards Manager URLs*:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. Vào **Tools** ➔ **Board** ➔ **Boards Manager**, tìm kiếm `esp32` và nhấn **Install**.
4. Cài đặt các thư viện cần thiết (**Tools** ➔ **Manage Libraries...**):
   * `PubSubClient` (bởi Nick O'Leary)
   * `ArduinoJson` (bởi Benoit Blanchon)
   * `OneWire` & `DallasTemperature`
   * `ESP32Servo`
   * `Adafruit SSD1306` & `Adafruit GFX Library`
5. Chọn Board: **Tools** ➔ **Board** ➔ **ESP32 Arduino** ➔ **ESP32S3 Dev Module**.
6. Sửa lại **SSID** và **Password** Wi-Fi nhà bạn trong file `smart_aquarium_esp32s3.ino`.
7. Cắm cáp USB nối ESP32-S3 với máy tính, chọn đúng cổng **COM/Port** và nhấn nút **Upload (➔)**.

---

## 📌 SƠ ĐỒ ĐẤU NỐI CHÂN PIN ESP32-S3

| Tên linh kiện | Chân linh kiện | Chân cắm trên ESP32-S3 | Ghi chú kỹ thuật |
| :--- | :--- | :--- | :--- |
| **DS18B20** | Data | **GPIO 4** | Cần điện trở kéo 4.7kΩ lên nguồn 5V |
| **Relay 1 (Chiller)** | IN1 | **GPIO 19** | Điều khiển Sò lạnh Peltier + Quạt 12V |
| **Relay 2 (Bơm lọc)** | IN2 | **GPIO 18** | Điều khiển Máy Bơm Lọc Nước 12V |
| **Servo MG90S** | PWM Signal | **GPIO 13** | Hộc xoay cho cá ăn tự động |
| **Còi Bíp Active** | VCC (+) | **GPIO 12** | Phát còi cảnh báo sự cố |
| **OLED 1.3" I2C** | SDA / SCL | **GPIO 8 / GPIO 9** | Màn hình hiển thị đồ họa 128x64 |
| **Nút BOOT** | Button | **GPIO 0** | Kích hoạt chu trình Self-Test thủ công |

---

## 🛠️ GIẢI QUYẾT LỖI PHẦN CỨNG THƯỜNG GẶP

1. **Lỗi `BROWNOUT_RST` (Sụt áp làm ESP32-S3 reset liên tục):**
   * **Nguyên nhân:** Dùng duy nhất nguồn cáp USB máy tính để nuôi cả Servo và Relay.
   * **Khắc phục:** Bắt buộc cấp nguồn **12V - 5A** ngoài qua mạch hạ áp **LM2596/XL4015** vặn biến trở về đúng **5.0V** rồi cắm vào chân `VIN` của ESP32-S3.
2. **Lỗi `invalid conversion from char to const char*`:**
   * **Khắc phục:** Đã được sửa triệt để trong bản code `smart_aquarium_esp32s3.ino` bằng cách khai báo mảng bộ đệm `char buffer[256];`.

---
*Đồ án Phát triển Ứng dụng IoT - Trường Đại học Sư phạm Kỹ thuật TP.HCM (HCMUTE)*
