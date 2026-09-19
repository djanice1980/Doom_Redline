export {};   // a module of its own, so its main() does not clash with the other scripts under type-checking
// Registers and confirms one player + machine against DATABASE_URL without mail
// (MAIL_MODE=log), and prints the token. For testing the game against a local
// server:  npx tsx scripts/seed-local-player.ts <player_id> <install_id> <name>
process.env.MAIL_MODE = "log";
process.env.EMAIL_HMAC_KEY ??= "smoke-hmac-key";
process.env.DATA_KEY ??= Buffer.alloc(32, 7).toString("base64");

async function main() {
  const [playerId, installId, name] = process.argv.slice(2);
  if (!playerId || !installId) { console.error("usage: seed-local-player <player_id> <install_id> [name]"); process.exit(2); }
  const { sql } = await import("../lib/db");
  const db = sql();
  const register = (await import("../app/api/register/route")).POST;
  const confirm = (await import("../app/api/confirm/route")).POST;
  const post = (url: string, body: unknown) => new Request(`http://local${url}`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  let r = await register(post("/api/register", { player_id: playerId, install_id: installId, email: "local-test@example.com", display_name: name ?? "TESTER", machine_label: "LOCAL TEST", version: "dev" }));
  if (r.status !== 200) { console.error("register failed", r.status, await r.text()); process.exit(1); }
  const mail = await db`select detail from mail_log order by id desc limit 1`;
  const code = /Your code:\s+(\d{6})/.exec(mail[0].detail as string)?.[1];
  r = await confirm(post("/api/confirm", { player_id: playerId, code }));
  const j = (await r.json()) as { token?: string };
  if (!j.token) { console.error("confirm failed", r.status, j); process.exit(1); }
  console.log(j.token);
  await db.end();
}
main().catch((e) => { console.error(e); process.exit(1); });
