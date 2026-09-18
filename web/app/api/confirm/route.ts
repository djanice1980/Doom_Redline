// POST /api/confirm {player_id, code}
// Completes the newest pending registration for the player: creates the
// account (by email hash) if needed, attaches the player and the machine to it,
// and hands the game its per-player token.
import { encrypt, isUuid, newToken, safeEqual, sha256 } from "@/lib/crypto";
import { sql } from "@/lib/db";
import { clientIp, rateLimited } from "@/lib/ratelimit";
import { error, json } from "@/lib/auth";

export const runtime = "nodejs";

export async function POST(req: Request) {
  let body: Record<string, unknown>;
  try { body = (await req.json()) as Record<string, unknown>; } catch { return error("invalid JSON"); }
  const playerId = body.player_id, code = String(body.code ?? "");
  if (!isUuid(playerId)) return error("player_id must be a uuid");
  if (!/^\d{6}$/.test(code)) return error("the code has six digits");
  if (await rateLimited(`confirm:ip:${clientIp(req)}`, 60, 3600)) return error("too many attempts, try again later", 429);

  const db = sql();
  const regs = await db`select id, email_hash, email_enc, install_id, display_name, machine_label, code_hash, attempts, expires_at
    from registrations where player_id = ${playerId} and status = 'pending' order by created_at desc limit 1`;
  if (regs.length === 0) return error("no registration is waiting for this player; ask for a new code", 404);
  const reg = regs[0];
  if (new Date(reg.expires_at as string).getTime() < Date.now()) {
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

  const token = newToken();
  const result = await db.begin(async (tx) => {
    const acc = await tx`insert into accounts (email_hash, email_enc) values (${reg.email_hash}, ${reg.email_enc})
      on conflict (email_hash) do update set email_hash = excluded.email_hash returning id`;
    const accountId = acc[0].id as string;
    await tx`insert into players (id, account_id, display_name, token_hash, last_seen) values (${playerId}, ${accountId}, ${reg.display_name}, ${sha256(token)}, now())
      on conflict (id) do update set account_id = excluded.account_id, display_name = excluded.display_name, token_hash = excluded.token_hash, last_seen = now()`;
    await tx`insert into machines (install_id, label, last_seen) values (${reg.install_id}, ${reg.machine_label}, now())
      on conflict (install_id) do update set label = excluded.label, last_seen = now()`;
    await tx`insert into player_machines (player_id, install_id) values (${playerId}, ${reg.install_id}) on conflict do nothing`;
    await tx`update registrations set status = 'confirmed', confirmed_at = now(), method = 'code' where id = ${reg.id}`;
    return { accountId };
  });
  return json({ ok: true, token, account_id: result.accountId });
}
