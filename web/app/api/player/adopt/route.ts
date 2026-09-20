// POST /api/player/adopt {player_id}: the game says "this new profile is really
// that older player of mine". Sent with the new profile's token, right after a
// registration, when the player picked CONTINUE AS on the question the confirm
// reply raised.
//
// Both rows must sit on the same account, which is only true when the same
// address was confirmed for both. Everything the new row has (runs, trophies,
// machines) moves onto the old one, the token moves with it so the game keeps
// the token it was just given, and the new row goes away. The game then writes
// the old id into its identity file and is one player again.
import { isUuid } from "@/lib/crypto";
import { sql } from "@/lib/db";
import { authPlayer, error, json } from "@/lib/auth";
import { clientIp, rateLimited } from "@/lib/ratelimit";
import { ensureSchema } from "@/lib/schema";

export const runtime = "nodejs";

export async function POST(req: Request) {
  await ensureSchema();
  const who = await authPlayer(req);
  if (!who) return error("unknown or missing token", 401);
  if (await rateLimited(`adopt:ip:${clientIp(req)}`, 30, 3600)) return error("too many requests", 429);
  let body: Record<string, unknown>;
  try { body = (await req.json()) as Record<string, unknown>; } catch { return error("invalid JSON"); }
  const oldId = body.player_id;
  if (!isUuid(oldId)) return error("player_id must be a uuid");
  if (oldId === who.playerId) return error("that is already this player");
  if (!who.accountId) return error("this player is not on an account", 409);

  const db = sql();
  const target = await db`select id, display_name from players where id = ${oldId} and account_id = ${who.accountId} and token_hash is not null`;
  if (target.length === 0) return error("no such player on this account", 404);

  const moved = await db.begin(async (tx) => {
    const [r] = await tx`select count(*) as n from runs where player_id = ${who.playerId}`;
    await tx`update runs set player_id = ${oldId} where player_id = ${who.playerId}`;
    await tx`insert into trophies (player_id, trophy_id, unlocked_at)
      select ${oldId}, trophy_id, unlocked_at from trophies where player_id = ${who.playerId}
      on conflict (player_id, trophy_id) do nothing`;
    await tx`insert into player_machines (player_id, install_id, confirmed_at)
      select ${oldId}, install_id, confirmed_at from player_machines where player_id = ${who.playerId}
      on conflict do nothing`;
    // The token the game is holding now belongs to the older row.
    await tx`update players set token_hash = (select token_hash from players where id = ${who.playerId}),
        display_name = ${who.displayName}, last_seen = now() where id = ${oldId}`;
    await tx`delete from players where id = ${who.playerId}`;
    return Number(r.n);
  });
  // The board and the tiers follow the moved runs straight away.
  await db`select recompute_ratings()`;
  return json({ ok: true, player_id: oldId, name: who.displayName, runs_moved: moved });
}
