import { sql } from "@/lib/db";
import { TROPHIES } from "@/lib/trophies";

export const dynamic = "force-dynamic";

export default async function TrophiesPage() {
  const db = sql();
  const [players] = await db`select count(*) as n from players where token_hash is not null`;
  const counts = await db`select trophy_id, count(*) as n from trophies group by trophy_id`;
  const n = Number(players.n);
  const byId = new Map(counts.map((c) => [c.trophy_id as string, Number(c.n)]));
  return (
    <>
      <h1>Trophies</h1>
      <p className="sub">How many of the {n} registered player{n === 1 ? "" : "s"} have earned each one.</p>
      <div className="grid2">
        {TROPHIES.map((t) => {
          const c = byId.get(t.id) ?? 0;
          return (
            <div key={t.id} className="trophy">
              <div className="skull">{t.id === "rip_and_tear" ? "!!!" : "✓"}</div>
              <div><div className="name">{t.name}</div><div className="desc">{t.description}</div></div>
              <div className="pct">{n ? Math.round((100 * c) / n) : 0}%</div>
            </div>
          );
        })}
      </div>
    </>
  );
}
