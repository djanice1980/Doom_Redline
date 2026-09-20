// POST /api/register {player_id, install_id, email, display_name, machine_label, version}
//
// Hands the game a token immediately, so finished games are posted from now on,
// and emails a one-click approve link. Nothing that is collected before that link
// is clicked appears anywhere public: the boards, player pages and stats all show
// approved players only, and declining deletes what was collected.
//
// Permission belongs to the address, not the profile. A profile made on a machine
// this address has already approved is approved with it and no mail is sent, so
// renaming or adding a profile costs the player nothing.
import { emailHash, encrypt, isEmail, isUuid, newCode, newToken, normalizeEmail, sha256 } from "@/lib/crypto";
import { sql } from "@/lib/db";
import { registrationMail, sendMail } from "@/lib/graph";
import { clientIp, rateLimited } from "@/lib/ratelimit";
import { error, json } from "@/lib/auth";
import { ensureSchema } from "@/lib/schema";

export const runtime = "nodejs";

export async function POST(req: Request) {
  await ensureSchema();
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

  const db = sql();
  const token = newToken();

  // An account row exists only for an address that has been approved at least once.
  // Reuse it when this machine is already one of its own: same person, new profile.
  const [account] = await db`select id from accounts where email_hash = ${hash}`;
  let known = false;
  if (account) {
    const [seen] = await db`select 1 as ok from player_machines pm join players p on p.id = pm.player_id
      where p.account_id = ${account.id} and pm.install_id = ${installId} limit 1`;
    known = !!seen;
  }

  await db`insert into machines (install_id, label, last_seen) values (${installId}, ${machineLabel}, now())
    on conflict (install_id) do update set label = excluded.label, last_seen = now()`;

  if (known) {
    await db.begin(async (tx) => {
      await tx`insert into players (id, account_id, display_name, token_hash, approved, last_seen)
        values (${playerId}, ${account.id}, ${displayName}, ${sha256(token)}, true, now())
        on conflict (id) do update set account_id = excluded.account_id, display_name = excluded.display_name,
          token_hash = excluded.token_hash, approved = true, last_seen = now()`;
      await tx`insert into player_machines (player_id, install_id) values (${playerId}, ${installId}) on conflict do nothing`;
    });
    return json({ ok: true, token, approved: true });
  }

  // A fresh address, or a machine it has not seen: collect from now, show on approval.
  if (await rateLimited(`register:email:${hash}`, 3, 3600)) return error("three emails an hour per address, try again later", 429);
  const code = newCode(), linkSecret = newToken(), pollSecret = newToken();
  await db`update registrations set status = 'expired' where player_id = ${playerId} and install_id = ${installId} and status = 'pending'`;
  const [reg] = await db.begin(async (tx) => {
    await tx`insert into players (id, display_name, token_hash, approved, last_seen)
      values (${playerId}, ${displayName}, ${sha256(token)}, false, now())
      on conflict (id) do update set display_name = excluded.display_name, token_hash = excluded.token_hash, last_seen = now()`;
    await tx`insert into player_machines (player_id, install_id) values (${playerId}, ${installId}) on conflict do nothing`;
    return tx`insert into registrations (email_hash, email_enc, player_id, install_id, display_name, machine_label, code_hash, link_hash, poll_hash)
      values (${hash}, ${encrypt(normalizeEmail(email))}, ${playerId}, ${installId}, ${displayName}, ${machineLabel}, ${sha256(code)}, ${sha256(linkSecret)}, ${sha256(pollSecret)}) returning id`;
  });

  const siteUrl = process.env.SITE_URL ?? new URL(req.url).origin;
  const link = `${siteUrl}/confirm/${reg.id as string}?t=${linkSecret}`;
  const sent = await sendMail(registrationMail(normalizeEmail(email), code, link, displayName, machineLabel, siteUrl));
  if (!sent.ok) {
    await db`update registrations set status = 'expired' where id = ${reg.id}`;
    return error("the approval email could not be sent, try again later", 502);
  }
  return json({ ok: true, token, approved: false, poll_secret: pollSecret });
}
