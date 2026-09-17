# 01 – TỔNG KẾT KẾT QUẢ THEO TUẦN

> Dự án: **Bể cá thông minh IoT (Smart Aquarium)** – Môn Embedded System Design (HCMUTE)
> Cập nhật: **17/09/2026 – Tuần 6** · Quy ước: ✅ hoàn thành · 🔄 đang làm · ⏳ chưa làm · ⚠️ phải làm lại
> *Ngày của từng tuần là dự kiến theo lịch 9 tuần – nhóm chỉnh lại theo lịch lớp thực tế. Cột "Minh chứng" điền link commit/ảnh/video.*

## Tổng quan tiến độ

| Tuần | Giai đoạn (theo roadmap môn) | Output yêu cầu | Trạng thái |
|---|---|---|---|
| W1 | Introduction & Requirement Analysis | Requirement Analysis + Proposed Solutions | ✅ |
| W2 | System Specification | System Specification | ✅ (bổ sung v2.0 ở W6) |
| W3 | Architecture & HW/SW Design | Architecture + HW/SW Design + BOM | ✅ (⚠️ sửa pinout/nguồn ở W6) |
| W4 | Prototype & Feasibility Validation | Validated Modules + Test Results | ✅ phần lớn module |
| W5 | Implementation | Working Modules + Source Code | ✅ FW v1 + Web v1 |
| W6 | System Integration | Integrated System / Prototype | 🔄 Rev2 phần cứng + RTC + FW v2 |
| W7 | Testing & Optimization | Test Results + Improved System | ⏳ |
| W8 | Finalization & Presentation | Final Prototype + Report + Slide | ⏳ (Report Ch.1–2 đã xong ở W6) |
| W9 | Evaluation | Final Evaluation | ⏳ |

---

## Tuần 1 – Phân tích yêu cầu
**Câu hỏi trung tâm:** *Hệ thống giải quyết vấn đề gì?*

| Kết quả | Minh chứng |
|---|---|
| Xác định bài toán: bể mini 4.5 L nóng nhanh theo phòng; quên/cho ăn quá tay khi vắng nhà | `docs/02_DAC_TA_HE_THONG.md` §1 |
| Bảng Requirement Analysis (8 use case) + giải pháp đề xuất | `docs/02_DAC_TA_HE_THONG.md` §3 |
| Thành lập nhóm, tạo repo GitHub, nhóm Zalo | Link repo: `…` |

## Tuần 2 – Đặc tả hệ thống
**Câu hỏi trung tâm:** *Hệ thống chính xác phải làm gì?*

| Kết quả | Minh chứng |
|---|---|
| Đặc tả chức năng: làm mát hysteresis 25.5–27.0°C, cho ăn servo, OLED, IoT MQTT | `TONG_HOP_HE_THONG_HO_CA_SMART.md` |
| Chọn phương án làm mát bằng sò Peltier + water block (thay vì quạt thổi mặt nước) | Slide/biên bản nhóm |
| **Bổ sung W6:** 16 FR + 8 NFR có tiêu chí kiểm chứng, ma trận truy vết | `docs/02_DAC_TA_HE_THONG.md` §4, §5, §8 |

## Tuần 3 – Kiến trúc & thiết kế HW/SW
**Câu hỏi trung tâm:** *Tổ chức hệ thống như thế nào?*

| Kết quả | Minh chứng |
|---|---|
| Chọn ESP32-S3 DevKitC (Wi-Fi, PSRAM, dư GPIO), DS18B20 chống nước, MG90S, TEC1-12706 | `README.md` |
| Bảng dự toán & mua sắm linh kiện v1.0 (23/08/2026) | `danh_sach_mua_sam_ho_ca_smart.xlsx` |
| Sơ đồ nối chân Rev1, phân phối nguồn 12V/5V | `TONG_HOP_HE_THONG_HO_CA_SMART.md` §3–4 |
| ⚠️ **Phát hiện ở W6:** BOM ghi TFT ST7789 nhưng thiết kế dùng OLED I2C; thiếu relay & bơm; nguồn 5A không đủ; GPIO19 trùng USB | `docs/04_PHAN_CUNG_REV2_RTC.md` §1 |

## Tuần 4 – Thử nghiệm module
**Câu hỏi trung tâm:** *Linh kiện đã chọn có chạy được không?*

> ⚠️ Bảng này được tổng hợp từ các lỗi đã ghi trong README/TONG_HOP v1 – **nhóm kiểm tra lại và sửa PASS/FAIL cho đúng thực tế** trước khi nộp.

| Module | Test cơ bản | Kết quả | Ghi chú / hành động |
|---|---|---|---|
| ESP32-S3 | Blink, Wi-Fi scan | PASS | – |
| DS18B20 | Đọc nhiệt độ | PASS | ⚠️ trở kéo đang lên 5V → sửa lên 3V3 |
| OLED 1.3" | Hiển thị chữ | PASS* | *Dùng lib SSD1306 cho chip SH1106 → lệch cột → đổi lib |
| Relay 2 kênh | ON/OFF | FAIL một phần | IN1 sáng đỏ liên tục → nguyên nhân: mức 3.3V không tắt opto khi VCC=5V |
| Servo MG90S | Sweep 0–110° | PASS* | *Reset `BROWNOUT_RST` khi chạy bằng USB → dùng nguồn ngoài + sweep chậm |
| Còi active | Bíp | PASS | Âm nhỏ → thêm transistor |
| MQTT HiveMQ | Publish/Subscribe | PASS | – |
| RTC DS3231 | – | ⏳ | Test ở W6 (bước B4 bring-up) |

## Tuần 5 – Hiện thực
**Câu hỏi trung tâm:** *Xây được các module đã thiết kế chưa?*

| Kết quả | Minh chứng |
|---|---|
| Firmware v1: đọc DS18B20, hysteresis chiller, servo cho ăn + tạm dừng bơm, OLED, Self-Test, MQTT 2 chiều | `smart_aquarium_esp32s3.ino` (v1) |
| Web dashboard v1 (Tailwind + MQTT.js + Chart.js) deploy Vercel | `Index1.html`, link Vercel `…` |
| Hướng dẫn nạp code, đẩy GitHub, deploy | `README.md` |
| **Lỗi còn tồn tại (đã phân tích ở W6):** vòng lặp blocking (`delay`, `requestTemperatures`), MANUAL không tự về AUTO, biểu đồ web lỗi `datasets.data`, backend `localhost` không gọi được từ Vercel, bộ đếm cho ăn mất khi reset | `docs/04_PHAN_CUNG_REV2_RTC.md`, commit W6 |

## Tuần 6 – Tích hợp hệ thống (tuần hiện tại)
**Câu hỏi trung tâm:** *Các module có làm việc cùng nhau không?*

| Hạng mục | Người phụ trách | Trạng thái | Minh chứng |
|---|---|---|---|
| Rà soát toàn bộ mạch Rev1, lập danh sách 13 lỗi/cải tiến | Rel | ✅ | `docs/04` §1 |
| Thiết kế lại mạch Rev2: pinout mới, nguồn 12V-10A + cầu chì, JD-VCC relay, MOSFET quạt, transistor còi, tụ servo | Rel | ✅ thiết kế · 🔄 hàn PCB | `docs/04` §3–6 |
| **Tích hợp RTC DS3231** (I2C chung OLED + SQW GPIO7) | Rel | ✅ thiết kế + FW · 🔄 test phần cứng | `docs/04` §4.5, §8 |
| Firmware v2.0.0: non-blocking, RTC/NTP, lịch cho ăn, an toàn, NVS, LWT | Rel + TV2 | ✅ code · 🔄 nạp & chạy thật | `firmware/…ino` |
| Web v2: sửa biểu đồ, hiển thị giờ RTC, lịch, ngưỡng, cảnh báo, đồng bộ giờ | TV3 | ✅ code · 🔄 deploy | `index.html` |
| Cập nhật BOM v1.1 | Rel | ✅ | `hardware/danh_sach_mua_sam_v1.1.xlsx` |
| Đặc tả hệ thống v2.0 | TV4 + Rel | ✅ | `docs/02` |
| Report Chương 1 & 2 | TV4 + cả nhóm | ✅ | `report/Report_Chuong1_2.docx` |
| Test end-to-end: Web → MQTT → relay → OLED | Cả nhóm | ⏳ sau khi hàn xong | video demo `…` |

**Use case tích hợp phải demo cuối W6:**
1. Ngâm cảm biến vào nước ấm 28°C → sò + quạt bật → OLED `LANH:ON` → Web cập nhật ≤ 2 s.
2. Tắt router → đến mốc lịch → servo cho ăn, bơm tạm dừng rồi chạy lại → OLED `An:1/ngay`.
3. Rút dây DS18B20 → sò tắt, còi kêu, Web hiện cảnh báo `Mất cảm biến`.

**Vướng mắc / rủi ro:** chờ linh kiện bổ sung (relay, bơm, nguồn 10A); cần đo năng lực làm lạnh thực tế của sò với bể 4.5 L ở W7.

---

## Mẫu cập nhật cho tuần tiếp theo (copy khi sang W7)

```markdown
## Tuần N – <Giai đoạn>
**Câu hỏi trung tâm:** …
| Hạng mục | Người phụ trách | Trạng thái | Minh chứng |
|---|---|---|---|
**Kết quả chính:** …
**Vướng mắc:** …
**Kế hoạch tuần sau:** …
```
