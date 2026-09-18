// Fixed-window counters in the database (no extra service). `limit` hits per
// `windowSeconds` for a key such as "register:<ip>" or "register:<email hash>".
import { sql } from "./db";

export async function rateLimited(key: string, limit: number, windowSeconds: number): Promise<boolean> {
  const windowStart = new Date(Math.floor(Date.now() / (windowSeconds * 1000)) * windowSeconds * 1000).toISOString();
  const rows = await sql()`insert into rate_limits (key, window_start, count) values (${key}, ${windowStart}, 1)
    on conflict (key, window_start) do update set count = rate_limits.count + 1 returning count`;
  const count = Number(rows[0].count);
  if (count === 1) {
    // Occasional cleanup of old windows.
    if (Math.random() < 0.05) await sql()`delete from rate_limits where window_start < now() - interval '2 days'`;
  }
  return count > limit;
}

export function clientIp(req: Request): string {
  const fwd = req.headers.get("x-forwarded-for") ?? req.headers.get("x-real-ip") ?? "";
  return fwd.split(",")[0].trim() || "unknown";
}
