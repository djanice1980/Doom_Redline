import Link from "next/link";
import { BOARDS, BOARD_TITLES, BOARD_VALUE, fetchBoard, type Board } from "@/lib/boards";
import { formatDate } from "@/lib/trophies";

export const dynamic = "force-dynamic";

export default async function Home({ searchParams }: { searchParams: Promise<{ board?: string }> }) {
  const params = await searchParams;
  const board = (BOARDS as readonly string[]).includes(params.board ?? "") ? (params.board as Board) : "global";
  const reply = await fetchBoard(board, 100, null);
  return (
    <>
      <h1>Leaderboard</h1>
      <p className="sub">Best run per player. Post yours from the game: OPTIONS &gt; ONLINE, then register your email.</p>
      <div className="tabs">
        {BOARDS.map((b) => (
          <Link key={b} href={b === "global" ? "/" : `/?board=${b}`} className={b === board ? "on" : ""}>{BOARD_TITLES[b]}</Link>
        ))}
      </div>
      {reply.rows.length === 0 ? (
        <div className="empty">Nobody has posted a score on this board yet.</div>
      ) : (
        <table>
          <thead>
            <tr><th>#</th><th>Player</th><th>Rank</th><th className="num">{BOARD_VALUE[board]}</th><th className="num">Level</th><th className="num">Red lines</th><th className="num">Fights</th><th className="num">Demons</th><th>Date</th></tr>
          </thead>
          <tbody>
            {reply.rows.map((r) => (
              <tr key={r.player_id}>
                <td className="rank">{r.rank}</td>
                <td><Link href={`/player/${r.player_id}`}>{r.name}</Link></td>
                <td>{r.tier ? <span className={"tier" + (r.tier === "DOOM SLAYER" || r.tier === "SPIDER MASTERMIND" ? " top" : "")}>{r.tier}</span> : null}</td>
                <td className="num">{r.value.toLocaleString("en-US")}</td>
                <td className="num">{r.level}</td>
                <td className="num">{r.red_lines}</td>
                <td className="num">{r.fights}</td>
                <td className="num">{r.kills}</td>
                <td>{formatDate(r.date)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      <p className="sub" style={{ marginTop: 16 }}>{reply.total.toLocaleString("en-US")} player{reply.total === 1 ? "" : "s"} on this board.</p>
    </>
  );
}
