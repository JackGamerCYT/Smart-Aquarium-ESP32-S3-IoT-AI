import pg from 'pg';
import { SCHEMA_SQL } from './schema.js';

let pool;
let schemaReady;

/** Tìm chuỗi kết nối Postgres: DATABASE_URL, POSTGRES_URL, hoặc biến có prefix tự đặt khi Connect (vd STORAGE_URL). */
export function findConnectionString() {
  const env = process.env;
  const preferred = ['DATABASE_URL', 'POSTGRES_URL', 'POSTGRES_PRISMA_URL', 'NEON_DATABASE_URL', 'DATABASE_URL_UNPOOLED', 'POSTGRES_URL_NON_POOLING'];
  for (const k of preferred) if (/^postgres(ql)?:\/\//.test(env[k] || '')) return { name: k, value: env[k] };
  const key = Object.keys(env).sort().find(k => /URL$/.test(k) && /^postgres(ql)?:\/\//.test(env[k] || ''));
  return key ? { name: key, value: env[key] } : null;
}

/** Pool dùng chung trong một instance serverless (Vercel tái sử dụng giữa các request). */
function getPool() {
  if (!pool) {
    const found = findConnectionString();
    if (!found) throw new Error('Thiếu biến môi trường DATABASE_URL: Vercel → Storage → Neon → Connect Project (tick Production) → Redeploy');
    pool = new pg.Pool({ connectionString: found.value, max: 3, idleTimeoutMillis: 10_000, connectionTimeoutMillis: 8_000 });
  }
  return pool;
}

/** Tự tạo bảng ở lần gọi đầu (CREATE ... IF NOT EXISTS nên chạy lại vẫn an toàn). */
function ensureSchema() {
  if (!schemaReady) {
    schemaReady = getPool().query(SCHEMA_SQL).catch(err => { schemaReady = undefined; throw err; });
  }
  return schemaReady;
}

/** Chạy câu SQL có tham số, trả về mảng dòng. */
export async function query(text, params = []) {
  await ensureSchema();
  const { rows } = await getPool().query(text, params);
  return rows;
}
