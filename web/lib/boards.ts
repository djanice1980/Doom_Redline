// Leaderboard queries shared by the API route and the pages.
import { sql } from "./db";

export const BOARDS = ["global", "week", "fights", "level", "kills"] as const;
export type Board = (typeof BOARDS)[number];
export const BOARD_TITLES: Record<Board, string> = { global: "All time", week: "This week", fights: "Fights survived", level: "Highest level", kills: "Demons slain" };
export const BOARD_VALUE: Record<Board, string> = { global: "Score", week: "Score", fights: "Fights", level: "Level", kills: "Demons" };

export interface BoardRow {
  rank: number;
  player_id: string;
  name: string;
  value: number;
  level: number;
  red_lines: number;
  fights: number;
  kills: number;
  tier: string;
  date: string | null;
}

export interface BoardReply {
  board: Board;
  rows: BoardRow[];
  me: BoardRow | null;
  total: number;
}

function toRow(r: Record<string, unknown>, rank: number): BoardRow {
  return {
    rank,
    player_id: r.player_id as string,
    name: r.display_name as string,
    value: Number(r.value),
    level: Number(r.level),
    red_lines: Number(r.red_lines),
    fights: Number(r.fights),
    kills: Number(r.kills),
    tier: (r.tier as string) ?? "",
    date: r.ended_at ? new Date(r.ended_at as string).toISOString() : null,
  };
}

export async function fetchBoard(board: Board, limit: number, playerId: string | null): Promise<BoardReply> {
  const db = sql();
  const view = `board_${board}`;
  const rows = await db`select b.player_id, b.display_name, b.value, b.level, b.red_lines, b.fights, b.kills, b.ended_at, coalesce(g.tier, '') as tier
    from ${db(view)} b left join ratings g on g.player_id = b.player_id
    order by b.value desc, b.ended_at asc limit ${limit}`;
  const [total] = await db`select count(*) as total from ${db(view)}`;
  let me: BoardRow | null = null;
  if (playerId) {
    const mine = await db`select b.player_id, b.display_name, b.value, b.level, b.red_lines, b.fights, b.kills, b.ended_at, coalesce(g.tier, '') as tier,
        (select 1 + count(*) from ${db(view)} o where o.value > b.value or (o.value = b.value and o.ended_at < b.ended_at)) as rank
      from ${db(view)} b left join ratings g on g.player_id = b.player_id where b.player_id = ${playerId}`;
    if (mine.length) me = toRow(mine[0] as Record<string, unknown>, Number(mine[0].rank));
  }
  return { board, rows: rows.map((r, i) => toRow(r as Record<string, unknown>, i + 1)), me, total: Number(total.total) };
}
