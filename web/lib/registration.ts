// Completing a registration, shared by the code route and the link route. The game
// already holds a token from /api/register, so this is mostly about approving: the
// account (by email hash) is created, the player is linked to it and marked
// approved, and only then does anything it has posted become visible.
import { sql } from "./db";
import { encrypt, newToken, sha256 } from "./crypto";

export interface PendingRegistration {
  id: string;
  email_hash: string;
  email_enc: string;
  player_id: string;
  install_id: string;
  display_name: string;
  machine_label: string;
  expires_at: string;
}

export async function completeRegistration(reg: PendingRegistration, method: "code" | "link"): Promise<{ token: string; accountId: string; playerId: string }> {
  const db = sql();
  // The game was given a token when it asked to register, so approval usually just
  // flips the flag. A player row that somehow has no token gets one parked for it.
  const [existing] = await db`select token_hash from players where id = ${reg.player_id}`;
  const token = existing?.token_hash ? "" : newToken();
  const accountId = await db.begin(async (tx) => {
    const acc = await tx`insert into accounts (email_hash, email_enc) values (${reg.email_hash}, ${reg.email_enc})
      on conflict (email_hash) do update set email_hash = excluded.email_hash returning id`;
    const id = acc[0].id as string;
    await tx`insert into players (id, account_id, display_name, token_hash, approved, last_seen)
      values (${reg.player_id}, ${id}, ${reg.display_name}, ${token ? sha256(token) : (existing?.token_hash as string)}, true, now())
      on conflict (id) do update set account_id = excluded.account_id, display_name = excluded.display_name,
        token_hash = coalesce(players.token_hash, excluded.token_hash), approved = true, last_seen = now()`;
    await tx`insert into machines (install_id, label, last_seen) values (${reg.install_id}, ${reg.machine_label}, now())
      on conflict (install_id) do update set label = excluded.label, last_seen = now()`;
    await tx`insert into player_machines (player_id, install_id) values (${reg.player_id}, ${reg.install_id}) on conflict do nothing`;
    await tx`update registrations set status = 'confirmed', confirmed_at = now(), method = ${method},
        token_enc = ${token ? encrypt(token) : null} where id = ${reg.id}`;
    return id;
  });
  return { token, accountId, playerId: reg.player_id };
}

// Declining removes what was collected while the email sat unread: the player row
// goes, and its runs, trophies and machine links cascade with it.
export async function declineRegistration(reg: PendingRegistration): Promise<number> {
  const db = sql();
  const [before] = await db`select count(*) as n from runs where player_id = ${reg.player_id}`;
  await db.begin(async (tx) => {
    await tx`update registrations set status = 'declined', method = 'link' where id = ${reg.id}`;
    await tx`delete from players where id = ${reg.player_id} and not approved`;
  });
  return Number(before.n);
}

export function expired(reg: { expires_at: string }): boolean {
  return new Date(reg.expires_at).getTime() < Date.now();
}
