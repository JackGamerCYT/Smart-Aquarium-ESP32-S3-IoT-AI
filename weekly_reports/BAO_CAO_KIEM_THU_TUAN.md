# BÁO CÁO KIỂM THỬ TÍCH HỢP PHẦN CỨNG - TUẦN 3
**Đồ án:** Hệ Thống Bể Cá Thông Minh AI + IoT (Mini Tank 4.5L)  
**Giảng viên hướng dẫn (Supervisor):** Thầy Huỳnh Hoàng Hà  
**Thành viên thực hiện (Team):**  
1. Lê Ngọc Hoàn (MSSV: 24119038)  
2. Võ Trần Đăng Khoa (MSSV: 24119051)  
3. Huỳnh Lê Thanh Liêm (MSSV: 24119053)  

---

## 1. MỤC TIÊU KIỂM THỬ TUẦN NÀY (WEEKLY MILESTONE)
Trọng tâm báo cáo tuần này là hoàn thiện và đánh giá tính an toàn, ổn định của **Khối Nguồn - Vi điều khiển - Thiết bị chấp hành công suất lớn**. Toàn bộ quy trình kiểm thử được chia thành các phần:
*   **Kiểm thử tĩnh (Static Test):** Đo kiểm ngắn mạch, sụt áp và ổn định hóa tuyến tính các đường nguồn 12V, 5V, 3.3V.
*   **Kiểm thử độc lập (Unit Test):** 
    *   Đọc và xử lý tín hiệu từ cảm biến nhiệt độ chống nước **DS18B20**.
    *   Đóng ngắt kênh rơ-le (Relay) kích hoạt **Động cơ bơm hút nước 12V**.
    *   Đóng ngắt kênh rơ-le kích hoạt **Sò nóng lạnh Peltier TEC1-12706 (12V - 60W)**.
    *   Hiển thị thông số kiểm tra lên màn hình **LCD 1602 (I2C)** hoặc **TFT ST7789**.
*   **Kiểm thử tích hợp (Integration Test):** Vận hành thuật toán điều khiển trễ **Hysteresis** tự động đóng ngắt Chiller bảo vệ hệ sinh thái của bể cá 4.5L.

---

## 2. SƠ ĐỒ ĐẤU NỐI CHI TIẾT (WIRING DIAGRAM)
Để đảm bảo chống sốc dòng và triệt tiêu nhiễu tần số cao từ sò Peltier sang chip vi điều khiển ESP32-S3, sơ đồ đấu nối được thiết kế phân tầng nguồn rõ ràng:

### A. Sơ đồ phân tầng nguồn (Power Tree)
```
                          [ Nguồn Tổ Ong 12V - 5A ]
                                     │
            ┌────────────────────────┴────────────────────────┐
            ▼                                                 ▼
   [Đường Động Lực 12V]                             [Mạch Hạ Áp LM2596]
            │                                                 │
   (Cấp cho Sò Peltier & Bơm)                        (Hạ áp xuống đúng 5.0V)
            │                                                 │
            ▼                                        ┌────────┴────────┐
      [Kênh Rơ-le]                                   ▼                 ▼
                                               [Chân VIN MCU]    [VCC Servo/Buzzer]
                                                     │
                                                     ▼
                                            [LDO 3.3V của ESP32]
                                                     │
                                                     ▼
                                            [DS18B20 & RTC DS3231]
```

### B. Bảng gán chân GPIO (Pinout Table)
| Linh kiện | Chân linh kiện | GPIO trên ESP32-S3 | Chức năng kỹ thuật |
| :--- | :--- | :--- | :--- |
| **DS18B20** | Data | **GPIO 4** | Cần trở kéo **4.7kΩ** lên đường nguồn 3.3V. |
| **Relay Bơm** | IN1 | **GPIO 18** | Kích rơ-le mở nguồn 12V cho động cơ bơm hút nước. |
| **Relay Chiller**| IN2 | **GPIO 19** | Kích rơ-le mở nguồn 12V cho sò tản nhiệt Peltier. |
| **LCD 1602** | SDA / SCL | **GPIO 21 / GPIO 22**| Giao tiếp bus I2C hiển thị thông số kiểm tra. |
| **Nút bấm** | Key Pin | **GPIO 15** | Nhấn nút vật lý để chuyển đổi nhanh các giai đoạn test. |

---

## 3. QUY TRÌNH TRIỂN KHAI & BẢN MÃ GIẢ (PSEUDOCODE)

```
KHỞI TẠO Hệ thống:
    Cấu hình chân Đầu ra cho PIN_RELAY_PUMP và PIN_RELAY_PELTIER
    Đặt trạng thái mặc định: TẮT tất cả Rơ-le
    Khởi động cảm biến DS18B20 và màn hình LCD
    Liên kết ngắt ngoài (Interrupt) cho PIN_BUTTON_TEST

VÒNG LẶP LIÊN TỤC (Loop):
    Đọc nhiệt độ nước từ DS18B20

    NẾU Nhấn nút chuyển pha kiểm thử:
        Chuyển trạng thái PHASE tương ứng (1 -> 2 -> 3 -> 4)

    LỰA CHỌN trạng thái PHASE:
        PHASE 1 (Đo nhiệt độ):
            Hiển thị nhiệt độ đo được lên LCD
            Kiểm tra trạng thái kết nối cảm biến
            
        PHASE 2 (Test bơm):
            Bật Relay Bơm (IN1 -> LOW) -> Giữ 2 giây
            Tắt Relay Bơm (IN1 -> HIGH) -> Giữ 2 giây
            Hiển thị trạng thái "Pump Testing" lên LCD

        PHASE 3 (Test Sò lạnh Peltier):
            Bật Relay Chiller (IN2 -> LOW) -> Giữ 3 giây
            Tắt Relay Chiller (IN2 -> HIGH) -> Giữ 2 giây
            Hiển thị trạng thái "Peltier Testing" lên LCD
            *Lưu ý: Quạt mặt nóng phải quay đồng thời để tránh cháy sò*

        PHASE 4 (Chạy tích hợp Hysteresis Loop):
            NẾU Nhiệt độ nước >= 27.0 độ C:
                Kích hoạt Bơm + Bật sò lạnh Peltier (Làm mát nước)
            NẾU Nhiệt độ nước <= 25.5 độ C:
                Ngắt sò lạnh Peltier + Tắt bơm (Chế độ chờ)
```

---

## 4. MA TRẬN KẾT QUẢ KIỂM THỬ KỲ VỌNG (TEST MATRIX)
Dưới đây là bảng ma trận kiểm thử được thiết kế khoa học giúp giảng viên dễ dàng đánh giá tiến độ thực tế:

| STT | Hạng mục kiểm thử | Điều kiện kiểm thử | Kết quả kỳ vọng (Expected) | Trạng thái thực tế |
| :--- | :--- | :--- | :--- | :---: |
| 1 | **Kiểm tra áp Buck** | Cấp nguồn 12V vào LM2596 | Đầu ra đạt **đúng 5.0V ± 0.1V** trên đồng hồ đo vạn năng trước khi cắm tải. | **ĐẠT** |
| 2 | **Kiểm tra thông mạch**| Đo thông mạch các chân | Đồng hồ **không kêu bíp** giữa các đường `12V - 5V - 3.3V` và `GND`. | **ĐẠT** |
| 3 | **Khởi chạy ESP32-S3** | Cắm cáp nạp Type-C | Serial Monitor hiển thị thông tin khởi tạo hệ thống ổn định. | **ĐẠT** |
| 4 | **Cảm biến DS18B20** | Nhúng cảm biến vào nước ấm | Nhiệt độ tăng đều, phản hồi mượt mà không bị ngắt quãng. | **ĐẠT** |
| 5 | **Động cơ bơm hút** | Kích chân GPIO 18 (LOW) | Rơ-le 1 kêu "tách", động cơ bơm quay khỏe, hút nước tuần hoàn qua khối nhôm. | **ĐẠT** |
| 6 | **Sò lạnh Chiller** | Kích chân GPIO 19 (LOW) | Rơ-le 2 kích hoạt, quạt tản nhiệt quay mạnh, khối nhôm Water Block lạnh dần. | **ĐẠT** |
| 7 | **Hysteresis Loop** | Giả lập nhiệt độ biến thiên | Tự động bật làm mát khi \\(T \ge 27.0^\circ\text{C}\\) và ngắt khi \\(T \le 25.5^\circ\text{C}\\). | **ĐẠT** |

---

## 5. CÁC NGUYÊN TẮC AN TOÀN TRONG QUÁ TRÌNH THỰC NGHIỆM
Khi tiến hành kiểm thử các linh kiện công suất lớn, nhóm nghiên cứu tuân thủ nghiêm ngặt các nguyên tắc sau:
1.  **Tuyệt đối không chạy sò Peltier chay:** Mặt nóng của sò Peltier có thể đạt tới \\(80^\circ\text{C}\\) trong vòng vài giây nếu không có tản nhiệt quạt. Việc chạy sò không kèm tản nhiệt khí sẽ làm hỏng sò vĩnh viễn ngay lập tức.
2.  **Mồi nước cho bơm:** Động cơ bơm hút nước màng rung 12V cần có dòng nước mồi, tránh chạy khô liên tục quá lâu gây mòn và nóng buồng bơm.
3.  **Điểm chung mát (Common GND):** GND động lực của sò Peltier và GND tín hiệu của ESP32-S3 được kết nối chụm chung tại chân ra GND của LM2596 để tránh hiện tượng dòng rò từ sò lạnh làm loạn xung điều khiển của vi điều khiển.
