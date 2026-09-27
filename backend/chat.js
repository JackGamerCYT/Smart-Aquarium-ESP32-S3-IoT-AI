// Trợ lý dữ liệu: KHÔNG gọi AI bên ngoài. Nhận diện ý định bằng biểu thức chính quy,
// truy vấn thẳng Postgres rồi ghép câu trả lời từ số liệu thật (không bịa số).
import { q, energy, TS } from './db.js';
import { state, deviceOnline } from './ingest.js';

const norm = s => (s || '').toLowerCase()
  .normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/đ/g, 'd').replace(/\s+/g, ' ').trim();

const f = (v, d = 1) => (v === null || v === undefined || isNaN(v) ? '--' : Number(v).toFixed(d));
const vnTime = t => new Date(t).toLocaleString('vi-VN', { timeZone: 'Asia/Ho_Chi_Minh', hour12: false });
const numIn = t => { const m = t.match(/(\d+(?:[.,]\d+)?)/); return m ? Number(m[1].replace(',', '.')) : null; };

function hoursFrom(text) {
  if (/7 ngay|tuan/.test(text)) return 24 * 7;
  if (/hom qua/.test(text)) return 48;
  if (/24|hom nay|ngay nay/.test(text)) return 24;
  const m = text.match(/(\d+)\s*gio/);
  if (m) return Math.min(24 * 31, Number(m[1]));
  return 24;
}

export async function answer(question) {
  const t = norm(question);
  if (!t) return { answer: 'Bạn muốn hỏi gì về bể cá?' };

  const isQuestion = /(may lan|bao nhieu|khi nao|luc nao|the nao|bao lau|lich su|hom qua|7 ngay|tuan|gan day|thong ke|\?)/.test(t);

  // ---- Lệnh điều khiển: chỉ trả về "action", web phải bấm xác nhận mới gửi ----
  if (!isQuestion) {
    if (/(cho ca an|cho an)/.test(t)) return { answer: 'Cho cá ăn ngay bây giờ?', action: { device: 'feed' } };
    if (/(dat|chinh|giu).*(nhiet|do)/.test(t) && numIn(t) !== null) {
      const v = numIn(t);
      if (v < 20 || v > 32) return { answer: 'Nhiệt độ đặt phải trong khoảng 20–32 °C.' };
      return { answer: `Đặt nhiệt độ mong muốn ${f(v)} °C?`, action: { device: 'setpoint', value: v } };
    }
    if (/(che do )?(tu dong|auto)/.test(t)) return { answer: 'Chuyển sang chế độ AUTO (PID tự điều khiển)?', action: { device: 'mode', action: 'AUTO' } };
    if (/(tat het|tat he thong|che do off|dung dieu khien)/.test(t)) return { answer: 'Tắt cả sò lạnh và lõi sưởi (chế độ OFF)?', action: { device: 'mode', action: 'OFF' } };
    if (/(lam lanh|so lanh|lam mat)/.test(t) && numIn(t) !== null) {
      const v = Math.min(100, Math.max(0, numIn(t)));
      return { answer: `Chạy sò lạnh ${f(v, 0)}% (MANUAL 30 phút)?`, action: { device: 'cool', value: v } };
    }
    if (/(suoi|lam nong|lam am)/.test(t) && numIn(t) !== null) {
      const v = Math.min(100, Math.max(0, numIn(t)));
      return { answer: `Chạy lõi sưởi ${f(v, 0)}% (MANUAL 30 phút)?`, action: { device: 'heat', value: v } };
    }
  }

  const hours = hoursFrom(t);

  // ---- Trạng thái hiện tại ----
  if (/(trang thai|hien tai|bay gio|dang the nao|nhiet do)/.test(t) && !/(cao nhat|thap nhat|trung binh|lich su|sai so)/.test(t)) {
    const d = state.lastTelemetry;
    if (!d) return { answer: 'Chưa nhận được dữ liệu nào từ ESP32.' };
    return { answer: `Nhiệt độ ${d.temp == null ? 'LỖI cảm biến' : f(d.temp, 2) + '°C'} (đặt ${f(d.sp)}°C) · chế độ ${d.mode}`
      + ` · sò lạnh ${f(d.cool, 0)}% · lõi sưởi ${f(d.heat, 0)}% · PID u=${f(d.pid_u)}`
      + ` · cho ăn hôm nay ${d.feed_today ?? 0} lần, lần kế ${d.next_feed ?? '--'} · cảnh báo ${d.alarm ?? 'NONE'}`
      + ` · thiết bị ${deviceOnline() ? 'ONLINE' : 'OFFLINE'}` };
  }

  // ---- Chất lượng điều khiển PID ----
  if (/(sai so|bam|pid|on dinh|chinh xac)/.test(t)) {
    const [r] = await q(`select round(avg(abs(temp-sp))::numeric,3)::float8 e, round(max(abs(temp-sp))::numeric,2)::float8 m,
                                round(100.0*avg(case when abs(temp-sp)<=0.5 then 1 else 0 end)::numeric,1)::float8 band, count(*)::int n
                           from telemetry where mode='AUTO' and temp is not null and sp is not null
                            and ${TS} >= now() - make_interval(hours => $1)`, [hours]);
    if (!r.n) return { answer: `Chưa có dữ liệu chế độ AUTO trong ${hours} giờ qua.` };
    const d = state.lastTelemetry;
    return { answer: `Trong ${hours} giờ qua (AUTO): sai số trung bình ${f(r.e, 2)}°C, lớn nhất ${f(r.m, 2)}°C, `
      + `${f(r.band, 1)}% thời gian nằm trong ±0.5°C (${r.n} mẫu).`
      + (d ? ` Tham số hiện tại Kp=${d.kp} Ki=${d.ki} Kd=${d.kd}.` : '') };
  }

  // ---- Nhiệt độ min/max/trung bình ----
  if (/(cao nhat|thap nhat|trung binh|nong nhat|lanh nhat)/.test(t)) {
    const [r] = await q(`select round(avg(temp)::numeric,2)::float8 avg, round(min(temp)::numeric,2)::float8 min,
                                round(max(temp)::numeric,2)::float8 max, count(*)::int n
                           from telemetry where ${TS} >= now() - make_interval(hours => $1) and temp is not null`, [hours]);
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

  // ---- Sò lạnh / sưởi chạy bao lâu ----
  if (/(so lanh|lam lanh|lam mat|suoi|lam nong)/.test(t)) {
    const e = await energy(hours);
    const pc = e.total_s ? e.cool_s / e.total_s * 100 : 0, ph = e.total_s ? e.heat_s / e.total_s * 100 : 0;
    return { answer: `Trong ${hours} giờ qua: sò lạnh hoạt động ${f(e.cool_s / 60, 0)} phút (${f(pc, 1)}% thời gian, ${f(e.cool_wh, 1)} Wh), `
      + `lõi sưởi ${f(e.heat_s / 60, 0)} phút (${f(ph, 1)}%, ${f(e.heat_wh, 1)} Wh).` };
  }

  // ---- Điện năng / tiền ----
  if (/(dien nang|tieu thu|tien dien|chi phi|bao nhieu dien|wh|kwh)/.test(t)) {
    const e = await energy(hours);
    return { answer: `Ước tính ${hours} giờ qua tiêu thụ ${f(e.wh, 1)} Wh (sò ${f(e.cool_wh, 1)} Wh + sưởi ${f(e.heat_wh, 1)} Wh) ≈ ${f(e.cost, 0)} đ`
      + ` (đơn giá ${e.settings.tariff_vnd_per_kwh} đ/kWh; sò ${e.settings.cool_w} W, sưởi ${e.settings.heat_w} W, tính theo % PWM).` };
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
    const rows = await q(`select payload, ack_ok, latency_ms, ts_sent from commands order by ts_sent desc limit 5`);
    if (!rows.length) return { answer: 'Chưa có lệnh điều khiển nào được ghi nhận.' };
    return { answer: rows.map(r => `${vnTime(r.ts_sent)} ${r.payload?.device ?? '?'} ${r.payload?.action ?? r.payload?.value ?? ''} → `
      + (r.ack_ok === null ? 'chưa xác nhận' : (r.ack_ok ? 'OK' : 'FAIL') + ` sau ${r.latency_ms} ms`)).join(' · ') };
  }

  return {
    answer: 'Mình trả lời được: nhiệt độ hiện tại · cao nhất/thấp nhất · sai số PID · cho ăn mấy lần · sò/sưởi chạy bao lâu · '
      + 'điện năng và tiền điện · cảnh báo · offline lúc nào · lệnh gần đây. Hoặc ra lệnh: "cho cá ăn", "đặt nhiệt độ 26", '
      + '"chế độ tự động", "tắt hết", "làm lạnh 50%", "sưởi 30%".'
  };
}
