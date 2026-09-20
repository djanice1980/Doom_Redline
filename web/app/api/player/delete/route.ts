// POST /api/player/delete: erase this player from the service.
//
// Sent with the player's own token when someone deletes a registered profile in
// the game and answers yes to "also remove it from the leaderboard". The row
// goes, and with it (on delete cascade) its runs, trophies, machine links and
// rating. The account itself stays, since the same address may have other
// players; an account with none left is tidied up by the nightly sweep.
//
// Irreversible, so the game asks first and defaults to keeping the record.
import { sql } from "@/lib/db";
import { authPlayer, error, json } from "@/lib/auth";
import { clientIp, rateLimited } from "@/lib/ratelimit";
import { ensureSchema } from "@/lib/schema";

export const runtime = "nodejs";

export async function POST(req: Request) {
  await ensureSchema();
  const who = await authPlayer(req);
  if (!who) return error("unknown or missing token", 401);
  if (await rateLimited(`delete:ip:${clientIp(req)}`, 30, 3600)) return error("too many requests", 429);

  const db = sql();
  const [before] = await db`select count(*) as n from runs where player_id = ${who.playerId}`;
  await db`delete from players where id = ${who.playerId}`;
  await db`delete from registrations where player_id = ${who.playerId}`;
  await db`select recompute_ratings()`;
  return json({ ok: true, deleted: who.playerId, runs_deleted: Number(before.n) });
}
