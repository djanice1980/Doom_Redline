// POST /api/confirm-link {id, t, action: "approve" | "decline"}
// The buttons on the /confirm/<id> page. `t` is the secret from the emailed link.
// Approving marks the player approved, which is what makes anything it has already
// posted visible; declining deletes it.
import { isUuid, safeEqual, sha256 } from "@/lib/crypto";
import { sql } from "@/lib/db";
import { clientIp, rateLimited } from "@/lib/ratelimit";
import { completeRegistration, declineRegistration, expired, type PendingRegistration } from "@/lib/registration";
import { error, json } from "@/lib/auth";
import { ensureSchema } from "@/lib/schema";

export const runtime = "nodejs";

export async function POST(req: Request) {
  await ensureSchema();
  let body: Record<string, unknown>;
  try { body = (await req.json()) as Record<string, unknown>; } catch { return error("invalid JSON"); }
  const id = body.id, t = String(body.t ?? ""), action = body.action;
  if (!isUuid(id) || !/^[A-Za-z0-9_-]{20,}$/.test(t)) return error("bad link");
  if (action !== "approve" && action !== "decline") return error("action must be approve or decline");
  if (await rateLimited(`confirm-link:ip:${clientIp(req)}`, 60, 3600)) return error("too many attempts", 429);

  const db = sql();
  const regs = await db`select id, email_hash, email_enc, player_id, install_id, display_name, machine_label, link_hash, expires_at, status from registrations where id = ${id}`;
  if (regs.length === 0 || !regs[0].link_hash || !safeEqual(sha256(t), regs[0].link_hash as string)) return error("this link is not valid", 404);
  const reg = regs[0];
  if (reg.status === "confirmed") return json({ ok: true, status: "confirmed" });
  if (reg.status !== "pending") return error("this link was already used or has expired", 410);
  if (expired({ expires_at: reg.expires_at as string })) {
    await db`update registrations set status = 'expired' where id = ${reg.id}`;
    return error("this link has expired; ask for a new one from the game", 410);
  }
  if (action === "decline") {
    // Whatever was collected while the mail sat unread goes with the refusal.
    const deleted = await declineRegistration(reg as unknown as PendingRegistration);
    return json({ ok: true, status: "declined", runs_deleted: deleted });
  }
  await completeRegistration(reg as unknown as PendingRegistration, "link");
  return json({ ok: true, status: "confirmed" });
}
