// GET /api/leaderboard?board=global|week|fights|level|kills&player=<id>&limit=100
// Public: display names and gameplay numbers only.
import { isUuid } from "@/lib/crypto";
import { BOARDS, fetchBoard, type Board } from "@/lib/boards";
import { error, json } from "@/lib/auth";

export const runtime = "nodejs";

export async function GET(req: Request) {
  const url = new URL(req.url);
  const board = (url.searchParams.get("board") ?? "global") as Board;
  if (!BOARDS.includes(board)) return error("unknown board");
  const limit = Math.min(100, Math.max(1, parseInt(url.searchParams.get("limit") ?? "100", 10) || 100));
  const player = url.searchParams.get("player");
  return json(await fetchBoard(board, limit, isUuid(player) ? player : null));
}
