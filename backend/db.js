// Kết nối PostgreSQL (Neon) hoặc SQLite-thay-thế: ở đây dùng Postgres cho cả local và Render.
import pg from 'pg';

const SCHEMA = `
create table if not exists telemetry (
  id          bigserial primary key,
  ts_device   timestamptz,
  ts_server   timestamptz not null default now(),
  device_id   text not null,
  seq         bigint,
  temp        real,
  chiller     boolean,
  pump        boolean,
  fan         boolean,
  mode        text,
  feed_today  int,
  feed_total  int,
  alarm       text,
  rtc_temp    real,
  rssi        int,
  time_src    text,
  safe_mode   boolean,
  buffered    boolean default false,
  uptime_s    bigint
);
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
  ('tariff_vnd_per_kwh', '3000'),
  ('chiller_w', '60'), ('pump_w', '5'), ('fan_w', '3')
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

export async function getSettings() {
  const rows = await q('select k, v from settings');
  const s = Object.fromEntries(rows.map(r => [r.k, Number(r.v)]));
  return {
    tariff_vnd_per_kwh: s.tariff_vnd_per_kwh ?? 3000,
    chiller_w: s.chiller_w ?? 60,
    pump_w: s.pump_w ?? 5,
    fan_w: s.fan_w ?? 3,
  };
}

export async function setSetting(k, v) {
  await q('insert into settings (k, v) values ($1, $2) on conflict (k) do update set v = excluded.v', [k, String(v)]);
}
