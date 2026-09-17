# 🐠 SMART AQUARIUM IoT – BỂ CÁ THÔNG MINH (ESP32-S3)

Hệ thống quản lý bể cá mini 4.5 L: **tự động làm mát** bằng sò Peltier (hysteresis 25.5–27.0°C), **cho cá ăn theo lịch thời gian thực** (RTC DS3231 – chạy cả khi mất Internet/mất điện), hiển thị OLED tại chỗ và **giám sát/điều khiển từ xa** qua MQTT + Web Dashboard trên Vercel.

> Môn **Embedded System Design** – GVHD: Huỳnh Hoàng Hà – Trường ĐH Sư phạm Kỹ thuật TP.HCM (HCMUTE)
> Trạng thái: **Tuần 6 – System Integration** · Firmware **v2.2.0** · Dashboard **v4** · **Vercel API + Postgres (Neon) + HiveMQ** · Phần cứng **Rev2**

## 📁 Cấu trúc repository

```text
smart-aquarium-ai/
├── README.md                         # README tổng (file này)
├── index.html                        # Web Dashboard v4 (HiveMQ realtime + Vercel API + Chart.js)
├── api/                              # Vercel Functions: ingest, history, stats, feeds, events, export, config, health, cron
├── lib/                              # Kết nối Postgres (pg), tiện ích HTTP, schema
├── db/schema.sql                     # Cấu trúc bảng (API tự tạo, không cần chạy tay)
├── package.json                      # Dependency: pg
├── vercel.json                       # Cron dọn dữ liệu + header
├── .env.example                      # Danh sách Environment Variables
├── docs/
│   ├── README.md
│   ├── 01_TONG_KET_KET_QUA_TUAN.md    # Tổng kết kết quả từng tuần (W1 → W6)
│   ├── 02_DAC_TA_HE_THONG.md          # Đặc tả: yêu cầu, FR/NFR, kiến trúc, MQTT, state machine
│   ├── 03_LO_TRINH_TIEN_DO_PHAN_CONG.md # Lộ trình 9 tuần, tiến độ, phân công, quản lý cá nhân
│   ├── 04_PHAN_CUNG_REV2_RTC.md       # Làm lại toàn bộ mạch + tích hợp RTC DS3231
│   └── 05_WEB_DATABASE.md             # Triển khai Vercel + Postgres + HiveMQ (public/Cloud)
├── firmware/
│   ├── README.md
│   └── smart_aquarium_esp32s3/
│       └── smart_aquarium_esp32s3.ino # Firmware v2.2.0
├── hardware/
│   ├── README.md
│   └── danh_sach_mua_sam_v1.1.xlsx     # BOM & dự toán (cập nhật Rev2)
├── report/
│   ├── README.md
│   └── Report_Chuong1_2.docx           # Báo cáo: Chương 1 Tổng quan, Chương 2 Cơ sở lý thuyết
└── weekly/
    └── W06/README.md                  # Bản nộp tuần 6
```

## ⚙️ Tính năng chính (v2.2.0)

| Nhóm | Tính năng |
|---|---|
| Nhiệt độ | DS18B20 đọc bất đồng bộ 1 s · hysteresis AUTO · MANUAL tự hết hạn 30 phút · nghỉ tối thiểu 60 s · quạt chạy trễ 60 s |
| Cho ăn | Servo MG90S · nút FEED / Web / **lịch tối đa 4 mốc theo RTC** · tạm dừng bơm · chống cho ăn lặp & quá tay |
| Thời gian | **DS3231** giữ giờ bằng pin · NTP → RTC · đặt giờ từ trình duyệt · ngắt SQW 1 Hz |
| An toàn | Lỗi cảm biến → tắt sò · ≥30°C ép bật · ≤22°C ép tắt · bơm tắt → khóa sò · còi cảnh báo |
| IoT | MQTT HiveMQ public (1883) hoặc **HiveMQ Cloud (TLS 8883 + user/pass)** · telemetry 2 s · event · LWT online/offline · **ACK cho mỗi lệnh web** · lưu cấu hình NVS |
| Database | **Vercel Functions + Postgres (Neon)**: ESP32 gửi theo lô lên `/api/ingest` (task FreeRTOS riêng) · web vẽ lịch sử 1h–7 ngày, KPI, cho ăn theo ngày, nhật ký, xuất CSV · cron dọn dữ liệu cũ |

## 📌 Sơ đồ chân Rev2 (tóm tắt)

| Linh kiện | GPIO | Ghi chú |
|---|---|---|
| DS18B20 | 4 | Trở kéo 4.7k lên **3V3** |
| OLED SH1106 + DS3231 (I2C) | SDA 8 / SCL 9 | Cấp **3V3**, 0x3C / 0x68 |
| DS3231 SQW | 7 | Ngắt 1 Hz |
| Relay CH1 – Sò Peltier | 5 | Active-LOW, tháo jumper JD-VCC |
| Relay CH2 – Bơm | 6 | Active-LOW |
| Quạt (IRLZ44N) | 15 | Chạy trễ 60 s |
| Servo MG90S | 13 | Tụ 1000 µF |
| Còi (qua S8050) | 14 | |
| Nút BOOT / FEED | 0 / 16 | Giữ BOOT 2 s = Self-Test |

Chi tiết đấu nối, nguồn 12V-10A, quy trình bring-up: **[docs/04_PHAN_CUNG_REV2_RTC.md](docs/04_PHAN_CUNG_REV2_RTC.md)**

## 🚀 Bắt đầu nhanh

1. **Firmware:** xem [firmware/README.md](firmware/README.md) – cài thư viện, sửa Wi-Fi & `TOPIC_BASE`, nạp bằng Arduino IDE.
2. **Vercel + Database + HiveMQ:** push GitHub → Vercel **Storage → Neon** → nhập Environment Variables (xem `.env.example`) → Redeploy. Chi tiết: [docs/05_WEB_DATABASE.md](docs/05_WEB_DATABASE.md).
3. **Kiểm tra:** mở `https://<app>.vercel.app/api/health`.
4. **Phần cứng:** lắp theo thứ tự B1 → B10 trong `docs/04`.

## 👥 Nhóm

Xem bảng thành viên & phân công: [docs/03_LO_TRINH_TIEN_DO_PHAN_CONG.md](docs/03_LO_TRINH_TIEN_DO_PHAN_CONG.md)
