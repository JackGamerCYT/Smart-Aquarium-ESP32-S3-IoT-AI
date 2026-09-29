// MÔ PHỎNG ESP32 v5 (không cần phần cứng) – dùng để test backend + web dashboard.
// Nói chuyện MQTT y hệt firmware 5.0.0: telemetry 2 s, event, ack, status (LWT), nhận command.
// Mô hình: sò lạnh PID một phía + bơm bật/tắt + module sưởi USB tự giữ nhiệt (bang-bang, trễ ±0.3 °C).
// Chạy:  node device_sim.js            (broker mặc định HiveMQ công cộng)
//        MQTT_URL=mqtt://localhost:1883 SPEEDUP=60 AMBIENT=30 node device_sim.js
import mqtt from 'mqtt';

const TOPIC = process.env.MQTT_TOPIC_BASE || 'smartaquarium_node2026';
const URL = process.env.MQTT_URL || 'mqtt://broker.hivemq.com:1883';
const SPEEDUP = Number(process.env.SPEEDUP || 30);
const AMBIENT = Number(process.env.AMBIENT || 30);
const GAP = 0.5;

const cfg = { sp: 27, kp: 60, ki: 0.01, kd: 0, db: 0.1, cool_max: 100, heater_sp: 26, cool_need_pump: true };
let T = Number(process.env.T0 || 28.5), mode = 'AUTO', manCool = 0, manualSince = 0, pumpOn = true, pumpPct = 100;
let I = 0, P = 0, D = 0, U = 0, prevT = null, dF = 0, cool = 0, heaterOn = false, coolBlock = '';
let seq = 0, feedToday = 0, feedTotal = 12, schedule = '07:00,17:30', alarm = 'NONE', feeding = false;
const C = 4186 * 4.5, UA = 1.5, COOL_W = 20, HEATER_W = 5, PUMP_HEAT_W = 1;   // bơm tỏa ~1 W vào nước

const c = mqtt.connect(URL, { clientId: 'aq-sim-' + Math.random().toString(16).slice(2, 8),
  will: { topic: `${TOPIC}/status`, payload: 'offline', retain: true, qos: 1 } });
const pub = (k, o, opt) => c.publish(`${TOPIC}/${k}`, typeof o === 'string' ? o : JSON.stringify(o), opt);
const event = (type, detail) => pub('event', { type, detail, ts: Date.now() });

c.on('connect', () => {
  console.log(`[SIM] MQTT ${URL} topic ${TOPIC}/*  (tua nhanh x${SPEEDUP})`);
  pub('status', 'online', { retain: true, qos: 1 });
  c.subscribe(`${TOPIC}/command`, { qos: 1 });
  event('boot', 'FW 5.0.0-sim online');
});

function pidStep(dt) {                     // giống firmware: u trong [-cool_max, 0]
  const e = cfg.sp - T;
  const eDb = Math.abs(e) <= cfg.db ? 0 : (e > 0 ? e - cfg.db : e + cfg.db);
  P = cfg.kp * eDb;
  const dRaw = prevT === null ? 0 : -(T - prevT) / dt; prevT = T;
  dF = 0.7 * dF + 0.3 * dRaw; D = cfg.kd * dF;
  const iNew = Math.min(Math.max(I + (Math.abs(e) < 2 ? cfg.ki * eDb * dt : 0), -cfg.cool_max), 0);
  const uTry = P + D + iNew;
  if (!((uTry > 0 && eDb > 0) || (uTry < -cfg.cool_max && eDb < 0))) I = iNew;
  U = Math.min(Math.max(P + D + I, -cfg.cool_max), 0);
  return U;
}
const resetPid = () => { I = P = D = U = 0; prevT = null; dF = 0; };

setInterval(() => {                       // vòng điều khiển 1 s thật = SPEEDUP s mô hình
  const dt = SPEEDUP;
  let ct = 0; coolBlock = '';
  if (mode === 'MANUAL' && Date.now() - manualSince > 30 * 60e3) { mode = 'AUTO'; resetPid(); event('mode', 'MANUAL het 30 phut -> AUTO'); }
  if (mode === 'OFF') { resetPid(); coolBlock = 'OFF'; }
  else if (cfg.cool_need_pump && !pumpOn) { resetPid(); coolBlock = 'PUMP_OFF'; }
  else {
    if (mode === 'AUTO') { const u = pidStep(dt); if (u <= -3) ct = -u; if (T < cfg.sp - 1.5) ct = 0; }
    else ct = manCool;
    if (T >= 31) ct = cfg.cool_max;
    if (T <= 20) { ct = 0; coolBlock = 'COLD'; }
  }
  cool = ct < cool ? ct : Math.min(ct, cool + 20);
  pumpPct = pumpOn ? 100 : 0;
  if (T < cfg.heater_sp - 0.3) heaterOn = true; else if (T > cfg.heater_sp + 0.3) heaterOn = false;   // thermostat của module USB
  const Q = (heaterOn ? HEATER_W : 0) + (pumpOn ? PUMP_HEAT_W : 0) - cool / 100 * COOL_W + UA * (AMBIENT - T);
  T += Q * dt / C;
  const a = T >= 31 ? 'TEMP_HIGH' : T <= 20 ? 'TEMP_LOW' : 'NONE';
  if (a !== alarm) { alarm = a; event(a === 'NONE' ? 'recovered' : 'alarm', 'Canh bao: ' + a); }
}, 1000);

const now = () => new Date().toLocaleString('sv-SE', { timeZone: 'Asia/Ho_Chi_Minh' });
function telemetry() {
  const r1 = v => Math.round(v * 10) / 10;
  pub('telemetry', { dev: 'aq-SIM001', fw: '5.0.0-sim', seq: ++seq, ts: Date.now(),
    temp: Math.round((T + (Math.random() - .5) * 0.06) / 0.0625) * 0.0625, sp: cfg.sp, mode,
    cool: r1(cool), pump: pumpOn, pump_pct: pumpPct, cool_block: coolBlock,
    pid_u: r1(U), pid_p: r1(P), pid_i: r1(I), pid_d: r1(D),
    kp: cfg.kp, ki: cfg.ki, kd: cfg.kd, db: cfg.db, cool_max: cfg.cool_max,
    heater_sp: cfg.heater_sp, sp_min: cfg.heater_sp + GAP, sp_max: 32, cool_need_pump: cfg.cool_need_pump,
    man_cool: manCool, sim_heater_on: heaterOn,
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
      if (act !== mode) { mode = act; resetPid(); if (mode === 'MANUAL') { manualSince = Date.now(); manCool = cool; } else manCool = 0;
        event('mode', `${mode} (web)`); }
      msg = 'Che do so lanh ' + mode; break;
    case 'setpoint': {
      const v = Number(d.value), lo = cfg.heater_sp + GAP;
      if (!(v >= lo - 0.001 && v <= 32)) { ok = false; msg = `Nhiet do dat phai ${lo.toFixed(1)}-32 C (cao hon nhiet suoi USB ${cfg.heater_sp.toFixed(1)} C)`; break; }
      cfg.sp = v; msg = `Nhiet do dat ${v.toFixed(1)} C`; event('setpoint', msg); break; }
    case 'pid': {
      const n = { ...cfg };
      for (const k of ['kp', 'ki', 'kd', 'db', 'cool_max', 'heater_sp']) if (d[k] !== undefined) n[k] = Number(d[k]);
      if (d.cool_need_pump !== undefined) n.cool_need_pump = !!d.cool_need_pump;
      if (n.kp < 0 || n.kp > 500 || n.ki < 0 || n.ki > 10 || n.kd < 0 || n.kd > 5000 || n.db < 0 || n.db > 2 ||
          n.cool_max < 0 || n.cool_max > 100 || n.heater_sp < 18 || n.heater_sp > 30) { ok = false; msg = 'Tham so ngoai gioi han'; break; }
      let raised = '';
      if (n.sp < n.heater_sp + GAP) { n.sp = n.heater_sp + GAP; raised = ' (da nang nhiet dat)'; }
      Object.assign(cfg, n); resetPid();
      msg = `PID Kp=${cfg.kp} Ki=${cfg.ki} Kd=${cfg.kd} DB=${cfg.db} suoi USB=${cfg.heater_sp}${raised}`; event('pid', msg); break; }
    case 'pid_reset': resetPid(); msg = 'Da xoa tich phan PID'; break;
    case 'cool': {
      const v = Number(d.value);
      if (!(v >= 0 && v <= 100)) { ok = false; msg = 'Gia tri 0-100 %'; break; }
      if (mode !== 'MANUAL') { mode = 'MANUAL'; resetPid(); event('mode', 'MANUAL (dat tay)'); }
      manualSince = Date.now(); manCool = v;
      msg = `So lanh ${v}% (MANUAL 30 phut)` + (cfg.cool_need_pump && !pumpOn ? ' - CANH BAO: bom dang TAT nen so dang bi khoa' : ''); break; }
    case 'pump': {
      const want = act === 'ON' ? true : act === 'OFF' ? false : act === 'TOGGLE' ? !pumpOn : null;
      if (want === null) { ok = false; msg = 'action: ON | OFF | TOGGLE'; break; }
      if (want !== pumpOn) { pumpOn = want; event('pump', `${pumpOn ? 'BAT' : 'TAT'} bom (web)`); }
      msg = 'Bom ' + (pumpOn ? 'BAT' : 'TAT') + (!pumpOn && cfg.cool_need_pump ? ' - so lanh tam khoa theo bom' : ''); break; }
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
