// Columns added after 0001 are ensured on first use, so a deployment never
// breaks on a database that has not had the newest migration run by hand yet.
// Everything here is idempotent and cheap; it runs once per process.
import { sql } from "./db";

let ensured: Promise<void> | null = null;

export function ensureSchema(): Promise<void> {
  if (!ensured) {
    ensured = (async () => {
      const db = sql();
      await db.unsafe(`
        alter table registrations add column if not exists link_hash text;
        alter table registrations add column if not exists poll_hash text;
        alter table registrations add column if not exists token_enc text;
      `);
    })().catch((e) => { ensured = null; throw e; });
  }
  return ensured;
}
