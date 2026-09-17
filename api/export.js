// GET /api/export?hours=24 – tải CSV telemetry
import { query } from '../lib/db.js';
import { allowMethods, intParam, deviceParam, fail } from '../lib/http.js';

const COLS = ['created_at', 'device_id', 'temp', 'chiller', 'pump', 'fan', 'mode', 'feed_today', 'feed_total', 'alarm', 'rtc_temp', 'rssi', 'time_src'];
const cell = v => (v == null ? '' : v instanceof Date ? v.toISOString() : /[",\n]/.test(String(v)) ? `"${String(v).replace(/"/g, '""')}"` : String(v));

export default async function handler(req, res) {
  if (!allowMethods(req, res, ['GET'])) return;
  const hours = intParam(req.query.hours, 24, 1, 24 * 31);
  try {
    const rows = await query(
      `select ${COLS.join(', ')} from telemetry
        where created_at >= now() - make_interval(hours => $1) and ($2::text is null or device_id = $2)
        order by created_at limit 50000`,
      [hours, deviceParam(req.query.device)]);
    const csv = [COLS.join(','), ...rows.map(r => COLS.map(c => cell(r[c])).join(','))].join('\n');
    res.setHeader('Content-Type', 'text/csv; charset=utf-8');
    res.setHeader('Content-Disposition', `attachment; filename="aquarium_${hours}h.csv"`);
    res.status(200).send('﻿' + csv);   // BOM để Excel đọc đúng UTF-8
  } catch (err) { fail(res, err); }
}
