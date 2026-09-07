# SMART AQUARIUM IOT + AI - SYSTEM DOCUMENTATION & GITHUB README

This repository contains the complete implementation for the **Smart Aquarium System**, which combines **IoT** for real-time hardware management (ESP32-S3) and **AI** (Gemini API via intermediate server) for temperature trend analysis and proactive notification alerts via Zalo.

---

## 👥 Nhóm Thực Hiện (Team Members)
* **Sinh viên thực hiện:**
  1. Lê Ngọc Hoàn (MSSV: 24119038)
  2. Võ Trần Đăng Khoa (MSSV: 24119051)
  3. Huỳnh Lê Thanh Liêm (MSSV: 24119053)
* **Giảng viên hướng dẫn:** Thầy Huỳnh Hoàng Hà (MEng.)
* **Đơn vị:** Trường Đại học Sư phạm Kỹ thuật TP.HCM (HCMUTE)

---

## 📅 Kế Hoạch Triển Khai Chi Tiết (Project Execution Plan)
Dự án được chia làm 16 bước chia theo 4 giai đoạn lớn, bám sát tiến trình chế tạo:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     QUY TRÌNH TRIỂN KHAI DỰ ÁN                          │
├─────────────────┬─────────────────┬─────────────────┬───────────────────┤
│    Giai đoạn 1  │   Giai đoạn 2   │   Giai đoạn 3   │    Giai đoạn 4    │
│  Cơ khí/Tản nhiệt│ Hàn mạch PCB FR4│ Lập trình & Test│   IoT & AI Zalo   │
└────────┬────────┴────────┬────────┴────────┬────────┴─────────┬─────────┘
         │                 │                 │                  │
         ▼                 ▼                 ▼                  ▼
     (Hoàn thành)      (Hàn mạch)       (Nạp code test)    (Cảnh báo AI)
```

### 1. Giai đoạn Cơ khí & Giải nhiệt (Mechanical & Heat Dissipation) - [HOÀN THÀNH]
* **Bước 1:** Lắp ráp hệ thống lọc thác tuần hoàn cho bể cá mini (thể tích thực 4.5 Lít, kích thước 20x14x16 cm).
* **Bước 2:** Lắp ráp sò nóng lạnh Peltier TEC1-12706 bôi keo tản nhiệt MX-4, áp mặt lạnh vào khối nhôm tản nhiệt nước (Water Block) và mặt nóng vào bộ tản nhiệt khí kèm quạt 12V.
* **Bước 3:** Đi đường ống dẻo silicone dẫn dòng chảy từ đầu ra của lọc thác chạy qua khối nhôm tản nhiệt nước trước khi xả về bể để tận dụng lực bơm tuần hoàn.

### 2. Giai đoạn Hàn mạch PCB & Đi dây (PCB Soldering & Wiring) - [ĐANG THỰC HIỆN]
* **Bước 4:** Sắp xếp vị trí linh kiện thô trên phíp lỗ xanh FR4 7x9cm và hàn các hàng rào cái (female header) cho ESP32-S3 DevKit, module RTC DS3231, màn hình TFT.
* **Bước 5:** Hàn mạch điều khiển nguồn Chiller: MOSFET kênh N (IRLZ44N/IRF3710), diode dập xung ngược 1N4007 cho quạt, optocoupler PC817 cách ly quang bảo vệ MCU, và các cọc đấu dây vặn vít Domino.
* **Bước 6:** Hàn linh kiện phụ trợ gồm Transistor S8050 đệm điều khiển còi báo động Buzzer 5V, điện trở kéo 4.7kΩ cho cảm biến DS18B20, và 2 nút bấm vật lý (Manual Feed và Mute).
* **Bước 7:** Hàn mạch giảm áp Buck LM2596, cấp nguồn 12V đầu vào và xoay biến trở điều chỉnh ngõ ra về đúng **5.0V** ổn định trước khi đấu nối vào chân VIN của ESP32-S3 và Servo.
* **Bước 8:** Tiến hành đi dây mát chung (Common GND) theo sơ đồ hình sao (Star Connection) và hàn các đường mạch đồng trần gánh dòng lớn (4A - 5A) ở mặt dưới phíp lỗ cho sò Peltier.
* **Bước 9:** Dùng đồng hồ vạn năng đo thông mạch để triệt tiêu hoàn toàn nguy cơ ngắn mạch giữa các đường nguồn 12V/5V/3.3V trước khi cấp điện thử nghiệm.

### 3. Giai đoạn Lập trình & Kiểm thử (Programming & Testing) - [SẮP TRIỂN KHAI]
* **Bước 10:** Lập trình đọc cảm biến nhiệt DS18B20 và hiển thị thông số liên tục lên màn hình màu TFT ST7789 qua giao tiếp SPI phần cứng.
* **Bước 11:** Lập trình điều khiển Servo MG90S xoay hộc cho ăn tự động khi bấm nút bấm vật lý hoặc định thời theo module RTC DS3231.
* **Bước 12:** Lập trình thuật toán điều khiển Chiller Peltier bằng xung PWM qua MOSFET với dải đệm trễ (Hysteresis Band) bảo vệ cá khỏi sốc nhiệt.

### 4. Giai đoạn Tích hợp IoT & Trí Tuệ Nhân Tạo (IoT & AI Cloud Base) - [SẮP TRIỂN KHAI]
* **Bước 13:** Cấu hình Wi-Fi cho ESP32-S3 đóng gói dữ liệu thành chuỗi JSON chứa `{nhiet_do, trang_thai_chiller, lan_cho_an}` gửi lên server trung gian (Node-RED/Make.com) định kỳ mỗi 5 phút.
* **Bước 14:** Tích hợp Gemini API trên server để phân tích xu hướng nhiệt độ nước, chẩn đoán sự cố thiết bị hoặc dự báo bốc hơi nước.
* **Bước 15:** Thiết lập hệ thống gửi tin nhắn cảnh báo bằng tiếng Việt tự nhiên về điện thoại người dùng thông qua Zalo Official Account Bot API.
* **Bước 16:** Đóng hộp in 3D lắp đặt hoàn chỉnh lên thành bể 4.5L và vận hành nghiệm thu thực tế.

---

## 🔌 Sơ Đồ Đi Dây & Phân Tầng Nguồn (Wiring Diagram & Power Tree)

### 1. Sơ đồ khối nguồn (Power Tree Block Diagram)
```
                                 [ Nguồn Tổ Ong 12V - 5A ]
                                             │
                       ┌─────────────────────┴─────────────────────┐
                       ▼                                           ▼
             [ Khối Động Lực 12V ]                       [ Mạch Giảm Áp LM2596 ]
                       │                                           │
       ┌───────────────┴───────────────┐                           ▼ (Hạ xuống đúng 5V)
       ▼                               ▼                  [ Đường Nguồn 5V Điều Khiển ]
Sò Lạnh Peltier                 Quạt Tản Nhiệt 12V                 │
(Đóng ngắt qua MOSFET           (Có Diode 1N4007                   ├─► Chân VIN (ESP32-S3)
 IRLZ44N điều khiển)             dập xung ngược)                   ├─► Nguồn VCC (Servo MG90S)
                                                                   ├─► Nguồn VCC (Buzzer 5V)
                                                                   │
                                                                   ▼ (Hạ qua LDO trên Board)
                                                          [ Đường Nguồn 3.3V Logic ]
                                                                   │
                                                                   ├─► Màn hình TFT ST7789 SPI
                                                                   ├─► Cảm biến DS18B20 (Trở kéo)
                                                                   └─► Module RTC DS3231 I2C
```

### 2. Sơ đồ đấu nối chân PIN chi tiết (ESP32-S3 Pinout)
| Thiết bị ngoại vi | Chân thiết bị | GPIO trên ESP32-S3 | Chức năng & Ghi chú kỹ thuật |
| :--- | :--- | :--- | :--- |
| **DS18B20 Temp Sensor**| VCC / GND / Data | **3.3V / GND / GPIO 4** | Giao tiếp 1-Wire. Điện trở kéo 4.7kΩ từ Data lên 3.3V. |
| **TFT ST7789 Screen**  | SCL (SCLK)<br>SDA (MOSI)<br>RES (Reset)<br>DC (Data/Cmd)<br>CS (Chip Select)<br>BLK (Backlight) | **GPIO 12**<br>**GPIO 11**<br>**GPIO 10**<br>**GPIO 9**<br>**GPIO 14**<br>**GPIO 21** | Kết nối SPI phần cứng. Sử dụng trực tiếp điện áp logic 3.3V của ESP32-S3 để chống cháy nổ màn hình. |
| **RTC DS3231**         | VCC / GND / SDA / SCL | **3.3V / GND / GPIO 21 / GPIO 22**| Giao tiếp I2C chung. Nhớ gắn kèm pin cúc áo CR2032 dự phòng. |
| **Micro Servo MG90S**  | VCC / GND / Signal | **5V (sau LM2596) / GND / GPIO 13**| Điều khiển PWM góc quay hộc chứa thức ăn. |
| **MOSFET Chiller Gate**| Gate Control | **GPIO 19** | Xuất PWM qua Optocoupler PC817 kích cực Gate của MOSFET đóng ngắt Peltier. |
| **Còi Buzzer Active 5V**| Trigger (qua Transistor)| **GPIO 18** | Đóng ngắt còi bằng transistor S8050, có diode dập xung song song còi. |
| **Nút bấm Cho Ăn**     | Chân nút 1 / GND | **GPIO 15 / GND** | Kích hoạt cho ăn tức thì (Sử dụng INPUT_PULLUP nội bộ). |
| **Nút bấm Tắt Còi**     | Chân nút 2 / GND | **GPIO 16 / GND** | Tắt tiếng còi báo động tại chỗ (INPUT_PULLUP nội bộ). |

---

## 📝 Mã Giả Chương Trình (System Pseudocode)

```python
# THUẬT TOÁN ĐIỀU KHIỂN TOÀN DIỆN BỂ CÁ THÔNG MINH

# Khai báo hằng số và cấu hình hệ thống
TEMP_HIGH_THRESHOLD = 27.0      # Ngưỡng bật Chiller Peltier
TEMP_LOW_THRESHOLD = 25.5       # Ngưỡng tắt Chiller Peltier
ALERT_CRITICAL_HIGH = 29.0      # Ngưỡng còi báo động quá nhiệt
ALERT_CRITICAL_LOW = 24.0       # Ngưỡng còi báo động lạnh đột ngột
INTERVAL_SEND_CLOUD = 300000    # Định thời gửi mây (5 phút - 300,000ms)

# Khởi tạo trạng thái
chiller_active = False
buzzer_alarm = False
last_cloud_send_time = 0
feeding_hours = [8, 17]         # Giờ cho ăn cố định (8:00 và 17:00)
has_fed_today = [False, False]  # Đánh dấu trạng thái cho ăn trong ngày

FUNCTION Setup():
    Khởi tạo giao tiếp Serial ở baudrate 115200
    Khởi tạo bus I2C cho RTC DS3231
    Khởi tạo màn hình TFT ST7789 qua giao tiếp SPI
    Khởi tạo cảm biến nhiệt độ DS18B20 (1-Wire)
    Cấu hình Servo MG90S ở chân GPIO 13
    Cấu hình chân OUTPUT: MOSFET Chiller (GPIO 19), Buzzer (GPIO 18)
    Cấu hình chân INPUT_PULLUP: Nút cho ăn (GPIO 15), Nút tắt còi (GPIO 16)
    Kết nối Wi-Fi nội bộ
    Đọc thời gian thực từ RTC DS3231 để đồng bộ ban đầu
    In màn hình khởi động thành công

FUNCTION Loop():
    # 1. Đọc dữ liệu cảm biến
    current_temp = DS18B20.ReadTemperature()
    current_time = RTC.GetCurrentTime()
    
    # 2. Thuật toán Hysteresis điều chỉnh Chiller Peltier
    IF current_temp >= TEMP_HIGH_THRESHOLD:
        chiller_active = True
        MOSFET_Peltier_PWM(Duty_Cycle = 255) # Bật tối đa công suất lạnh
    ELSE IF current_temp <= TEMP_LOW_THRESHOLD:
        chiller_active = False
        MOSFET_Peltier_PWM(Duty_Cycle = 0)   # Tắt tản nhiệt sò lạnh
        
    # 3. Giám sát an toàn & Phát còi Buzzer cảnh báo tại chỗ
    IF current_temp >= ALERT_CRITICAL_HIGH OR current_temp <= ALERT_CRITICAL_LOW:
        IF NOT buzzer_alarm:
            Buzzer_Set_State(HIGH) # Kích hoạt còi
    ELSE:
        Buzzer_Set_State(LOW)  # Tắt còi nếu nhiệt độ an toàn
        buzzer_alarm = False

    # 4. Kiểm tra nút nhấn vật lý
    IF Digital_Read(GPIO_Button_Feed) == LOW: # Nút Cho Ăn được nhấn
        Trigger_Feeding()
        Chờ 500ms để chống dội phím
        
    IF Digital_Read(GPIO_Button_Mute) == LOW: # Nút Tắt Còi được nhấn
        Buzzer_Set_State(LOW)  # Tắt còi tạm thời tại chỗ
        buzzer_alarm = True    # Đánh dấu đã tắt tạm thời
        Chờ 500ms chống dội
        
    # 5. Hẹn giờ cho ăn tự động qua RTC DS3231
    FOR i từ 0 đến length(feeding_hours) - 1:
        IF current_time.hour == feeding_hours[i] AND current_time.minute == 0:
            IF NOT has_fed_today[i]:
                Trigger_Feeding()
                has_fed_today[i] = True
        
    # Reset cờ cho ăn khi qua ngày mới
    IF current_time.hour == 0 AND current_time.minute == 0:
        has_fed_today = [False, False]

    # 6. Cập nhật hiển thị lên LCD TFT ST7789 màu
    TFT_Clear_Screen()
    TFT_Print_Text("NHIỆT ĐỘ: " + String(current_temp) + " °C")
    TFT_Print_Text("GIỜ: " + String(current_time.hour) + ":" + String(current_time.minute))
    TFT_Print_Text("CHILLER: " + (chiller_active ? "ON" : "OFF"))

    # 7. Định thời gửi dữ liệu lên Server IoT & AI
    IF Millis() - last_cloud_send_time >= INTERVAL_SEND_CLOUD:
        json_payload = Build_JSON(current_temp, chiller_active, feeding_count)
        WiFi_HTTP_Post_JSON(json_payload)
        last_cloud_send_time = Millis()

FUNCTION Trigger_Feeding():
    TFT_Print_Text("ĐANG CHO CÁ ĂN...")
    Buzzer_Beep_Short() # Phát bíp ngắn thông báo
    Xoay_Servo(Góc = 90) # Mở hộc thức ăn rơi xuống
    Chờ 1000ms (1 giây)
    Xoay_Servo(Góc = 0)  # Đóng hộc thức ăn xoay về vị trí cũ
    feeding_count = feeding_count + 1
```
