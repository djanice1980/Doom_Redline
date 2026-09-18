// POST /api/register {player_id, install_id, email, display_name, machine_label, version}
// Starts a registration: stores the pending consent record and emails a six-digit code.
// Nothing about the player is stored beyond that record until the code is confirmed.
import { emailHash, encrypt, isEmail, isUuid, newCode, normalizeEmail, sha256 } from "@/lib/crypto";
import { sql } from "@/lib/db";
import { registrationMail, sendMail } from "@/lib/graph";
import { clientIp, rateLimited } from "@/lib/ratelimit";
import { error, json } from "@/lib/auth";

export const runtime = "nodejs";

export async function POST(req: Request) {
  let body: Record<string, unknown>;
  try { body = (await req.json()) as Record<string, unknown>; } catch { return error("invalid JSON"); }
  const playerId = body.player_id, installId = body.install_id, email = body.email;
  if (!isUuid(playerId)) return error("player_id must be a uuid");
  if (!isUuid(installId)) return error("install_id must be a uuid");
  if (!isEmail(email)) return error("that does not look like an email address");
  const displayName = String(body.display_name ?? "").slice(0, 24).trim();
  if (!/^[A-Z0-9_ ]{1,24}$/i.test(displayName)) return error("display_name must be 1-24 letters, digits or underscores");
  const machineLabel = String(body.machine_label ?? "").slice(0, 48);

  const ip = clientIp(req);
  const hash = emailHash(email);
  if (await rateLimited(`register:ip:${ip}`, 20, 3600)) return error("too many registrations from this address, try again later", 429);
  if (await rateLimited(`register:email:${hash}`, 3, 3600)) return error("three codes an hour per email address, try again later", 429);

  const code = newCode();
  const db = sql();
  await db`update registrations set status = 'expired' where player_id = ${playerId} and install_id = ${installId} and status = 'pending'`;
  await db`insert into registrations (email_hash, email_enc, player_id, install_id, display_name, machine_label, code_hash)
    values (${hash}, ${encrypt(normalizeEmail(email))}, ${playerId}, ${installId}, ${displayName}, ${machineLabel}, ${sha256(code)})`;

  const siteUrl = process.env.SITE_URL ?? new URL(req.url).origin;
  const sent = await sendMail(registrationMail(normalizeEmail(email), code, displayName, machineLabel, siteUrl));
  if (!sent.ok) {
    await db`update registrations set status = 'expired' where player_id = ${playerId} and install_id = ${installId} and status = 'pending'`;
    return error("the confirmation email could not be sent, try again later", 502);
  }
  return json({ ok: true });
}
