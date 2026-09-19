export {};   // a module of its own, so its main() does not clash with the other scripts under type-checking
// Starts a registration for a player + machine without mail (MAIL_MODE=log) and
// prints the poll secret and the approve link, for testing the link flow with
// the game against a local server:
//   npx tsx scripts/seed-pending.ts <player_id> <install_id> [name]
process.env.MAIL_MODE = "log";
process.env.EMAIL_HMAC_KEY ??= "smoke-hmac-key";
process.env.DATA_KEY ??= Buffer.alloc(32, 7).toString("base64");
process.env.SITE_URL ??= "http://localhost:3000";

async function main() {
  const [playerId, installId, name] = process.argv.slice(2);
  if (!playerId || !installId) { console.error("usage: seed-pending <player_id> <install_id> [name]"); process.exit(2); }
  const { sql } = await import("../lib/db");
  const db = sql();
  const register = (await import("../app/api/register/route")).POST;
  const r = await register(new Request("http://local/api/register", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ player_id: playerId, install_id: installId, email: "local-test@example.com", display_name: name ?? "TESTER", machine_label: "LOCAL TEST", version: "dev" }) }));
  const j = (await r.json()) as { poll_secret?: string; error?: string };
  if (!j.poll_secret) { console.error("register failed", r.status, j); process.exit(1); }
  const mail = await db`select detail from mail_log order by id desc limit 1`;
  const link = /(http:\/\/[^\s]+\/confirm\/[0-9a-f-]+\?t=[A-Za-z0-9_-]+)/.exec(mail[0].detail as string)?.[1];
  console.log(JSON.stringify({ poll_secret: j.poll_secret, link }));
  await db.end();
}
main().catch((e) => { console.error(e); process.exit(1); });
