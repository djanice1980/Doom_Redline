// The other players on one account, for the game's "continue as them" question.
//
// Deleting a game profile and making a new one with the same name mints a new
// player id, so registering the same address again would otherwise leave two
// rows behind, one holding all the history. After a registration is confirmed
// the game is told which other players the account already has, with enough
// detail to choose: run count, best score and when each was first seen.
//
// Only ever returned once the address has been proven (the confirm reply and
// the link poll), never from /api/register: that would let anyone ask which
// addresses exist and what the people behind them are called.
import { sql } from "./db";

export interface Sibling {
  player_id: string;
  name: string;
  since: string;
  runs: number;
  best_score: number;
  machines: number;
  same_name: boolean;
}

export async function accountSiblings(accountId: string | null, excludePlayerId: string, name: string): Promise<Sibling[]> {
  if (!accountId) return [];
  const rows = await sql()`
    select p.id, p.display_name, p.created_at,
      (select count(*) from runs r where r.player_id = p.id) as runs,
      (select coalesce(max(score), 0) from runs r where r.player_id = p.id) as best_score,
      (select count(*) from player_machines m where m.player_id = p.id) as machines
    from players p
    where p.account_id = ${accountId} and p.id <> ${excludePlayerId} and p.token_hash is not null
    order by p.created_at`;
  const want = name.trim().toLowerCase();
  return rows.map((r) => ({
    player_id: r.id as string,
    name: r.display_name as string,
    since: new Date(r.created_at as string).toISOString(),
    runs: Number(r.runs),
    best_score: Number(r.best_score),
    machines: Number(r.machines),
    same_name: (r.display_name as string).trim().toLowerCase() === want,
  }));
}
