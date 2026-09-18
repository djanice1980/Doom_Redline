// A player's public summary, shared by the API route and the player page.
import { sql } from "./db";

export interface PlayerSummary {
  player_id: string;
  name: string;
  since: string;
  rating: number;
  rank: number | null;
  tier: string;
  best_score: number;
  totals: { runs: number; seconds: number; kills: number; best_level: number; fights: number; blocks: number; deaths: number; machines: number };
  trophies: { id: string; at: string }[];
  recent: { id: string; date: string | null; score: number; level: number; lines: number; red_lines: number; fights: number; kills: number; death_cause: string; duration_s: number }[];
}

export async function playerSummary(id: string): Promise<PlayerSummary | null> {
  const db = sql();
  const p = await db`select p.id, p.display_name, p.created_at, coalesce(g.rating, 0) as rating, g.rank, coalesce(g.tier, '') as tier, coalesce(g.best_score, 0) as best_score
    from players p left join ratings g on g.player_id = p.id where p.id = ${id} and p.token_hash is not null`;
  if (p.length === 0) return null;
  const [t] = await db`select count(*) as runs, coalesce(sum(duration_s), 0) as seconds, coalesce(sum(kills), 0) as kills, coalesce(max(level), 0) as best_level,
      coalesce(sum(fights), 0) as fights, coalesce(sum(blocks_destroyed), 0) as blocks, count(*) filter (where death_cause = 'killed') as deaths,
      (select count(*) from player_machines where player_id = ${id}) as machines
    from runs where player_id = ${id}`;
  const trophies = await db`select trophy_id, unlocked_at from trophies where player_id = ${id} order by unlocked_at`;
  const recent = await db`select id, ended_at, score, level, lines, red_lines, fights, kills, death_cause, duration_s from runs where player_id = ${id} order by submitted_at desc limit 10`;
  const r = p[0];
  return {
    player_id: r.id as string,
    name: r.display_name as string,
    since: new Date(r.created_at as string).toISOString(),
    rating: Number(r.rating),
    rank: r.rank == null ? null : Number(r.rank),
    tier: r.tier as string,
    best_score: Number(r.best_score),
    totals: { runs: Number(t.runs), seconds: Number(t.seconds), kills: Number(t.kills), best_level: Number(t.best_level), fights: Number(t.fights), blocks: Number(t.blocks), deaths: Number(t.deaths), machines: Number(t.machines) },
    trophies: trophies.map((x) => ({ id: x.trophy_id as string, at: new Date(x.unlocked_at as string).toISOString() })),
    recent: recent.map((x) => ({ id: x.id as string, date: x.ended_at ? new Date(x.ended_at as string).toISOString() : null, score: Number(x.score), level: Number(x.level), lines: Number(x.lines), red_lines: Number(x.red_lines), fights: Number(x.fights), kills: Number(x.kills), death_cause: x.death_cause as string, duration_s: Number(x.duration_s) })),
  };
}
