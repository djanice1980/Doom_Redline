// GET /api/cron/ratings: recompute ratings and tiers (Vercel cron, nightly; see
// vercel.json). Protected by CRON_SECRET (Vercel sends it as a Bearer token).
import { sql } from "@/lib/db";
import { safeEqual } from "@/lib/crypto";
import { error, json } from "@/lib/auth";

export const runtime = "nodejs";

export async function GET(req: Request) {
  const secret = process.env.CRON_SECRET ?? "";
  const h = req.headers.get("authorization") ?? "";
  if (!secret || !safeEqual(h, `Bearer ${secret}`)) return error("forbidden", 403);
  const db = sql();
  // Sweep first: a player row is created the moment an address is confirmed, so
  // deleting a game profile and never coming back leaves one behind. A row with
  // no runs, no trophies and a month of silence is one of those. Anything that
  // has been played is kept, and so is anything recent, since a player who has
  // just registered has not had a chance to finish a game yet.
  const pruned = await db`delete from players p
    where p.created_at < now() - interval '30 days' and p.last_seen < now() - interval '30 days'
      and not exists (select 1 from runs r where r.player_id = p.id)
      and not exists (select 1 from trophies t where t.player_id = p.id)
    returning p.id`;
  // An account with no players left has nothing to hold.
  const accounts = await db`delete from accounts a
    where not exists (select 1 from players p where p.account_id = a.id) returning a.id`;
  const [row] = await db`select recompute_ratings() as n`;
  return json({ ok: true, players: Number(row.n), pruned: pruned.length, accounts_pruned: accounts.length });
}
