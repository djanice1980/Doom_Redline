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
  const [row] = await sql()`select recompute_ratings() as n`;
  return json({ ok: true, players: Number(row.n) });
}
