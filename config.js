// GET /api/config – cấu hình MQTT cho trình duyệt, lấy từ Environment Variables
// (không phải commit tài khoản HiveMQ lên GitHub). Chỉ dùng tài khoản dành riêng cho WEB.
import { allowMethods } from '../lib/http.js';
import { findConnectionString } from '../lib/db.js';

export default function handler(req, res) {
  if (!allowMethods(req, res, ['GET'])) return;
  res.status(200).json({
    mqtt: {
      url: process.env.MQTT_WS_URL || 'wss://broker.hivemq.com:8884/mqtt',
      topicBase: process.env.MQTT_TOPIC_BASE || null,   // null = web dùng TOPIC viết sẵn trong index.html
      username: process.env.MQTT_WEB_USERNAME || '',
      password: process.env.MQTT_WEB_PASSWORD || '',
    },
    database: Boolean(findConnectionString()),
  });
}
