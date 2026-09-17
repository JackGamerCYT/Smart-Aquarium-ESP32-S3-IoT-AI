// GET /api/stats?hours=24
import { query } from '../lib/db.js';
import { allowMethods, intParam, deviceParam, fail } from '../lib/http.js';

export default async function handler(req, res) {
  if (!allowMethods(req, res, ['GET'])) return;
  const hours = intParam(req.query.hours, 24, 1, 24 * 31);
  try {
    const [row] = await query(
      `with t as (select temp, chiller from telemetry
                   where created_at >= now() - make_interval(hours => $1) and ($2::text is null or device_id = $2)),
            e as (select type from events
                   where created_at >= now() - make_interval(hours => $1) and ($2::text is null or device_id = $2))
       select (select round(avg(temp)::numeric, 2)::float8 from t)                                    as temp_avg,
              (select round(min(temp)::numeric, 2)::float8 from t)                                     as temp_min,
              (select round(max(temp)::numeric, 2)::float8 from t)                                     as temp_max,
              (select round(100 * avg(case when chiller then 1 else 0 end)::numeric, 1)::float8 from t) as chiller_pct,
              (select count(*)::int from t)                                                            as samples,
              (select count(*)::int from e where type = 'feed')                                        as feeds,
              (select count(*)::int from e where type = 'alarm')                                       as alarms,
              (select max(created_at) from telemetry where ($2::text is null or device_id = $2))       as last_seen`,
      [hours, deviceParam(req.query.device)]);
    res.status(200).json({ hours, ...row });
  } catch (err) { fail(res, err); }
}
