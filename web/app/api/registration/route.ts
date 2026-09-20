// POST /api/registration {player_id, poll_secret}
// The game asks how its pending registration is doing. When a link approval has
// parked a token, it is handed over once and wiped.
import { decrypt, isUuid, safeEqual, sha256 } from "@/lib/crypto";
import { sql } from "@/lib/db";
import { clientIp, rateLimited } from "@/lib/ratelimit";
import { expired } from "@/lib/registration";
import { error, json } from "@/lib/auth";
import { ensureSchema } from "@/lib/schema";
import { accountSiblings } from "@/lib/siblings";

export const runtime = "nodejs";

export async function POST(req: Request) {
  await ensureSchema();
  let body: Record<string, unknown>;
  try { body = (await req.json()) as Record<string, unknown>; } catch { return error("invalid JSON"); }
  const playerId = body.player_id, secret = String(body.poll_secret ?? "");
  if (!isUuid(playerId) || !/^[A-Za-z0-9_-]{20,}$/.test(secret)) return error("bad request");
  if (await rateLimited(`poll:ip:${clientIp(req)}`, 600, 3600)) return error("too many requests", 429);

  const db = sql();
  const regs = await db`select id, status, poll_hash, token_enc, expires_at, display_name from registrations where player_id = ${playerId} and poll_hash is not null order by created_at desc limit 1`;
  if (regs.length === 0 || !safeEqual(sha256(secret), regs[0].poll_hash as string)) return error("unknown registration", 404);
  const reg = regs[0];
  let status = reg.status as string;
  if (status === "pending" && expired({ expires_at: reg.expires_at as string })) {
    await db`update registrations set status = 'expired' where id = ${reg.id}`;
    status = "expired";
  }
  if (status === "confirmed") {
    // The game already holds a token from /api/register; one is only parked here if
    // it somehow had none. What matters is that it is approved now.
    const token = reg.token_enc ? decrypt(reg.token_enc as string) : undefined;
    if (reg.token_enc) await db`update registrations set token_enc = null where id = ${reg.id}`;
    const [p] = await db`select account_id from players where id = ${playerId}`;
    const existing = await accountSiblings((p?.account_id as string | null) ?? null, playerId as string, (reg.display_name as string) ?? "");
    return json({ status, approved: true, ...(token ? { token } : {}), player_id: playerId, existing });
  }
  return json({ status, approved: false });
}
