import { sql } from "@/lib/db";
import { MONSTER_NAMES, formatDuration } from "@/lib/trophies";

export const dynamic = "force-dynamic";

export default async function StatsPage() {
  const db = sql();
  const [s] = await db`select * from public_stats`;
  const kills = await db`select k.i as kind, sum((r.kills_by_kind->>k.i)::int) as n from runs r, generate_series(0, 12) as k(i) group by k.i order by k.i`;
  const killedBy = await db`select killed_by, count(*) as n from runs where killed_by >= 0 group by killed_by order by n desc limit 5`;
  const platforms = await db`select platform, count(*) as n from runs group by platform order by n desc`;
  const runs = Number(s.runs);
  return (
    <>
      <h1>Stats</h1>
      <p className="sub">Across every game posted. Machine facts are only ever shown like this, in aggregate.</p>
      <div className="cards">
        <div className="card"><div className="big">{Number(s.players).toLocaleString("en-US")}</div><div className="label">Registered players</div></div>
        <div className="card"><div className="big">{runs.toLocaleString("en-US")}</div><div className="label">Games posted</div></div>
        <div className="card"><div className="big">{formatDuration(Number(s.seconds_played))}</div><div className="label">Played in total</div></div>
        <div className="card"><div className="big">{Number(s.demons_slain).toLocaleString("en-US")}</div><div className="label">Demons slain</div></div>
        <div className="card"><div className="big">{Number(s.blocks_destroyed).toLocaleString("en-US")}</div><div className="label">Blocks destroyed</div></div>
        <div className="card"><div className="big">{runs ? Math.round((100 * Number(s.deaths_in_fights)) / runs) : 0}%</div><div className="label">Games ended by a demon</div></div>
        <div className="card"><div className="big">{runs ? Math.round((100 * Number(s.gamepad_runs)) / runs) : 0}%</div><div className="label">Played on a gamepad</div></div>
        <div className="card"><div className="big">{runs ? Math.round((100 * Number(s.voxel_runs)) / runs) : 0}%</div><div className="label">With voxel monsters</div></div>
        <div className="card"><div className="big">{runs ? Math.round((100 * Number(s.brutal_runs)) / runs) : 0}%</div><div className="label">In Brutal mode</div></div>
      </div>
      <div className="grid2">
        <div>
          <h2>Kills by monster</h2>
          <table><thead><tr><th>Monster</th><th className="num">Killed</th></tr></thead><tbody>
            {kills.map((k) => <tr key={Number(k.kind)}><td>{MONSTER_NAMES[Number(k.kind)] ?? Number(k.kind)}</td><td className="num">{Number(k.n ?? 0).toLocaleString("en-US")}</td></tr>)}
          </tbody></table>
        </div>
        <div>
          <h2>What kills players</h2>
          <table><thead><tr><th>Monster</th><th className="num">Deaths</th></tr></thead><tbody>
            {killedBy.length === 0 ? <tr><td colSpan={2} className="empty">Nobody has died yet.</td></tr> : killedBy.map((k) => <tr key={Number(k.killed_by)}><td>{MONSTER_NAMES[Number(k.killed_by)] ?? Number(k.killed_by)}</td><td className="num">{Number(k.n)}</td></tr>)}
          </tbody></table>
          <h2>Platforms</h2>
          <table><thead><tr><th>Platform</th><th className="num">Games</th></tr></thead><tbody>
            {platforms.map((p) => <tr key={String(p.platform)}><td>{String(p.platform) || "unknown"}</td><td className="num">{Number(p.n)}</td></tr>)}
          </tbody></table>
        </div>
      </div>
    </>
  );
}
