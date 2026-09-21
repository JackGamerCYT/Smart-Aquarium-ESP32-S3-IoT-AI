# Backend (Render)

Nghe MQTT 24/7, ghi Postgres (Neon), phục vụ REST API và trợ lý dữ liệu cho dashboard.

## Chạy trên máy
```bash
cd backend
npm install
set DATABASE_URL=postgresql://...        # Windows PowerShell: $env:DATABASE_URL="postgresql://..."
node server.js                           # http://localhost:8000/api/health
```

## Biến môi trường
| Biến | Mặc định | Ý nghĩa |
|---|---|---|
| `DATABASE_URL` | – | Chuỗi kết nối Neon (bắt buộc) |
| `MQTT_URL` | `mqtt://broker.hivemq.com:1883` | Broker cho backend |
| `MQTT_WS_URL` | `wss://broker.hivemq.com:8884/mqtt` | Broker WebSocket trả cho web qua `/api/config` |
| `MQTT_TOPIC_BASE` | `smartaquarium_node2026` | Trùng firmware + `index.html` |
| `MQTT_USER` / `MQTT_PASS` | – | Khi dùng HiveMQ Cloud |
| `API_KEY` | rỗng | Bắt buộc header `X-API-Key` cho POST/PUT |
| `INGEST_MIN_INTERVAL_MS` | 10000 | Gộp telemetry, tránh phình database |
| `RETENTION_DAYS` | 60 | Tự xóa dữ liệu cũ |

## Endpoint
`/api/health` · `/api/config` · `/api/status` · `/api/history?hours=` · `/api/stats?hours=` · `/api/feeds?days=`
· `/api/energy/daily?days=` · `/api/events` · `/api/commands` · `/api/availability`
· `POST /api/command` · `GET|PUT /api/settings` · `GET /api/export/{telemetry|events|commands|availability}.csv`
· `POST /api/chat`
