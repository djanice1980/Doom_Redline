import { notFound } from "next/navigation";
import { isUuid } from "@/lib/crypto";
import { playerSummary } from "@/lib/players";
import { TROPHIES, formatDate, formatDuration } from "@/lib/trophies";

export const dynamic = "force-dynamic";

export default async function PlayerPage({ params }: { params: Promise<{ id: string }> }) {
  const { id } = await params;
  if (!isUuid(id)) notFound();
  const p = await playerSummary(id);
  if (!p) notFound();
  const have = new Map(p.trophies.map((t) => [t.id, t.at]));
  return (
    <>
      <h1>{p.name} {p.tier ? <span className={"tier" + (p.tier === "DOOM SLAYER" || p.tier === "SPIDER MASTERMIND" ? " top" : "")}>{p.tier}</span> : null}</h1>
      <p className="sub">Playing since {formatDate(p.since)}{p.rank ? ` · ranked #${p.rank}` : ""}{p.totals.machines > 1 ? ` · ${p.totals.machines} machines` : ""}</p>
      <div className="cards">
        <div className="card"><div className="big">{p.best_score.toLocaleString("en-US")}</div><div className="label">Best score</div></div>
        <div className="card"><div className="big">{p.totals.best_level}</div><div className="label">Highest level</div></div>
        <div className="card"><div className="big">{p.totals.runs}</div><div className="label">Games posted</div></div>
        <div className="card"><div className="big">{p.totals.kills.toLocaleString("en-US")}</div><div className="label">Demons slain</div></div>
        <div className="card"><div className="big">{p.totals.fights}</div><div className="label">Fights</div></div>
        <div className="card"><div className="big">{formatDuration(p.totals.seconds)}</div><div className="label">Played</div></div>
      </div>
      <h2>Trophies {have.size}/{TROPHIES.length}</h2>
      <div className="grid2">
        {TROPHIES.map((t) => (
          <div key={t.id} className={"trophy" + (have.has(t.id) ? "" : " locked")}>
            <div className="skull">{have.has(t.id) ? "✓" : "·"}</div>
            <div><div className="name">{t.name}</div><div className="desc">{t.description}</div></div>
            <div className="pct">{have.has(t.id) ? formatDate(have.get(t.id) ?? null) : ""}</div>
          </div>
        ))}
      </div>
      <h2>Recent games</h2>
      {p.recent.length === 0 ? <div className="empty">No games posted yet.</div> : (
        <table>
          <thead><tr><th>Date</th><th className="num">Score</th><th className="num">Level</th><th className="num">Lines</th><th className="num">Red lines</th><th className="num">Fights</th><th className="num">Demons</th><th>Ended by</th><th className="num">Length</th></tr></thead>
          <tbody>
            {p.recent.map((r) => (
              <tr key={r.id}><td>{formatDate(r.date)}</td><td className="num">{r.score.toLocaleString("en-US")}</td><td className="num">{r.level}</td><td className="num">{r.lines}</td><td className="num">{r.red_lines}</td><td className="num">{r.fights}</td><td className="num">{r.kills}</td><td>{r.death_cause === "killed" ? "a demon" : "the stack"}</td><td className="num">{formatDuration(r.duration_s)}</td></tr>
            ))}
          </tbody>
        </table>
      )}
    </>
  );
}
