export {};   // a module of its own, so its main() does not clash with the other scripts under type-checking
// Registers one player + machine against DATABASE_URL without mail (MAIL_MODE=log),
// approves it through the emailed link, and prints the token. For testing the game
// against a local server:  npx tsx scripts/seed-local-player.ts <id> <install> [name]
process.env.MAIL_MODE = "log";
process.env.EMAIL_HMAC_KEY ??= "smoke-hmac-key";
process.env.DATA_KEY ??= Buffer.alloc(32, 7).toString("base64");

async function main() {
  const [playerId, installId, name] = process.argv.slice(2);
  if (!playerId || !installId) { console.error("usage: seed-local-player <player_id> <install_id> [name]"); process.exit(2); }
  const { sql } = await import("../lib/db");
  const db = sql();
  const register = (await import("../app/api/register/route")).POST;
  const confirmLink = (await import("../app/api/confirm-link/route")).POST;
  const post = (url: string, body: unknown) => new Request(`http://local${url}`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  let r = await register(post("/api/register", { player_id: playerId, install_id: installId, email: "local-test@example.com", display_name: name ?? "TESTER", machine_label: "LOCAL TEST", version: "dev" }));
  const reg = (await r.json()) as { token?: string; approved?: boolean };
  if (r.status !== 200 || !reg.token) { console.error("register failed", r.status, reg); process.exit(1); }
  if (!reg.approved) {
    const mail = await db`select detail from mail_log order by id desc limit 1`;
    const link = /\/confirm\/([0-9a-f-]+)\?t=([A-Za-z0-9_-]+)/.exec(mail[0].detail as string);
    if (!link) { console.error("no approve link in the mail log"); process.exit(1); }
    r = await confirmLink(post("/api/confirm-link", { id: link[1], t: link[2], action: "approve" }));
    if (r.status !== 200) { console.error("approve failed", r.status, await r.text()); process.exit(1); }
  }
  console.log(reg.token);
  await db.end();
}
main().catch((e) => { console.error(e); process.exit(1); });
