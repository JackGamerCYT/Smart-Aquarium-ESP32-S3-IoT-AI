// Trợ lý dữ liệu: KHÔNG gọi AI bên ngoài. Nhận diện ý định bằng biểu thức chính quy,
// truy vấn thẳng Postgres rồi ghép câu trả lời từ số liệu thật (không bịa số).
import { q, getSettings } from './db.js';
import { state, deviceOnline } from './ingest.js';

const norm = s => (s || '').toLowerCase()
  .normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/đ/g, 'd').replace(/\s+/g, ' ').trim();

const f = (v, d = 1) => (v === null || v === undefined || isNaN(v) ? '--' : Number(v).toFixed(d));
const vnTime = t => new Date(t).toLocaleString('vi-VN', { timeZone: 'Asia/Ho_Chi_Minh', hour12: false });

function hoursFrom(text) {
  if (/7 ngay|tuan/.test(text)) return 24 * 7;
  if (/hom qua/.test(text)) return 48;
  if (/24|hom nay|ngay nay/.test(text)) return 24;
  if (/(\d+)\s*gio/.test(text)) return Math.min(24 * 31, Number(RegExp.$1));
  return 24;
}

async function energy(hours) {
  const s = await getSettings();
  const [r] = await q(
    `with x as (
       select ts_server, chiller, pump, fan,
              least(extract(epoch from ts_server - lag(ts_server) over (order by ts_server)), 120) as dt
         from telemetry where ts_server >= now() - make_interval(hours => $1))
     select coalesce(sum((case when chiller then $2 else 0 end + case when pump then $3 else 0 end
                        + case when fan then $4 else 0 end) * coalesce(dt, 0)), 0) / 3600.0 as wh,
            coalesce(sum(case when chiller then coalesce(dt,0) else 0 end), 0) as chiller_s,
            coalesce(sum(coalesce(dt,0)), 0) as total_s
       from x`, [hours, s.chiller_w, s.pump_w, s.fan_w]);
  const wh = Number(r.wh);
  return { wh, cost: wh / 1000 * s.tariff_vnd_per_kwh, chiller_s: Number(r.chiller_s), total_s: Number(r.total_s), settings: s };
}

export async function answer(question) {
  const t = norm(question);
  if (!t) return { answer: 'Bạn muốn hỏi gì về bể cá?' };

  const isQuestion = /(may lan|bao nhieu|khi nao|luc nao|the nao|bao lau|lich su|hom nay|hom qua|7 ngay|tuan|gan day|thong ke|\?)/.test(t);

  // ---- Lệnh điều khiển: chỉ trả về "action", web phải bấm xác nhận mới gửi ----
  if (!isQuestion && /(cho ca an|cho an)/.test(t)) return { answer: 'Bạn muốn cho cá ăn ngay bây giờ?', action: { device: 'feed', action: 'ON' } };
  if (!isQuestion && /(bat|mo).*(so|lanh|chiller)/.test(t)) return { answer: 'Bật sò lạnh (chuyển MANUAL 30 phút)?', action: { device: 'chiller', action: 'ON' } };
  if (!isQuestion && /(tat).*(so|lanh|chiller)/.test(t)) return { answer: 'Tắt sò lạnh?', action: { device: 'chiller', action: 'OFF' } };
  if (!isQuestion && /(bat|mo).*(bom)/.test(t)) return { answer: 'Bật máy bơm?', action: { device: 'pump', action: 'ON' } };
  if (!isQuestion && /(tat).*(bom)/.test(t)) return { answer: 'Tắt máy bơm? Lưu ý tắt bơm sẽ khóa sò lạnh.', action: { device: 'pump', action: 'OFF' } };

  const hours = hoursFrom(t);

  // ---- Trạng thái hiện tại ----
  if (/(trang thai|hien tai|bay gio|dang the nao|nhiet do)/.test(t) && !/(cao nhat|thap nhat|trung binh|lich su)/.test(t)) {
    const d = state.lastTelemetry;
    if (!d) return { answer: 'Chưa nhận được dữ liệu nào từ ESP32.' };
    return { answer: `Nhiệt độ ${f(d.temp)}°C · sò lạnh ${d.chiller ? 'ON' : 'OFF'} · bơm ${d.pump ? 'ON' : 'OFF'} · quạt ${d.fan ? 'ON' : 'OFF'} · chế độ ${d.mode}`
      + ` · cho ăn hôm nay ${d.feed_today ?? 0} lần · lần kế tiếp ${d.next_feed ?? '--'} · cảnh báo ${d.alarm ?? 'NONE'}`
      + ` · thiết bị ${deviceOnline() ? 'ONLINE' : 'OFFLINE'}` };
  }

  // ---- Nhiệt độ min/max/trung bình ----
  if (/(cao nhat|thap nhat|trung binh|nong nhat|lanh nhat)/.test(t)) {
    const [r] = await q(`select round(avg(temp)::numeric,2)::float8 avg, round(min(temp)::numeric,2)::float8 min,
                                round(max(temp)::numeric,2)::float8 max, count(*)::int n
                           from telemetry where ts_server >= now() - make_interval(hours => $1) and temp is not null`, [hours]);
    if (!r.n) return { answer: `Chưa có dữ liệu nhiệt độ trong ${hours} giờ qua.` };
    return { answer: `Trong ${hours} giờ qua: trung bình ${f(r.avg, 2)}°C, thấp nhất ${f(r.min, 2)}°C, cao nhất ${f(r.max, 2)}°C (${r.n} mẫu).` };
  }

  // ---- Cho ăn ----
  if (/(cho an|an may lan|so lan an|lich cho an)/.test(t)) {
    const rows = await q(`select to_char(ts_server at time zone 'Asia/Ho_Chi_Minh', 'DD/MM') d, count(*)::int n
                            from events where type = 'feed' and ts_server >= now() - make_interval(hours => $1)
                           group by 1 order by min(ts_server)`, [hours]);
    const [last] = await q(`select ts_server, detail from events where type = 'feed' order by ts_server desc limit 1`);
    const tot = rows.reduce((a, r) => a + r.n, 0);
    return { answer: `Trong ${hours} giờ qua cá được cho ăn ${tot} lần` + (rows.length ? ` (${rows.map(r => `${r.d}: ${r.n}`).join(', ')})` : '')
      + (last ? `. Lần gần nhất ${vnTime(last.ts_server)} – ${last.detail}` : '.') };
  }

  // ---- Sò lạnh chạy bao lâu ----
  if (/(so lanh|chiller|lam mat|quat)/.test(t)) {
    const e = await energy(hours);
    const pct = e.total_s ? e.chiller_s / e.total_s * 100 : 0;
    return { answer: `Trong ${hours} giờ qua sò lạnh chạy ${f(e.chiller_s / 60, 0)} phút, chiếm ${f(pct, 1)}% thời gian.` };
  }

  // ---- Điện năng / tiền ----
  if (/(dien nang|tieu thu|tien dien|chi phi|bao nhieu dien|wh|kwh)/.test(t)) {
    const e = await energy(hours);
    return { answer: `Ước tính ${hours} giờ qua tiêu thụ ${f(e.wh, 1)} Wh ≈ ${f(e.cost, 0)} đ`
      + ` (đơn giá ${e.settings.tariff_vnd_per_kwh} đ/kWh; sò ${e.settings.chiller_w} W, bơm ${e.settings.pump_w} W, quạt ${e.settings.fan_w} W).` };
  }

  // ---- Cảnh báo ----
  if (/(canh bao|alarm|su co|loi)/.test(t)) {
    const rows = await q(`select ts_server, detail from events where type in ('alarm','error')
                           and ts_server >= now() - make_interval(hours => $1) order by ts_server desc limit 5`, [hours]);
    if (!rows.length) return { answer: `Không có cảnh báo nào trong ${hours} giờ qua.` };
    return { answer: `${rows.length} cảnh báo gần đây: ` + rows.map(r => `${vnTime(r.ts_server)} – ${r.detail}`).join(' · ') };
  }

  // ---- Mất kết nối ----
  if (/(offline|mat ket noi|mat mang|ngat)/.test(t)) {
    const rows = await q(`select ts_server, status from availability where ts_server >= now() - make_interval(hours => $1)
                           order by ts_server desc limit 10`, [hours]);
    if (!rows.length) return { answer: `Không ghi nhận thay đổi kết nối trong ${hours} giờ qua.` };
    return { answer: rows.map(r => `${vnTime(r.ts_server)}: ${r.status}`).join(' · ') };
  }

  // ---- Lệnh gần đây ----
  if (/(lenh|dieu khien|command)/.test(t)) {
    const rows = await q(`select cmd_id, source, payload, ack_ok, latency_ms, ts_sent from commands
                           order by ts_sent desc limit 5`);
    if (!rows.length) return { answer: 'Chưa có lệnh điều khiển nào được ghi nhận.' };
    return { answer: rows.map(r => `${vnTime(r.ts_sent)} ${r.payload?.device ?? '?'} ${r.payload?.action ?? ''} → `
      + (r.ack_ok === null ? 'chưa xác nhận' : (r.ack_ok ? 'OK' : 'FAIL') + ` sau ${r.latency_ms} ms`)).join(' · ') };
  }

  return {
    answer: 'Mình trả lời được: nhiệt độ hiện tại · nhiệt độ cao nhất/thấp nhất · cho ăn mấy lần · sò lạnh chạy bao lâu · '
      + 'điện năng và tiền điện · cảnh báo · thiết bị offline lúc nào · lệnh gần đây. Hoặc ra lệnh: cho cá ăn, bật/tắt sò, bật/tắt bơm.'
  };
}

export { energy };
