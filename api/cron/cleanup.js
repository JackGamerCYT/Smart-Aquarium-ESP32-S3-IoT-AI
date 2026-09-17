// GET /api/cron/cleanup – Vercel Cron chạy mỗi ngày (vercel.json), xóa dữ liệu cũ
import { query } from '../../lib/db.js';
import { safeEqual, fail, intParam } from '../../lib/http.js';

export default async function handler(req, res) {
  // Có CRON_SECRET → kiểm tra header; không có → chỉ nhận request từ Vercel Cron (user-agent vercel-cron)
  const authorized = process.env.CRON_SECRET
    ? safeEqual(req.headers.authorization, `Bearer ${process.env.CRON_SECRET}`)
    : /vercel-cron/i.test(req.headers['user-agent'] || '');
  if (!authorized) {
    return res.status(401).json({ error: 'Unauthorized' });
  }
  const days = intParam(process.env.RETENTION_DAYS, 30, 1, 3650);
  try {
    const t = await query(`with d as (delete from telemetry where created_at < now() - make_interval(days => $1) returning 1) select count(*)::int as n from d`, [days]);
    const e = await query(`with d as (delete from events where created_at < now() - make_interval(days => $1) returning 1) select count(*)::int as n from d`, [days * 3]);
    res.status(200).json({ ok: true, telemetry_deleted: t[0].n, events_deleted: e[0].n });
  } catch (err) { fail(res, err); }
}
