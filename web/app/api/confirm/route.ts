// POST /api/confirm {player_id, code}: completes the newest pending registration
// for the player with the emailed code and hands the game its per-player token.
import { isUuid, safeEqual, sha256 } from "@/lib/crypto";
import { sql } from "@/lib/db";
import { clientIp, rateLimited } from "@/lib/ratelimit";
import { completeRegistration, expired, type PendingRegistration } from "@/lib/registration";
import { error, json } from "@/lib/auth";
import { ensureSchema } from "@/lib/schema";
import { accountSiblings } from "@/lib/siblings";

export const runtime = "nodejs";

export async function POST(req: Request) {
  await ensureSchema();
  let body: Record<string, unknown>;
  try { body = (await req.json()) as Record<string, unknown>; } catch { return error("invalid JSON"); }
  const playerId = body.player_id, code = String(body.code ?? "");
  if (!isUuid(playerId)) return error("player_id must be a uuid");
  if (!/^\d{6}$/.test(code)) return error("the code has six digits");
  if (await rateLimited(`confirm:ip:${clientIp(req)}`, 60, 3600)) return error("too many attempts, try again later", 429);

  const db = sql();
  const regs = await db`select id, email_hash, email_enc, player_id, install_id, display_name, machine_label, code_hash, attempts, expires_at, status
    from registrations where player_id = ${playerId} and status in ('pending', 'confirmed') order by created_at desc limit 1`;
  if (regs.length === 0) return error("no registration is waiting for this player; ask for a new code", 404);
  const reg = regs[0];
  if (reg.status === "confirmed") return error("this registration was already approved; the game picks the result up by itself", 409);
  if (expired({ expires_at: reg.expires_at as string })) {
    await db`update registrations set status = 'expired' where id = ${reg.id}`;
    return error("that code has expired; ask for a new one", 410);
  }
  if (Number(reg.attempts) >= 6) {
    await db`update registrations set status = 'expired' where id = ${reg.id}`;
    return error("too many wrong codes; ask for a new one", 429);
  }
  if (!safeEqual(sha256(code), reg.code_hash as string)) {
    await db`update registrations set attempts = attempts + 1 where id = ${reg.id}`;
    return error("wrong code", 400);
  }
  const { token, accountId } = await completeRegistration(reg as unknown as PendingRegistration, "code");
  // Other players this address already has, so the game can offer to carry on as one of them.
  const existing = await accountSiblings(accountId, playerId as string, reg.display_name as string);
  return json({ ok: true, token, account_id: accountId, player_id: playerId, existing });
}
