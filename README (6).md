# API (Vercel Functions)

| Endpoint | Method | Dùng bởi | Mô tả |
|---|---|---|---|
| `/api/ingest` | POST | ESP32 | Header `x-device-key`. Body `{"items":[{"t":"telemetry"\|"events","r":{...}}]}` (≤ 60) |
| `/api/history?hours=24` | GET | Web | Nhiệt độ avg/min/max + % sò chạy theo bucket |
| `/api/stats?hours=24` | GET | Web | KPI |
| `/api/feeds?days=7` | GET | Web | Số lần cho ăn/ngày |
| `/api/events?type=&limit=100` | GET | Web | Nhật ký |
| `/api/export?hours=24` | GET | Web | CSV |
| `/api/config` | GET | Web | MQTT URL, topic, tài khoản web (từ env) |
| `/api/health` | GET | Người dùng | Kiểm tra env + DB |
| `/api/cron/cleanup` | GET | Vercel Cron | Xóa dữ liệu cũ (`Authorization: Bearer CRON_SECRET`) |

Biến môi trường: xem `../.env.example`. Hướng dẫn: `../docs/05_WEB_DATABASE.md`.
