// GET /api/player/<id>: a player's public summary (name, standing, totals, trophies).
import { isUuid } from "@/lib/crypto";
import { playerSummary } from "@/lib/players";
import { error, json } from "@/lib/auth";

export const runtime = "nodejs";

export async function GET(_req: Request, ctx: { params: Promise<{ id: string }> }) {
  const { id } = await ctx.params;
  if (!isUuid(id)) return error("bad player id");
  const summary = await playerSummary(id);
  if (!summary) return error("no such player", 404);
  return json(summary);
}
