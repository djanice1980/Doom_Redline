// Player tokens (Bearer, hashed at rest) and the admin gate (HTTP Basic with
// ADMIN_PASSWORD; user name is ignored).
import { sql } from "./db";
import { safeEqual, sha256 } from "./crypto";

export interface PlayerAuth {
  playerId: string;
  displayName: string;
  accountId: string | null;
}

export async function authPlayer(req: Request): Promise<PlayerAuth | null> {
  const h = req.headers.get("authorization") ?? "";
  const m = /^Bearer\s+([A-Za-z0-9_-]{20,})$/.exec(h);
  if (!m) return null;
  const rows = await sql()`select id, display_name, account_id from players where token_hash = ${sha256(m[1])}`;
  if (rows.length === 0) return null;
  await sql()`update players set last_seen = now() where id = ${rows[0].id}`;
  return { playerId: rows[0].id as string, displayName: rows[0].display_name as string, accountId: (rows[0].account_id as string | null) ?? null };
}

export function isAdmin(req: Request): boolean {
  const want = process.env.ADMIN_PASSWORD;
  if (!want) return false;
  const h = req.headers.get("authorization") ?? "";
  if (!h.startsWith("Basic ")) return false;
  let decoded = "";
  try { decoded = Buffer.from(h.slice(6), "base64").toString("utf8"); } catch { return false; }
  const pass = decoded.includes(":") ? decoded.slice(decoded.indexOf(":") + 1) : decoded;
  return safeEqual(pass, want);
}

export function adminChallenge(): Response {
  return new Response("Authentication required", { status: 401, headers: { "WWW-Authenticate": 'Basic realm="REDLINE admin"' } });
}

export function json(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), { status, headers: { "Content-Type": "application/json", "Cache-Control": "no-store" } });
}

export function error(message: string, status = 400): Response {
  return json({ error: message }, status);
}
