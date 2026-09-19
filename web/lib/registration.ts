// Completing a registration, shared by the code route and the link route: the
// account (by email hash), the player, the machine, the consent record, and a
// fresh per-player token. Returns the token; the caller hands it to the game
// (code path) or parks it encrypted for the game to poll (link path).
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

export async function completeRegistration(reg: PendingRegistration, method: "code" | "link"): Promise<{ token: string; accountId: string }> {
  const token = newToken();
  const db = sql();
  const accountId = await db.begin(async (tx) => {
    const acc = await tx`insert into accounts (email_hash, email_enc) values (${reg.email_hash}, ${reg.email_enc})
      on conflict (email_hash) do update set email_hash = excluded.email_hash returning id`;
    const id = acc[0].id as string;
    await tx`insert into players (id, account_id, display_name, token_hash, last_seen) values (${reg.player_id}, ${id}, ${reg.display_name}, ${sha256(token)}, now())
      on conflict (id) do update set account_id = excluded.account_id, display_name = excluded.display_name, token_hash = excluded.token_hash, last_seen = now()`;
    await tx`insert into machines (install_id, label, last_seen) values (${reg.install_id}, ${reg.machine_label}, now())
      on conflict (install_id) do update set label = excluded.label, last_seen = now()`;
    await tx`insert into player_machines (player_id, install_id) values (${reg.player_id}, ${reg.install_id}) on conflict do nothing`;
    await tx`update registrations set status = 'confirmed', confirmed_at = now(), method = ${method},
        token_enc = ${method === "link" ? encrypt(token) : null} where id = ${reg.id}`;
    return id;
  });
  return { token, accountId };
}

export function expired(reg: { expires_at: string }): boolean {
  return new Date(reg.expires_at).getTime() < Date.now();
}
