// GET /api/feeds?days=7 – số lần cho ăn theo ngày (giờ Việt Nam)
import { query } from '../lib/db.js';
import { allowMethods, intParam, deviceParam, fail } from '../lib/http.js';

export default async function handler(req, res) {
  if (!allowMethods(req, res, ['GET'])) return;
  const days = intParam(req.query.days, 7, 1, 60);
  try {
    const rows = await query(
      `with d as (
         select generate_series((now() at time zone 'Asia/Ho_Chi_Minh')::date - ($1::int - 1),
                                (now() at time zone 'Asia/Ho_Chi_Minh')::date, interval '1 day')::date as day)
       select to_char(d.day, 'YYYY-MM-DD')                                        as day,
              count(e.created_at)::int                                            as feeds,
              count(e.created_at) filter (where e.detail ilike 'LICH%')::int      as scheduled,
              count(e.created_at) filter (where e.detail not ilike 'LICH%')::int  as manual
         from d
         left join events e
           on e.type = 'feed'
          and e.created_at >= (d.day::timestamp at time zone 'Asia/Ho_Chi_Minh')
          and e.created_at <  ((d.day + 1)::timestamp at time zone 'Asia/Ho_Chi_Minh')
          and ($2::text is null or e.device_id = $2)
        group by d.day order by d.day`,
      [days, deviceParam(req.query.device)]);
    res.status(200).json({ days, points: rows });
  } catch (err) { fail(res, err); }
}
