// Kết nối PostgreSQL (Neon). Tự tạo bảng, tự thêm cột mới khi nâng cấp từ bản cũ (relay -> MOSFET/PID).
import pg from 'pg';

const SCHEMA = `
create table if not exists telemetry (
  id          bigserial primary key,
  ts_device   timestamptz,
  ts_server   timestamptz not null default now(),
  device_id   text not null,
  seq         bigint,
  temp        real,
  mode        text,
  feed_today  int,
  feed_total  int,
  alarm       text,
  rssi        int,
  time_src    text,
  safe_mode   boolean,
  buffered    boolean default false,
  uptime_s    bigint
);
-- Cột mới cho bản PID + MOSFET (không làm mất dữ liệu cũ)
alter table telemetry add column if not exists sp     real;
alter table telemetry add column if not exists cool   real;   -- % PWM sò lạnh
alter table telemetry add column if not exists heat   real;   -- % PWM lõi sưởi
alter table telemetry add column if not exists pid_u  real;
alter table telemetry add column if not exists pid_p  real;
alter table telemetry add column if not exists pid_i  real;
alter table telemetry add column if not exists pid_d  real;
create index if not exists telemetry_time_idx on telemetry (ts_server desc);

create table if not exists events (
  id         bigserial primary key,
  ts_device  timestamptz,
  ts_server  timestamptz not null default now(),
  device_id  text not null,
  type       text not null,
  detail     text
);
create index if not exists events_time_idx on events (ts_server desc);
create index if not exists events_type_idx on events (type, ts_server desc);

create table if not exists commands (
  id         bigserial primary key,
  cmd_id     text unique not null,
  ts_sent    timestamptz not null default now(),
  source     text,
  payload    jsonb,
  ts_ack     timestamptz,
  ack_ok     boolean,
  ack_msg    text,
  latency_ms int
);
create index if not exists commands_time_idx on commands (ts_sent desc);

create table if not exists availability (
  id        bigserial primary key,
  ts_server timestamptz not null default now(),
  device_id text,
  status    text not null
);
create index if not exists availability_time_idx on availability (ts_server desc);

create table if not exists settings (
  k text primary key,
  v text not null
);
insert into settings (k, v) values
  ('tariff_vnd_per_kwh', '3000'), ('cool_w', '60'), ('heat_w', '10')
on conflict (k) do nothing;
`;

let pool;
let ready;

export function getPool() {
  if (!pool) {
    const connectionString = process.env.DATABASE_URL || process.env.POSTGRES_URL;
    if (!connectionString) throw new Error('Thiếu DATABASE_URL (chuỗi kết nối Neon)');
    pool = new pg.Pool({
      connectionString,
      max: 5,
      idleTimeoutMillis: 30_000,
      connectionTimeoutMillis: 10_000,
      ssl: /localhost|127\.0\.0\.1|host=/.test(connectionString) ? false : { rejectUnauthorized: false },
    });
  }
  return pool;
}

export async function initDb() {
  if (!ready) ready = getPool().query(SCHEMA).then(() => true).catch(e => { ready = undefined; throw e; });
  return ready;
}

export async function q(text, params = []) {
  await initDb();
  const { rows } = await getPool().query(text, params);
  return rows;
}

export const SETTING_KEYS = ['tariff_vnd_per_kwh', 'cool_w', 'heat_w'];

export async function getSettings() {
  const rows = await q('select k, v from settings');
  const s = Object.fromEntries(rows.map(r => [r.k, Number(r.v)]));
  return {
    tariff_vnd_per_kwh: s.tariff_vnd_per_kwh ?? 3000,
    cool_w: s.cool_w ?? 60,
    heat_w: s.heat_w ?? 10,
  };
}

export async function setSetting(k, v) {
  await q('insert into settings (k, v) values ($1, $2) on conflict (k) do update set v = excluded.v', [k, String(v)]);
}

// Thời điểm của 1 bản ghi: giờ thiết bị (đúng cả với bản ghi gửi bù), không có thì giờ server.
export const TS = 'coalesce(ts_device, ts_server)';

/** Điện năng ước tính trong N giờ: Σ (cool%·Psò + heat%·Psưởi) · Δt, Δt tối đa 120 s. */
export async function energy(hours) {
  const s = await getSettings();
  const [r] = await q(
    `with x as (
       select coalesce(cool,0) cool, coalesce(heat,0) heat,
              least(extract(epoch from ${TS} - lag(${TS}) over (order by ${TS})), 120) dt
         from telemetry where ${TS} >= now() - make_interval(hours => $1))
     select coalesce(sum(cool/100.0 * $2 * coalesce(dt,0)), 0) / 3600.0 as cool_wh,
            coalesce(sum(heat/100.0 * $3 * coalesce(dt,0)), 0) / 3600.0 as heat_wh,
            coalesce(sum(case when cool > 0 then coalesce(dt,0) else 0 end), 0) as cool_s,
            coalesce(sum(case when heat > 0 then coalesce(dt,0) else 0 end), 0) as heat_s,
            coalesce(sum(coalesce(dt,0)), 0) as total_s
       from x`, [hours, s.cool_w, s.heat_w]);
  const cool_wh = Number(r.cool_wh), heat_wh = Number(r.heat_wh), wh = cool_wh + heat_wh;
  return { wh, cool_wh, heat_wh, cost: wh / 1000 * s.tariff_vnd_per_kwh,
           cool_s: Number(r.cool_s), heat_s: Number(r.heat_s), total_s: Number(r.total_s), settings: s };
}
