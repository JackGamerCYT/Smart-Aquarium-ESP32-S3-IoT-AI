// GET /api/events?type=feed&limit=100
import { query } from '../lib/db.js';
import { allowMethods, intParam, deviceParam, fail } from '../lib/http.js';

export default async function handler(req, res) {
  if (!allowMethods(req, res, ['GET'])) return;
  const type = typeof req.query.type === 'string' && /^[\w-]{1,24}$/.test(req.query.type) ? req.query.type : null;
  const limit = intParam(req.query.limit, 100, 1, 500);
  try {
    const rows = await query(
      `select created_at, device_id, type, detail from events
        where ($1::text is null or type = $1) and ($2::text is null or device_id = $2)
        order by created_at desc limit $3`,
      [type, deviceParam(req.query.device), limit]);
    res.status(200).json({ events: rows });
  } catch (err) { fail(res, err); }
}
