// POST /api/runs (Authorization: Bearer <token>, body: the game's run record)
// Validates, stores, records trophies and machine facts, and replies with the
// player's standing on the global board.
import { encrypt } from "@/lib/crypto";
import { sql } from "@/lib/db";
import { clientIp, rateLimited } from "@/lib/ratelimit";
import { validateRun } from "@/lib/validate";
import { authPlayer, error, json } from "@/lib/auth";

export const runtime = "nodejs";

export async function POST(req: Request) {
  const who = await authPlayer(req);
  if (!who) return error("unknown player token; register again in the game", 401);
  if (await rateLimited(`runs:player:${who.playerId}`, 60, 3600)) return error("too many runs an hour", 429);
  if (await rateLimited(`runs:ip:${clientIp(req)}`, 300, 3600)) return error("too many runs from this address", 429);
  let body: unknown;
  try { body = await req.json(); } catch { return error("invalid JSON"); }
  const v = validateRun(body, who.playerId);
  if (!v.ok) return error(v.error, 422);
  const r = v.row;
  const db = sql();
  // The machine must be one this player confirmed (section 9): otherwise the run is refused.
  const allowed = await db`select 1 from player_machines where player_id = ${who.playerId} and install_id = ${r.install_id}`;
  if (allowed.length === 0) return error("this machine is not registered for this player; register it under OPTIONS > PLAYER EMAIL", 403);

  const facts = (body as Record<string, unknown>).machine;
  await db.begin(async (tx) => {
    await tx`insert into runs (id, player_id, install_id, started_at, ended_at, version, platform, duration_s, score, level, lines, red_lines, fights, pieces, tetrises, best_chain,
        kills, kills_by_kind, highest_kind, blocks_destroyed, pickups, weapons_owned, shots, damage_taken, bfg_used, death_cause, killed_by, seed, voxels, brutal, input, trophies)
      values (${r.id}, ${r.player_id}, ${r.install_id}, ${r.started_at}, ${r.ended_at}, ${r.version}, ${r.platform}, ${r.duration_s}, ${r.score}, ${r.level}, ${r.lines}, ${r.red_lines}, ${r.fights}, ${r.pieces}, ${r.tetrises}, ${r.best_chain},
        ${r.kills}, ${tx.json(r.kills_by_kind)}, ${r.highest_kind}, ${r.blocks_destroyed}, ${r.pickups}, ${tx.json(r.weapons_owned)}, ${tx.json(r.shots)}, ${r.damage_taken}, ${r.bfg_used}, ${r.death_cause}, ${r.killed_by}, ${r.seed}, ${r.voxels}, ${r.brutal}, ${tx.json(r.input)}, ${tx.json(r.trophies)})
      on conflict (id) do nothing`;
    for (const t of r.trophies) await tx`insert into trophies (player_id, trophy_id) values (${who.playerId}, ${t}) on conflict do nothing`;
    if (facts && typeof facts === "object") await tx`update machines set facts_enc = ${encrypt(JSON.stringify(facts).slice(0, 4000))}, last_seen = now() where install_id = ${r.install_id}`;
    else await tx`update machines set last_seen = now() where install_id = ${r.install_id}`;
  });

  // Standing: this run's rank among every player's best, the player's best rank, the player count.
  const [rankRow] = await db`select 1 + count(*) as rank from board_global where value > ${r.score} and player_id <> ${who.playerId}`;
  const [bestRow] = await db`select 1 + count(*) as rank from board_global b where b.value > (select coalesce(max(score), 0) from runs where player_id = ${who.playerId})`;
  const [totalRow] = await db`select count(*) as total from board_global`;
  const tier = await db`select tier from ratings where player_id = ${who.playerId}`;
  return json({
    ok: true,
    rank: Number(rankRow.rank),
    best_rank: Number(bestRow.rank),
    total_players: Number(totalRow.total),
    tier: tier.length ? (tier[0].tier as string) : "",
  });
}

export function GET() {
  return error("POST a run record with a Bearer token", 405);
}

