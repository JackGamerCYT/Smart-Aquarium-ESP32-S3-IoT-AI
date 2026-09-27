// MÔ PHỎNG ESP32 (không cần phần cứng) – dùng để test backend + web dashboard.
// Nói chuyện MQTT y hệt firmware v4.0.0: telemetry 2 s, event, ack, status (LWT), nhận command.
// Chạy:  node device_sim.js            (broker mặc định HiveMQ công cộng)
//        MQTT_URL=mqtt://localhost:1883 SPEEDUP=60 node device_sim.js
// SPEEDUP: tua nhanh mô hình nhiệt (60 = 1 giây thật bằng 1 phút nước nguội/ấm).
import mqtt from 'mqtt';

const TOPIC = process.env.MQTT_TOPIC_BASE || 'smartaquarium_node2026';
const URL = process.env.MQTT_URL || 'mqtt://broker.hivemq.com:1883';
const SPEEDUP = Number(process.env.SPEEDUP || 30);
const AMBIENT = Number(process.env.AMBIENT || 30);

const cfg = { sp: 26, kp: 60, ki: 0.01, kd: 0, db: 0.1, cool_max: 100, heat_max: 100 };
let T = Number(process.env.T0 || 28.5), mode = 'AUTO', manCool = 0, manHeat = 0, manualSince = 0;
let I = 0, P = 0, D = 0, U = 0, prevT = null, dF = 0, cool = 0, heat = 0;
let seq = 0, feedToday = 0, feedTotal = 12, schedule = '07:00,17:30', alarm = 'NONE', feeding = false;
const C = 4186 * 4.5, UA = 1.5, COOL_W = 20, HEAT_W = 8;

const c = mqtt.connect(URL, { clientId: 'aq-sim-' + Math.random().toString(16).slice(2, 8),
  will: { topic: `${TOPIC}/status`, payload: 'offline', retain: true, qos: 1 } });
const pub = (k, o, opt) => c.publish(`${TOPIC}/${k}`, typeof o === 'string' ? o : JSON.stringify(o), opt);
const event = (type, detail) => pub('event', { type, detail, ts: Date.now() });

c.on('connect', () => {
  console.log(`[SIM] MQTT ${URL} topic ${TOPIC}/*  (tua nhanh x${SPEEDUP})`);
  pub('status', 'online', { retain: true, qos: 1 });
  c.subscribe(`${TOPIC}/command`, { qos: 1 });
  event('boot', 'FW 4.0.0-sim online');
});

function pidStep(dt) {
  const e = cfg.sp - T;
  const eDb = Math.abs(e) <= cfg.db ? 0 : (e > 0 ? e - cfg.db : e + cfg.db);
  P = cfg.kp * eDb;
  const dRaw = prevT === null ? 0 : -(T - prevT) / dt; prevT = T;
  dF = 0.7 * dF + 0.3 * dRaw; D = cfg.kd * dF;
  const iNew = Math.min(Math.max(I + (Math.abs(e) < 2 ? cfg.ki * eDb * dt : 0), -cfg.cool_max), cfg.heat_max);
  const uTry = P + D + iNew;
  if (!((uTry > cfg.heat_max && eDb > 0) || (uTry < -cfg.cool_max && eDb < 0))) I = iNew;
  U = Math.min(Math.max(P + D + I, -cfg.cool_max), cfg.heat_max);
  return U;
}
const resetPid = () => { I = P = D = U = 0; prevT = null; dF = 0; };

// vòng điều khiển 1 s (thời gian mô hình = 1 s * SPEEDUP)
setInterval(() => {
  const dt = SPEEDUP;
  let ct = 0, ht = 0;
  if (mode === 'MANUAL' && Date.now() - manualSince > 30 * 60e3) { mode = 'AUTO'; resetPid(); event('mode', 'MANUAL het 30 phut -> AUTO'); }
  if (mode === 'AUTO') { const u = pidStep(dt); if (u >= 3) ht = u; else if (u <= -3) ct = -u;
    if (T > cfg.sp + 1.5) ht = 0; if (T < cfg.sp - 1.5) ct = 0; }
  else if (mode === 'MANUAL') { ct = manCool; ht = manHeat; } else resetPid();
  if (T >= 31) { ht = 0; ct = cfg.cool_max; } if (T <= 20) { ct = 0; ht = cfg.heat_max; }
  cool = ct < cool ? ct : Math.min(ct, cool + 20); heat = ht < heat ? ht : Math.min(ht, heat + 20);
  const Q = heat / 100 * HEAT_W - cool / 100 * COOL_W + UA * (AMBIENT - T);
  T += Q * dt / C;
  const a = T >= 31 ? 'TEMP_HIGH' : T <= 20 ? 'TEMP_LOW' : 'NONE';
  if (a !== alarm) { alarm = a; event(a === 'NONE' ? 'recovered' : 'alarm', 'Canh bao: ' + a); }
}, 1000);

const now = () => new Date().toLocaleString('sv-SE', { timeZone: 'Asia/Ho_Chi_Minh' });
function telemetry() {
  const r1 = v => Math.round(v * 10) / 10;
  pub('telemetry', { dev: 'aq-SIM001', fw: '4.0.0-sim', seq: ++seq, ts: Date.now(),
    temp: Math.round((T + (Math.random() - .5) * 0.06) / 0.0625) * 0.0625, sp: cfg.sp, mode,
    cool: r1(cool), heat: r1(heat), pid_u: r1(U), pid_p: r1(P), pid_i: r1(I), pid_d: r1(D),
    kp: cfg.kp, ki: cfg.ki, kd: cfg.kd, db: cfg.db, cool_max: cfg.cool_max, heat_max: cfg.heat_max,
    man_cool: manCool, man_heat: manHeat,
    ...(mode === 'MANUAL' ? { manual_left_s: Math.round((30 * 60e3 - (Date.now() - manualSince)) / 1000) } : {}),
    feed_today: feedToday, feed_count: feedTotal, feeding, next_feed: '17:30', schedule,
    time: now(), time_src: 'NTP', alarm, ds_ok: true, oled: true, rssi: -58 - Math.round(Math.random() * 6),
    uptime: Math.round(process.uptime()), heap: 210000, buf_count: 0, safe_mode: false });
}
setInterval(telemetry, 2000);

c.on('message', (t, buf) => {
  let d; try { d = JSON.parse(buf.toString()); } catch { return; }
  const act = String(d.action || '').toUpperCase();
  let ok = true, msg = 'OK';
  switch (d.device) {
    case 'mode':
      if (!['AUTO', 'MANUAL', 'OFF'].includes(act)) { ok = false; msg = 'action: AUTO | MANUAL | OFF'; break; }
      if (act !== mode) { mode = act; resetPid(); if (mode === 'MANUAL') { manualSince = Date.now(); manCool = cool; manHeat = heat; } else manCool = manHeat = 0;
        event('mode', `${mode} (web)`); }
      msg = 'Che do ' + mode; break;
    case 'setpoint': {
      const v = Number(d.value);
      if (!(v >= 20 && v <= 32)) { ok = false; msg = 'Nhiet do dat phai 20-32 C'; break; }
      cfg.sp = v; msg = `Nhiet do dat ${v.toFixed(1)} C`; event('setpoint', msg); break; }
    case 'pid': {
      const n = { ...cfg };
      for (const k of ['kp', 'ki', 'kd', 'db', 'cool_max', 'heat_max']) if (d[k] !== undefined) n[k] = Number(d[k]);
      if (n.kp < 0 || n.kp > 500 || n.ki < 0 || n.ki > 10 || n.kd < 0 || n.kd > 5000 || n.db < 0 || n.db > 2 || n.cool_max < 0 || n.cool_max > 100 || n.heat_max < 0 || n.heat_max > 100) {
        ok = false; msg = 'Tham so ngoai gioi han'; break; }
      Object.assign(cfg, n); resetPid(); msg = `PID Kp=${cfg.kp} Ki=${cfg.ki} Kd=${cfg.kd} DB=${cfg.db}`; event('pid', msg); break; }
    case 'pid_reset': resetPid(); msg = 'Da xoa tich phan PID'; break;
    case 'cool': case 'heat': {
      const v = Number(d.value);
      if (!(v >= 0 && v <= 100)) { ok = false; msg = 'Gia tri 0-100 %'; break; }
      if (mode !== 'MANUAL') { mode = 'MANUAL'; resetPid(); event('mode', 'MANUAL (dat tay)'); }
      manualSince = Date.now();
      if (d.device === 'cool') { manCool = v; if (v > 0) manHeat = 0; } else { manHeat = v; if (v > 0) manCool = 0; }
      msg = `${d.device === 'cool' ? 'So lanh' : 'Loi suoi'} ${v}% (MANUAL 30 phut)`; break; }
    case 'feed':
      if (feeding) { ok = false; msg = 'Dang cho an'; break; }
      if (feedToday >= 6) { ok = false; msg = 'Vuot gioi han so lan/ngay'; break; }
      feeding = true; msg = 'Dang cho ca an';
      setTimeout(() => { feeding = false; feedToday++; feedTotal++; event('feed', `WEB - hom nay ${feedToday} lan`); }, 2500);
      break;
    case 'schedule':
      if (!/^$|^([01]\d|2[0-3]):[0-5]\d(,([01]\d|2[0-3]):[0-5]\d){0,3}$/.test(d.slots || '')) { ok = false; msg = 'Lich sai'; break; }
      schedule = d.slots; msg = 'Lich: ' + (schedule || 'TAT'); event('schedule', msg); break;
    case 'time': msg = 'Da dat gio ' + now(); event('time_sync', 'WEB ' + now()); break;
    case 'oled': msg = 'OLED ' + act; break;
    case 'ping': msg = 'pong'; break;
    default: ok = false; msg = 'Lenh khong ho tro: ' + d.device;
  }
  console.log(`[SIM] ${d.device} ${act || d.value || ''} -> ${ok ? 'OK' : 'FAIL'} ${msg}`);
  if (d.id) pub('ack', { id: d.id, device: d.device, ok, msg });
  telemetry();
});
