-- SMART AQUARIUM – PostgreSQL schema (Neon / Vercel Postgres)
-- API tự chạy file này (bản sao trong lib/schema.js) ở lần gọi đầu tiên → KHÔNG cần chạy tay.
-- Có thể chạy tay trong Neon Console → SQL Editor để kiểm tra.

create table if not exists telemetry (
  id          bigint generated always as identity primary key,
  created_at  timestamptz not null default now(),
  device_id   text        not null,
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
  time_src    text
);
create index if not exists telemetry_time_idx        on telemetry (created_at desc);
create index if not exists telemetry_device_time_idx on telemetry (device_id, created_at desc);

create table if not exists events (
  id          bigint generated always as identity primary key,
  created_at  timestamptz not null default now(),
  device_id   text        not null,
  type        text        not null,
  detail      text
);
create index if not exists events_time_idx      on events (created_at desc);
create index if not exists events_type_time_idx on events (type, created_at desc);
