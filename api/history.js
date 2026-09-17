// GET /api/history?hours=24[&device=aquarium-XXXXXX]
import { query } from '../lib/db.js';
import { allowMethods, intParam, deviceParam, fail } from '../lib/http.js';

export default async function handler(req, res) {
  if (!allowMethods(req, res, ['GET'])) return;
  const hours = intParam(req.query.hours, 24, 1, 24 * 31);
  const step = hours <= 6 ? '1 minute' : hours <= 48 ? '10 minutes' : '1 hour';
  try {
    const rows = await query(
      `select date_bin($2::interval, created_at, timestamptz '2000-01-01 00:00:00+00') as bucket,
              round(avg(temp)::numeric, 2)::float8                           as temp_avg,
              round(min(temp)::numeric, 2)::float8                           as temp_min,
              round(max(temp)::numeric, 2)::float8                           as temp_max,
              round(100 * avg(case when chiller then 1 else 0 end)::numeric, 1)::float8 as chiller_pct,
              count(*)::int                                                   as samples
         from telemetry
        where created_at >= now() - make_interval(hours => $1)
          and ($3::text is null or device_id = $3)
        group by 1 order by 1`,
      [hours, step, deviceParam(req.query.device)]);
    res.status(200).json({ hours, step, points: rows });
  } catch (err) { fail(res, err); }
}
