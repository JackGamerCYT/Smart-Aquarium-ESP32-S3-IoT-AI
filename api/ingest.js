// POST /api/ingest – ESP32 gửi dữ liệu
// Nếu đặt biến DEVICE_KEY trên Vercel thì bắt buộc header x-device-key; không đặt = API mở, không cần khóa.
// Body: { "items": [ { "t": "telemetry", "r": {...} }, { "t": "events", "r": {...} } ] }
import { query } from '../lib/db.js';
import { allowMethods, safeEqual, fail } from '../lib/http.js';

const MAX_ITEMS = 60;
const MAX_CLOCK_SKEW_MS = 3 * 24 * 3600 * 1000;

const num  = (v, min, max) => (typeof v === 'number' && Number.isFinite(v) && v >= min && v <= max ? v : null);
const int  = (v, min, max) => (Number.isInteger(v) && v >= min && v <= max ? v : null);
const bool = v => (typeof v === 'boolean' ? v : null);
const text = (v, max) => (typeof v === 'string' && v.length ? v.slice(0, max) : null);

function timestamp(v) {
  const t = typeof v === 'string' ? Date.parse(v) : NaN;
  return Number.isFinite(t) && Math.abs(Date.now() - t) <= MAX_CLOCK_SKEW_MS ? new Date(t).toISOString() : null;
}

const TABLES = {
  telemetry: {
    cols: ['created_at', 'device_id', 'temp', 'chiller', 'pump', 'fan', 'mode', 'feed_today', 'feed_total', 'alarm', 'rtc_temp', 'rssi', 'time_src'],
    map: r => [timestamp(r.created_at), r.device_id, num(r.temp, -20, 100), bool(r.chiller), bool(r.pump), bool(r.fan),
      ['AUTO', 'MANUAL'].includes(r.mode) ? r.mode : null, int(r.feed_today, 0, 100), int(r.feed_total, 0, 1e9),
      text(r.alarm, 20), num(r.rtc_temp, -40, 125), int(r.rssi, -127, 0), text(r.time_src, 6)],
    valid: r => true,
  },
  events: {
    cols: ['created_at', 'device_id', 'type', 'detail'],
    map: r => [timestamp(r.created_at), r.device_id, text(r.type, 24), text(r.detail, 200)],
    valid: r => typeof r.type === 'string' && r.type.length > 0,
  },
};

async function insertRows(table, rows) {
  const { cols } = TABLES[table];
  const params = [];
  const values = rows.map(row => {
    const ph = row.map(v => { params.push(v); return `$${params.length}`; });
    ph[0] = `coalesce(${ph[0]}::timestamptz, now())`;   // created_at: giờ thiết bị hoặc giờ server
    return `(${ph.join(', ')})`;
  });
  await query(`insert into ${table} (${cols.join(', ')}) values ${values.join(', ')}`, params);
}

export default async function handler(req, res) {
  if (!allowMethods(req, res, ['POST'])) return;
  if (process.env.DEVICE_KEY && !safeEqual(req.headers['x-device-key'], process.env.DEVICE_KEY)) {
    return res.status(401).json({ error: 'Sai x-device-key' });
  }

  let body = req.body;
  if (typeof body === 'string') { try { body = JSON.parse(body); } catch { body = null; } }
  const items = Array.isArray(body?.items) ? body.items : body?.table ? [{ t: body.table, r: body.row }] : null;
  if (!items || items.length === 0) return res.status(400).json({ error: 'Body phải có "items": [...]' });
  if (items.length > MAX_ITEMS) return res.status(413).json({ error: `Tối đa ${MAX_ITEMS} bản ghi/lần` });

  const grouped = { telemetry: [], events: [] };
  let rejected = 0;
  for (const it of items) {
    const spec = TABLES[it?.t];
    const r = it?.r;
    if (!spec || !r || typeof r !== 'object' || !/^[\w-]{3,40}$/.test(r.device_id ?? '') || !spec.valid(r)) { rejected++; continue; }
    grouped[it.t].push(spec.map(r));
  }

  try {
    for (const t of Object.keys(grouped)) if (grouped[t].length) await insertRows(t, grouped[t]);
    res.status(200).json({ ok: true, telemetry: grouped.telemetry.length, events: grouped.events.length, rejected });
  } catch (err) { fail(res, err); }
}
