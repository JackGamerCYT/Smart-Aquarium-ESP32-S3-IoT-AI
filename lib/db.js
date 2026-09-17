import pg from 'pg';
import { SCHEMA_SQL } from './schema.js';

let pool;
let schemaReady;

/** Pool dùng chung trong một instance serverless (Vercel tái sử dụng giữa các request). */
function getPool() {
  if (!pool) {
    const connectionString = process.env.DATABASE_URL || process.env.POSTGRES_URL;
    if (!connectionString) throw new Error('Thiếu biến môi trường DATABASE_URL (kết nối Neon trong Vercel → Storage)');
    pool = new pg.Pool({ connectionString, max: 3, idleTimeoutMillis: 10_000, connectionTimeoutMillis: 8_000 });
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
