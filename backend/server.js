// Backend Smart Aquarium v5 (sò lạnh PID + bơm): MQTT ingest 24/7 + REST API + trợ lý dữ liệu.
// Chạy trên Render (free) hoặc trên máy: node server.js
import express from 'express';
import { q, initDb, getSettings, setSetting, SETTING_KEYS, TS, energy } from './db.js';
import { startIngest, state, publishCommand, deviceOnline, topicBase } from './ingest.js';
import { answer } from './chat.js';

const app = express();
const PORT = process.env.PORT || 8000;
const API_KEY = process.env.API_KEY || '';           // để trống = không cần khóa
const RETENTION_DAYS = Number(process.env.RETENTION_DAYS || 60);

app.use(express.json({ limit: '256kb' }));
app.use((req, res, next) => {                         // CORS cho dashboard trên Vercel
  res.set('Access-Control-Allow-Origin', '*');
  res.set('Access-Control-Allow-Headers', 'Content-Type, X-API-Key');
  res.set('Access-Control-Allow-Methods', 'GET, POST, PUT, OPTIONS');
  res.set('Cache-Control', 'no-store');
  if (req.method === 'OPTIONS') return res.sendStatus(204);
  if (API_KEY && req.method !== 'GET' && req.get('X-API-Key') !== API_KEY) return res.status(401).json({ error: 'Sai X-API-Key' });
  next();
});

const int = (v, d, lo, hi) => { const n = parseInt(v, 10); return Number.isFinite(n) ? Math.min(hi, Math.max(lo, n)) : d; };
const wrap = fn => (req, res) => fn(req, res).catch(e => { console.error(e); res.status(500).json({ error: e.message }); });

// ---------------------------------------------------------------- health / config
app.get('/api/health', wrap(async (req, res) => {
  let db = 'error', rows = null;
  try { const [r] = await q('select count(*)::int n, max(ts_server) last from telemetry'); db = 'ok'; rows = r; }
  catch (e) { db = e.message; }
  res.json({
    ok: db === 'ok', db, rows, topic: topicBase,
    mqtt_connected: state.mqttConnected, device: deviceOnline() ? 'online' : state.deviceStatus,
    received: state.received, stored: state.stored, missed_seq: state.missedSeq,
    uptime_s: Math.round(process.uptime()), now: new Date().toISOString(),
  });
}));

app.get('/api/config', wrap(async (req, res) => {
  res.json({
    mqtt: {
      url: process.env.MQTT_WS_URL || 'wss://broker.hivemq.com:8884/mqtt',
      topicBase, username: process.env.MQTT_WEB_USERNAME || '', password: process.env.MQTT_WEB_PASSWORD || '',
    },
    database: true, api_key_required: Boolean(API_KEY),
  });
}));

app.get('/api/status', wrap(async (req, res) => {
  const e = await energy(24);
  res.json({ telemetry: state.lastTelemetry, age_s: state.lastTelemetryAt ? (Date.now() - state.lastTelemetryAt) / 1000 : null,
    online: deviceOnline(), energy_24h_wh: e.wh, cost_24h_vnd: e.cost, settings: e.settings });
}));

// ---------------------------------------------------------------- lịch sử & thống kê
app.get('/api/history', wrap(async (req, res) => {
  const hours = int(req.query.hours, 24, 1, 24 * 31);
  const step = hours <= 6 ? '1 minute' : hours <= 48 ? '10 minutes' : '1 hour';
  const points = await q(
    `select date_bin($2::interval, ${TS}, timestamptz '2000-01-01') bucket,
            round(avg(temp)::numeric,2)::float8 temp_avg,
            round(min(temp)::numeric,2)::float8 temp_min,
            round(max(temp)::numeric,2)::float8 temp_max,
            round(avg(sp)::numeric,2)::float8 sp,
            round(avg(coalesce(cool,0))::numeric,1)::float8 cool,
            round((100.0 * avg(case when pump then 1 when pump is null then null else 0 end))::numeric,0)::float8 pump,
            count(*)::int samples
       from telemetry where ${TS} >= now() - make_interval(hours => $1)
      group by 1 order by 1`, [hours, step]);
  res.json({ hours, step, points });
}));

app.get('/api/stats', wrap(async (req, res) => {
  const hours = int(req.query.hours, 24, 1, 24 * 31);
  const [t] = await q(
    `select round(avg(temp)::numeric,2)::float8 temp_avg, round(min(temp)::numeric,2)::float8 temp_min,
            round(max(temp)::numeric,2)::float8 temp_max,
            round((avg(abs(temp - sp)) filter (where mode = 'AUTO'))::numeric,3)::float8 track_err_avg,
            round((max(abs(temp - sp)) filter (where mode = 'AUTO'))::numeric,2)::float8 track_err_max,
            round((100.0 * avg(case when abs(temp - sp) <= 0.5 then 1 else 0 end) filter (where mode = 'AUTO'))::numeric,1)::float8 in_band_pct,
            round(avg(coalesce(cool,0))::numeric,1)::float8 cool_avg,
            round((100.0 * avg(case when pump then 1 when pump is null then null else 0 end))::numeric,1)::float8 pump_on_pct,
            count(*)::int samples, max(ts_server) last_seen,
            count(*) filter (where alarm is not null and alarm <> 'NONE')::int alarm_samples
       from telemetry where ${TS} >= now() - make_interval(hours => $1)`, [hours]);
  const [ev] = await q(
    `select count(*) filter (where type='feed')::int feeds, count(*) filter (where type='alarm')::int alarms,
            count(*) filter (where type in ('pid','setpoint','mode'))::int tuning_changes,
            count(*) filter (where type='pump')::int pump_switches
       from events where ts_server >= now() - make_interval(hours => $1)`, [hours]);
  const [cmd] = await q(
    `select count(*)::int total, count(ts_ack)::int acked,
            round(avg(latency_ms)::numeric,0)::int latency_avg_ms,
            percentile_disc(0.95) within group (order by latency_ms) latency_p95_ms,
            max(latency_ms) latency_max_ms
       from commands where ts_sent >= now() - make_interval(hours => $1)`, [hours]);
  const [av] = await q(
    `select count(*) filter (where status='offline')::int offline_count
       from availability where ts_server >= now() - make_interval(hours => $1)`, [hours]);
  const e = await energy(hours);
  res.json({ hours, temperature: t, events: ev, commands: cmd, availability: av,
    energy_wh: +e.wh.toFixed(2), cool_wh: +e.cool_wh.toFixed(2), pump_wh: +e.pump_wh.toFixed(2), cost_vnd: Math.round(e.cost),
    cool_runtime_min: Math.round(e.cool_s / 60), pump_runtime_min: Math.round(e.pump_s / 60), settings: e.settings,
    packets: { received: state.received, missed_seq: state.missedSeq,
      delivery_pct: state.received + state.missedSeq ? +(state.received / (state.received + state.missedSeq) * 100).toFixed(2) : null } });
}));

app.get('/api/feeds', wrap(async (req, res) => {
  const days = int(req.query.days, 7, 1, 60);
  const points = await q(
    `with dd as (select generate_series((now() at time zone 'Asia/Ho_Chi_Minh')::date - ($1::int - 1),
                                        (now() at time zone 'Asia/Ho_Chi_Minh')::date, interval '1 day')::date as d)
     select to_char(dd.d,'YYYY-MM-DD') as day, count(e.id)::int as feeds,
            count(e.id) filter (where e.detail ilike 'LICH%')::int as scheduled,
            count(e.id) filter (where e.detail not ilike 'LICH%')::int as manual
       from dd left join events e on e.type='feed'
            and e.ts_server >= (dd.d::timestamp at time zone 'Asia/Ho_Chi_Minh')
            and e.ts_server <  ((dd.d + 1)::timestamp at time zone 'Asia/Ho_Chi_Minh')
      group by dd.d order by dd.d`, [days]);
  res.json({ days, points });
}));

app.get('/api/energy/daily', wrap(async (req, res) => {
  const days = int(req.query.days, 7, 1, 60);
  const s = await getSettings();
  const points = await q(
    `with x as (
       select (${TS} at time zone 'Asia/Ho_Chi_Minh')::date as dcol, coalesce(cool,0) cool, case when pump then 1 else 0 end pump,
              least(extract(epoch from ${TS} - lag(${TS}) over (order by ${TS})), 120) dt
         from telemetry where ${TS} >= now() - make_interval(days => $1))
     select to_char(dcol,'YYYY-MM-DD') as day,
            round((sum(cool/100.0 * $2 * coalesce(dt,0))/3600.0)::numeric, 2)::float8 cool_wh,
            round((sum(pump * $3 * coalesce(dt,0))/3600.0)::numeric, 2)::float8 pump_wh
       from x group by dcol order by dcol`, [days, s.cool_w, s.pump_w]);
  res.json({ days, tariff: s.tariff_vnd_per_kwh, points: points.map(p => ({ ...p, wh: +(p.cool_wh + p.pump_wh).toFixed(2),
    cost_vnd: Math.round((p.cool_wh + p.pump_wh) / 1000 * s.tariff_vnd_per_kwh) })) });
}));

app.get('/api/events', wrap(async (req, res) => {
  const limit = int(req.query.limit, 100, 1, 500);
  const type = /^[\w-]{1,24}$/.test(req.query.type || '') ? req.query.type : null;
  res.json({ events: await q(`select ts_server, ts_device, type, detail from events
                               where ($1::text is null or type = $1) order by ts_server desc limit $2`, [type, limit]) });
}));

app.get('/api/commands', wrap(async (req, res) => {
  const limit = int(req.query.limit, 50, 1, 300);
  res.json({ commands: await q(`select cmd_id, ts_sent, source, payload, ts_ack, ack_ok, ack_msg, latency_ms
                                  from commands order by ts_sent desc limit $1`, [limit]) });
}));

app.get('/api/availability', wrap(async (req, res) => {
  const limit = int(req.query.limit, 50, 1, 300);
  res.json({ availability: await q('select ts_server, status from availability order by ts_server desc limit $1', [limit]) });
}));

// ---------------------------------------------------------------- điều khiển & cấu hình
const CMD_DEVICES = ['mode', 'setpoint', 'pid', 'pid_reset', 'cool', 'pump', 'feed', 'schedule', 'time', 'oled', 'ping'];
const CMD_FIELDS = ['action', 'value', 'kp', 'ki', 'kd', 'db', 'cool_max', 'heater_sp', 'cool_need_pump', 'slots', 'epoch'];
app.post('/api/command', wrap(async (req, res) => {
  const b = req.body || {};
  if (!CMD_DEVICES.includes(b.device)) return res.status(400).json({ error: `"device" phải là: ${CMD_DEVICES.join(', ')}` });
  const body = { device: b.device };
  for (const k of CMD_FIELDS) if (b[k] !== undefined) body[k] = b[k];
  res.json(await publishCommand(body, Math.min(10, Number(b.wait_ack_s) || 0)));
}));

app.get('/api/settings', wrap(async (req, res) => res.json(await getSettings())));
app.put('/api/settings', wrap(async (req, res) => {
  for (const k of SETTING_KEYS) {
    const v = Number(req.body?.[k]);
    if (Number.isFinite(v) && v >= 0 && v < 1e6) await setSetting(k, v);
  }
  res.json(await getSettings());
}));

// ---------------------------------------------------------------- CSV & chatbot
const CSV = {
  telemetry: ['ts_server', 'ts_device', 'device_id', 'seq', 'temp', 'sp', 'mode', 'cool', 'pump',
              'pid_u', 'pid_p', 'pid_i', 'pid_d', 'feed_today', 'alarm', 'rssi', 'safe_mode', 'buffered'],
  events: ['ts_server', 'ts_device', 'type', 'detail'],
  commands: ['ts_sent', 'source', 'cmd_id', 'payload', 'ts_ack', 'ack_ok', 'ack_msg', 'latency_ms'],
  availability: ['ts_server', 'status'],
};
app.get('/api/export/:table.csv', wrap(async (req, res) => {
  const cols = CSV[req.params.table];
  if (!cols) return res.status(404).json({ error: 'bảng không hỗ trợ' });
  const hours = int(req.query.hours, 24, 1, 24 * 90);
  const tcol = req.params.table === 'commands' ? 'ts_sent' : 'ts_server';
  const rows = await q(`select ${cols.join(',')} from ${req.params.table}
                         where ${tcol} >= now() - make_interval(hours => $1) order by ${tcol} limit 100000`, [hours]);
  const cell = v => v === null || v === undefined ? '' : v instanceof Date ? v.toISOString() : typeof v === 'object' ? JSON.stringify(v) : String(v);
  const csv = [cols.join(','), ...rows.map(r => cols.map(c => {
    const s = cell(r[c]); return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  }).join(','))].join('\n');
  res.type('text/csv; charset=utf-8').set('Content-Disposition', `attachment; filename="${req.params.table}_${hours}h.csv"`).send('﻿' + csv);
}));

app.post('/api/chat', wrap(async (req, res) => res.json(await answer(req.body?.question || ''))));

app.get('/', (req, res) => res.json({ service: 'smart-aquarium-backend', version: '5.0', docs: '/api/health' }));

// ---------------------------------------------------------------- dọn dữ liệu cũ
async function purge() {
  try {
    await q('delete from telemetry where ts_server < now() - make_interval(days => $1)', [RETENTION_DAYS]);
    await q('delete from events where ts_server < now() - make_interval(days => $1)', [RETENTION_DAYS * 3]);
  } catch (e) { console.error('[PURGE]', e.message); }
}

const server = app.listen(PORT, () => console.log(`[HTTP] listening on ${PORT}`));
if (process.env.NO_MQTT !== '1') {
  initDb().then(() => { console.log('[DB] sẵn sàng'); startIngest(); setInterval(purge, 6 * 3600 * 1000); })
    .catch(e => { console.error('[DB] LỖI:', e.message); startIngest(); });
} else initDb().then(() => console.log('[DB] sẵn sàng (NO_MQTT)')).catch(e => console.error('[DB]', e.message));

export { app, server };
