import crypto from 'node:crypto';

export function allowMethods(req, res, methods) {
  if (methods.includes(req.method)) return true;
  res.setHeader('Allow', methods.join(', '));
  res.status(405).json({ error: `Chỉ hỗ trợ ${methods.join(', ')}` });
  return false;
}

export function safeEqual(a, b) {
  const A = Buffer.from(String(a ?? ''));
  const B = Buffer.from(String(b ?? ''));
  return A.length > 0 && A.length === B.length && crypto.timingSafeEqual(A, B);
}

export function intParam(value, fallback, min, max) {
  const n = Number.parseInt(value, 10);
  return Number.isFinite(n) ? Math.min(max, Math.max(min, n)) : fallback;
}

export function deviceParam(value) {
  return typeof value === 'string' && /^[\w-]{3,40}$/.test(value) ? value : null;
}

export function fail(res, err) {
  console.error(err);
  res.status(500).json({ error: err?.message || 'Lỗi server' });
}
