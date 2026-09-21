// Worker MQTT: chạy 24/7 trên Render, nghe mọi topic của thiết bị và ghi vào Postgres.
import mqtt from 'mqtt';
import { q } from './db.js';

const TOPIC_BASE = process.env.MQTT_TOPIC_BASE || 'smartaquarium_node2026';
const MQTT_URL = process.env.MQTT_URL || 'mqtt://broker.hivemq.com:1883';
const MIN_STORE_MS = Number(process.env.INGEST_MIN_INTERVAL_MS || 10_000);   // gộp telemetry: lưu tối đa 1 bản ghi/10 s
const DEVICE_STALE_MS = 15_000;

export const state = {
  mqttConnected: false,
  lastTelemetry: null,
  lastTelemetryAt: 0,
  deviceStatus: 'unknown',
  stored: 0, received: 0, missedSeq: 0, lastSeq: null,
};

let client = null;
let lastStoredAt = 0;

const ts = v => (typeof v === 'number' && v > 1.6e12 ? new Date(v) : null);
const bool = v => (typeof v === 'boolean' ? v : null);
const num = v => (typeof v === 'number' && Number.isFinite(v) ? v : null);
const txt = (v, n) => (typeof v === 'string' && v ? v.slice(0, n) : null);

async function saveTelemetry(d) {
  await q(
    `insert into telemetry (ts_device, device_id, seq, temp, chiller, pump, fan, mode, feed_today, feed_total,
                            alarm, rtc_temp, rssi, time_src, safe_mode, buffered, uptime_s)
     values (coalesce($1, now()), $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17)`,
    [ts(d.ts), txt(d.dev, 40) || 'aquarium', num(d.seq), num(d.temp), bool(d.chiller), bool(d.pump), bool(d.fan),
     txt(d.mode, 8), num(d.feed_today), num(d.feed_count), txt(d.alarm, 20), num(d.rtc_temp), num(d.rssi),
     txt(d.time_src, 6), bool(d.safe_mode), !!d.buffered, num(d.uptime)]);
  state.stored++;
}

async function handle(topic, payload) {
  const kind = topic.split('/').pop();
  const text = payload.toString();

  if (kind === 'status') {
    const st = text.trim().toLowerCase();
    if (st !== state.deviceStatus) {
      state.deviceStatus = st;
      await q('insert into availability (device_id, status) values ($1, $2)', [TOPIC_BASE, st]);
    }
    return;
  }

  let d;
  try { d = JSON.parse(text); } catch { return; }

  if (kind === 'telemetry') {
    state.received++;
    state.lastTelemetry = d;
    state.lastTelemetryAt = Date.now();
    if (state.deviceStatus !== 'online') {
      state.deviceStatus = 'online';
      await q('insert into availability (device_id, status) values ($1, $2)', [TOPIC_BASE, 'online']);
    }
    if (typeof d.seq === 'number') {
      if (state.lastSeq !== null && d.seq > state.lastSeq + 1) state.missedSeq += d.seq - state.lastSeq - 1;
      state.lastSeq = d.seq;
    }
    const now = Date.now();
    if (d.buffered || now - lastStoredAt >= MIN_STORE_MS) { lastStoredAt = now; await saveTelemetry(d); }
    return;
  }

  if (kind === 'event') {
    await q('insert into events (ts_device, device_id, type, detail) values (coalesce($1, now()), $2, $3, $4)',
      [ts(d.ts), TOPIC_BASE, txt(d.type, 24) || 'event', txt(d.detail, 300)]);
    return;
  }

  if (kind === 'ack' && d.id) {
    // ESP32 có thể ACK nhanh hơn lúc lệnh của web kịp ghi vào bảng commands,
    // nên nếu chưa thấy dòng nào thì thử lại vài lần trước khi bỏ qua.
    for (let i = 0; i < 4; i++) {
      const rows = await q(
        `update commands set ts_ack = now(), ack_ok = $2, ack_msg = $3,
                             latency_ms = greatest(0, (extract(epoch from now() - ts_sent) * 1000)::int)
          where cmd_id = $1 and ts_ack is null returning cmd_id`,
        [String(d.id), !!d.ok, txt(d.msg, 200)]);
      if (rows.length) return;
      await new Promise(r => setTimeout(r, 500));
    }
    return;
  }

  if (kind === 'command' && d.id) {           // lệnh do web gửi: ghi lại để tính độ trễ
    await q(`insert into commands (cmd_id, source, payload) values ($1, $2, $3) on conflict (cmd_id) do nothing`,
      [String(d.id), txt(d.src, 16) || 'web', JSON.stringify(d)]);
  }
}

export function startIngest() {
  client = mqtt.connect(MQTT_URL, {
    clientId: 'aquarium-backend-' + Math.random().toString(16).slice(2, 8),
    username: process.env.MQTT_USER || undefined,
    password: process.env.MQTT_PASS || undefined,
    reconnectPeriod: 3000,
    keepalive: 30,
  });
  client.on('connect', () => {
    state.mqttConnected = true;
    client.subscribe(`${TOPIC_BASE}/#`, { qos: 1 });
    console.log(`[MQTT] connected ${MQTT_URL}, subscribed ${TOPIC_BASE}/#`);
  });
  client.on('close', () => { state.mqttConnected = false; });
  client.on('error', e => console.error('[MQTT]', e.message));
  client.on('message', (t, p) => handle(t, p).catch(e => console.error('[INGEST]', e.message)));
  return client;
}

/** REST publish lệnh xuống thiết bị và (tùy chọn) chờ ACK. */
export function publishCommand(body, waitAckSec = 0) {
  const id = 'api-' + Math.random().toString(36).slice(2, 10);
  const payload = { ...body, id, src: 'api' };
  return new Promise(async resolve => {
    if (!client || !state.mqttConnected) return resolve({ ok: false, id, error: 'backend chưa nối MQTT' });
    await q('insert into commands (cmd_id, source, payload) values ($1, $2, $3) on conflict (cmd_id) do nothing',
      [id, 'api', JSON.stringify(payload)]).catch(() => {});
    client.publish(`${TOPIC_BASE}/command`, JSON.stringify(payload), { qos: 1 });
    if (!waitAckSec) return resolve({ ok: true, id, confirmed: false });
    const t0 = Date.now();
    const onMsg = (topic, buf) => {
      if (!topic.endsWith('/ack')) return;
      try {
        const d = JSON.parse(buf.toString());
        if (d.id !== id) return;
        client.off('message', onMsg);
        resolve({ ok: true, id, confirmed: true, device_ok: !!d.ok, msg: d.msg, latency_ms: Date.now() - t0 });
      } catch {}
    };
    client.on('message', onMsg);
    setTimeout(() => { client.off('message', onMsg); resolve({ ok: true, id, confirmed: false }); }, waitAckSec * 1000);
  });
}

export function deviceOnline() {
  return state.deviceStatus === 'online' && Date.now() - state.lastTelemetryAt < DEVICE_STALE_MS;
}
export const topicBase = TOPIC_BASE;
