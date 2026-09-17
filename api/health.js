// GET /api/health – kiểm tra nhanh cấu hình & database (không lộ giá trị bí mật)
import { query } from '../lib/db.js';
import { allowMethods } from '../lib/http.js';

export default async function handler(req, res) {
  if (!allowMethods(req, res, ['GET'])) return;
  const env = {
    DATABASE_URL: Boolean(process.env.DATABASE_URL || process.env.POSTGRES_URL),
    DEVICE_KEY: process.env.DEVICE_KEY ? 'bật' : 'tắt (API mở)',
    CRON_SECRET: process.env.CRON_SECRET ? 'bật' : 'tắt',
    MQTT_TOPIC_BASE: process.env.MQTT_TOPIC_BASE || '(mặc định)',
    MQTT_WEB_USERNAME: Boolean(process.env.MQTT_WEB_USERNAME),
  };
  try {
    const [r] = await query(`select (select count(*)::int from telemetry) as telemetry_rows,
                                    (select count(*)::int from events)    as event_rows,
                                    (select max(created_at) from telemetry) as last_telemetry, now() as server_time`);
    res.status(200).json({ ok: true, env, db: r });
  } catch (err) {
    res.status(500).json({ ok: false, env, error: err.message });
  }
}
