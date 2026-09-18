// GET /api/admin/settings: which Graph values are set and when (never the values).
// POST {graph_tenant_id?, graph_client_id?, graph_client_secret?, graph_sender?}: store (encrypted).
// Both behind ADMIN_PASSWORD (HTTP Basic).
import { GRAPH_KEYS, setSetting, settingUpdatedAt } from "@/lib/graph";
import { sql } from "@/lib/db";
import { adminChallenge, error, isAdmin, json } from "@/lib/auth";

export const runtime = "nodejs";

export async function GET(req: Request) {
  if (!isAdmin(req)) return adminChallenge();
  const status: Record<string, { set: boolean; updated_at: string | null; from_env: boolean }> = {};
  for (const k of GRAPH_KEYS) {
    const at = await settingUpdatedAt(k);
    status[k] = { set: at !== null || !!process.env[k.toUpperCase()], updated_at: at, from_env: at === null && !!process.env[k.toUpperCase()] };
  }
  const log = await sql()`select id, subject, status, detail, sent_at from mail_log order by id desc limit 20`;
  return json({ settings: status, mail_mode: process.env.MAIL_MODE ?? "graph", mail_log: log.map((r) => ({ id: Number(r.id), subject: r.subject, status: r.status, detail: r.detail, sent_at: new Date(r.sent_at as string).toISOString() })) });
}

export async function POST(req: Request) {
  if (!isAdmin(req)) return adminChallenge();
  let body: Record<string, unknown>;
  try { body = (await req.json()) as Record<string, unknown>; } catch { return error("invalid JSON"); }
  let n = 0;
  for (const k of GRAPH_KEYS) {
    const v = body[k];
    if (typeof v === "string" && v.trim()) { await setSetting(k, v.trim()); ++n; }
  }
  return json({ ok: true, updated: n });
}
