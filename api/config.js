// GET /api/config – cấu hình MQTT cho trình duyệt, lấy từ Environment Variables
// (không phải commit tài khoản HiveMQ lên GitHub). Chỉ dùng tài khoản dành riêng cho WEB.
import { allowMethods } from '../lib/http.js';

export default function handler(req, res) {
  if (!allowMethods(req, res, ['GET'])) return;
  res.status(200).json({
    mqtt: {
      url: process.env.MQTT_WS_URL || 'wss://broker.hivemq.com:8884/mqtt',
      topicBase: process.env.MQTT_TOPIC_BASE || 'hcmute/esd/beca-nhomXX',
      username: process.env.MQTT_WEB_USERNAME || '',
      password: process.env.MQTT_WEB_PASSWORD || '',
    },
    database: Boolean(process.env.DATABASE_URL || process.env.POSTGRES_URL),
  });
}
