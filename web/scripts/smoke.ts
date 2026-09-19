// End-to-end check against a local Postgres (see README): migrate, register,
// read the code from the mail log (MAIL_MODE=log), confirm, post a run, read
// the boards and the player page, recompute ratings. Run with `npm run smoke`.
process.env.MAIL_MODE = "log";
process.env.EMAIL_HMAC_KEY ??= "smoke-hmac-key";
process.env.DATA_KEY ??= Buffer.alloc(32, 7).toString("base64");
process.env.ADMIN_PASSWORD ??= "smoke";
process.env.CRON_SECRET ??= "smoke-cron";
process.env.SITE_URL ??= "http://localhost:3000";

import { randomUUID } from "node:crypto";
import { readFileSync } from "node:fs";
import { join } from "node:path";

let failures = 0;
function check(cond: unknown, what: string) {
  if (!cond) { failures++; console.error("FAIL:", what); } else console.log("ok  :", what);
}

async function main() {
  const { sql } = await import("../lib/db");
  const db = sql();
  for (const f of ["0001_init.sql", "0002_links_and_polling.sql", "0003_hardening.sql", "0004_linter_quiet.sql"]) await db.unsafe(readFileSync(join(__dirname, "..", "supabase", "migrations", f), "utf8"));
  for (const t of ["runs", "trophies", "player_machines", "registrations", "players", "accounts", "machines", "ratings", "mail_log", "rate_limits", "settings"]) await db.unsafe(`delete from ${t}`);

  const register = (await import("../app/api/register/route")).POST;
  const confirm = (await import("../app/api/confirm/route")).POST;
  const runs = (await import("../app/api/runs/route")).POST;
  const leaderboard = (await import("../app/api/leaderboard/route")).GET;
  const player = (await import("../app/api/player/[id]/route")).GET;
  const cron = (await import("../app/api/cron/ratings/route")).GET;
  const adminSettings = (await import("../app/api/admin/settings/route"));
  const post = (url: string, body: unknown, headers: Record<string, string> = {}) => new Request(`http://local${url}`, { method: "POST", headers: { "Content-Type": "application/json", ...headers }, body: JSON.stringify(body) });

  const playerId = randomUUID(), installId = randomUUID();
  // 1. register: bad input, then good
  let r = await register(post("/api/register", { player_id: "nope", install_id: installId, email: "a@b.co", display_name: "MARINE" }));
  check(r.status === 400, "register rejects a bad player id");
  r = await register(post("/api/register", { player_id: playerId, install_id: installId, email: "marine@example.com", display_name: "MARINE", machine_label: "LINUX / RADEON", version: "0.1.0" }));
  const regReply = (await r.clone().json()) as { ok?: boolean; poll_secret?: string };
  check(r.status === 200 && !!regReply.poll_secret, `register accepts and returns a poll secret (status ${r.status})`);
  const mail = await db`select detail from mail_log order by id desc limit 1`;
  const code = /code into the game where it asks:\s+(\d{6})/.exec(mail[0].detail as string)?.[1];
  check(!!code, "a six-digit code was mailed (logged)");
  const link = /(http:\/\/localhost:3000\/confirm\/[0-9a-f-]+\?t=[A-Za-z0-9_-]+)/.exec(mail[0].detail as string)?.[1];
  check(!!link, "an approve link was mailed (logged)");
  const poll = (await import("../app/api/registration/route")).POST;
  r = await poll(post("/api/registration", { player_id: playerId, poll_secret: regReply.poll_secret }));
  check(r.status === 200 && ((await r.json()) as { status: string }).status === "pending", "polling reports pending");
  // 2. confirm: wrong code, then right
  r = await confirm(post("/api/confirm", { player_id: playerId, code: code === "000000" ? "000001" : "000000" }));
  check(r.status === 400, "confirm rejects a wrong code");
  r = await confirm(post("/api/confirm", { player_id: playerId, code }));
  const conf = (await r.json()) as { ok?: boolean; token?: string };
  check(r.status === 200 && !!conf.token, "confirm returns a token");
  const token = conf.token as string;
  const acc = await db`select count(*) as n from accounts`;
  check(Number(acc[0].n) === 1, "one account row, keyed by the email hash");
  const pm = await db`select count(*) as n from player_machines where player_id = ${playerId} and install_id = ${installId}`;
  check(Number(pm[0].n) === 1, "the machine is attached to the player");
  // 3. a run
  const run = {
    run_id: randomUUID(), player_id: playerId, install_id: installId, display_name: "MARINE", version: "0.1.0", platform: "Linux",
    started_at: "2026-09-18T10:00:00Z", ended_at: "2026-09-18T10:06:00Z", duration_s: 360, score: 24680, level: 3, lines: 22, red_lines: 2, fights: 2, pieces: 90, tetrises: 1, best_chain: 2,
    kills_by_kind: [3, 4, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1], kills: 9, highest_kind: 12, blocks_destroyed: 31, pickups: 6, weapons_owned: ["shotgun", "chaingun"], shots: { shotgun: 40, chaingun: 120, rocket_launcher: 0, plasma_rifle: 0 },
    damage_taken: 88, bfg_used: false, death_cause: "killed", killed_by: 12, seed: 1234, voxels: true, brutal: true, input: { keyboard: 300, mouse: 200, gamepad: 0 }, trophies: ["first_blood", "red_line"],
    machine: { platform: "Linux", os: "CachyOS", gpu: "Radeon 8060S", cores: 16, ram: 65536, pad: "DualSense" },
  };
  r = await runs(post("/api/runs", run));
  check(r.status === 401, "a run without a token is refused");
  r = await runs(post("/api/runs", { ...run, score: 999_999_999 }, { Authorization: `Bearer ${token}` }));
  check(r.status === 422, "an implausible score is refused");
  r = await runs(post("/api/runs", run, { Authorization: `Bearer ${token}` }));
  const posted = (await r.json()) as { ok?: boolean; rank?: number; total_players?: number };
  check(r.status === 200 && posted.rank === 1 && posted.total_players === 1, `the run is stored and ranked (${JSON.stringify(posted)})`);
  r = await runs(post("/api/runs", run, { Authorization: `Bearer ${token}` }));
  check(r.status === 200, "posting the same run twice is harmless");
  const runRows = await db`select count(*) as n from runs`;
  check(Number(runRows[0].n) === 1, "one run row after the duplicate");
  const jt = await db`select jsonb_typeof(kills_by_kind) as a, jsonb_typeof(shots) as o, jsonb_typeof(input) as i from runs limit 1`;
  check(jt[0].a === "array" && jt[0].o === "object" && jt[0].i === "object", "jsonb columns hold real JSON values, not strings");
  const tro = await db`select count(*) as n from trophies where player_id = ${playerId}`;
  check(Number(tro[0].n) === 2, "trophies recorded");
  const mach = await db`select facts_enc from machines where install_id = ${installId}`;
  check(typeof mach[0].facts_enc === "string" && (mach[0].facts_enc as string).startsWith("v1."), "machine facts stored encrypted");
  const otherMachine = randomUUID();
  r = await runs(post("/api/runs", { ...run, run_id: randomUUID(), install_id: otherMachine }, { Authorization: `Bearer ${token}` }));
  check(r.status === 403, "a run from an unregistered machine is refused");
  // 4. boards
  r = await leaderboard(new Request(`http://local/api/leaderboard?board=global&player=${playerId}`));
  const board = (await r.json()) as { rows: { name: string; value: number }[]; me: { rank: number } | null; total: number };
  check(board.rows.length === 1 && board.rows[0].name === "MARINE" && board.rows[0].value === 24680 && board.me?.rank === 1, "the global board lists the run with the player's own row");
  r = await leaderboard(new Request("http://local/api/leaderboard?board=kills"));
  check(((await r.json()) as { rows: { value: number }[] }).rows[0].value === 9, "the kills board uses kills");
  r = await leaderboard(new Request("http://local/api/leaderboard?board=nope"));
  check(r.status === 400, "an unknown board is refused");
  // 5. player page data
  r = await player(new Request(`http://local/api/player/${playerId}`), { params: Promise.resolve({ id: playerId }) });
  const ps = (await r.json()) as { name: string; totals: { runs: number; machines: number }; trophies: unknown[] };
  check(ps.name === "MARINE" && ps.totals.runs === 1 && ps.totals.machines === 1 && ps.trophies.length === 2, "the player summary is right");
  // 6. ratings
  r = await cron(new Request("http://local/api/cron/ratings", { headers: { Authorization: "Bearer wrong" } }));
  check(r.status === 403, "the cron route needs the secret");
  r = await cron(new Request("http://local/api/cron/ratings", { headers: { Authorization: `Bearer ${process.env.CRON_SECRET}` } }));
  check(r.status === 200, "ratings recomputed");
  const rating = await db`select tier, rank from ratings where player_id = ${playerId}`;
  check(rating[0]?.tier === "DOOM SLAYER" && Number(rating[0]?.rank) === 1, "the only player is DOOM SLAYER");
  // 6b. approval by link for a second player + machine, then decline for a third
  const confirmLink = (await import("../app/api/confirm-link/route")).POST;
  const p2 = randomUUID(), m2 = randomUUID();
  r = await register(post("/api/register", { player_id: p2, install_id: m2, email: "second@example.com", display_name: "SECOND", machine_label: "WINDOWS / RTX", version: "0.3.0" }));
  const reg2 = (await r.json()) as { poll_secret?: string };
  const mail2 = await db`select detail from mail_log order by id desc limit 1`;
  const link2 = /\/confirm\/([0-9a-f-]+)\?t=([A-Za-z0-9_-]+)/.exec(mail2[0].detail as string);
  check(!!link2, "second registration mailed a link");
  r = await confirmLink(post("/api/confirm-link", { id: link2![1], t: "wrong-secret-wrong-secret", action: "approve" }));
  check(r.status === 404, "a link with the wrong secret is refused");
  r = await confirmLink(post("/api/confirm-link", { id: link2![1], t: link2![2], action: "approve" }));
  check(r.status === 200 && ((await r.json()) as { status: string }).status === "confirmed", "approving by link confirms");
  r = await poll(post("/api/registration", { player_id: p2, poll_secret: reg2.poll_secret }));
  const polled = (await r.json()) as { status: string; token?: string };
  check(polled.status === "confirmed" && !!polled.token, "polling after a link approval hands over the token");
  r = await poll(post("/api/registration", { player_id: p2, poll_secret: reg2.poll_secret }));
  check(((await r.json()) as { token?: string }).token === undefined, "the token is handed over only once");
  r = await runs(post("/api/runs", { ...run, run_id: randomUUID(), player_id: p2, install_id: m2, score: 100, level: 1, fights: 0, lines: 2, pieces: 10, tetrises: 0, kills_by_kind: [1,0,0,0,0,0,0,0,0,0,0,0,0], kills: 1, highest_kind: 0, death_cause: "stack", killed_by: -1, trophies: [] }, { Authorization: `Bearer ${polled.token}` }));
  check(r.status === 200, "the link-approved player can post a run");
  const p3 = randomUUID(), m3 = randomUUID();
  r = await register(post("/api/register", { player_id: p3, install_id: m3, email: "third@example.com", display_name: "THIRD", machine_label: "", version: "0.3.0" }));
  const reg3 = (await r.json()) as { poll_secret?: string };
  const mail3 = await db`select detail from mail_log order by id desc limit 1`;
  const link3 = /\/confirm\/([0-9a-f-]+)\?t=([A-Za-z0-9_-]+)/.exec(mail3[0].detail as string);
  r = await confirmLink(post("/api/confirm-link", { id: link3![1], t: link3![2], action: "decline" }));
  check(r.status === 200 && ((await r.json()) as { status: string }).status === "declined", "declining by link works");
  r = await poll(post("/api/registration", { player_id: p3, poll_secret: reg3.poll_secret }));
  check(((await r.json()) as { status: string }).status === "declined", "polling reports the decline");
  const p3rows = await db`select count(*) as n from players where id = ${p3}`;
  check(Number(p3rows[0].n) === 0, "a declined registration stores no player");
  const vopts = await db`select c.relname, c.reloptions from pg_class c join pg_namespace n on n.oid = c.relnamespace where n.nspname = 'public' and c.relkind = 'v'`;
  check(vopts.length === 6 && vopts.every((v) => JSON.stringify(v.reloptions ?? []).includes("security_invoker=on")), "every view runs as the caller (security_invoker)");
  const fn = await db`select proconfig from pg_proc where proname = 'recompute_ratings'`;
  check(JSON.stringify(fn[0]?.proconfig ?? []).includes("search_path=public"), "recompute_ratings has a fixed search_path");
  const pol = await db`select count(distinct tablename) as n from pg_policies where schemaname = 'public' and policyname = 'deny_all'`;
  check(Number(pol[0].n) === 11, "every table carries the deny_all policy");
  // 6c. changelog parsing for the version route
  const { parseChangelog } = await import("../lib/version");
  const cl = parseChangelog("# Changelog\n\n## 0.3.0 (unreleased)\n- One thing.\n- Another `thing` **bold**.\n\n## v0.2.0 (2026-09-18)\n- Old.\n");
  check(cl.length === 2 && cl[0].version === "0.3.0" && cl[0].items.length === 2 && cl[0].items[1] === "Another thing bold." && cl[1].date === "2026-09-18", "the changelog parser reads headings and bullets");
  // 7. admin settings (encrypted at rest, never returned)
  const basic = "Basic " + Buffer.from(`admin:${process.env.ADMIN_PASSWORD}`).toString("base64");
  r = await adminSettings.POST(post("/api/admin/settings", { graph_tenant_id: "tenant-123" }, { Authorization: basic }));
  check(r.status === 200, "admin can save a setting");
  r = await adminSettings.GET(new Request("http://local/api/admin/settings", { headers: { Authorization: basic } }));
  const st = (await r.json()) as { settings: Record<string, { set: boolean }> };
  check(st.settings.graph_tenant_id.set && !JSON.stringify(st).includes("tenant-123"), "settings status shows set without the value");
  const raw = await db`select value_enc from settings where key = 'graph_tenant_id'`;
  check((raw[0].value_enc as string).startsWith("v1.") && !(raw[0].value_enc as string).includes("tenant-123"), "the setting is encrypted in the table");
  r = await adminSettings.GET(new Request("http://local/api/admin/settings"));
  check(r.status === 401, "admin routes need the password");
  // 8. the email is not readable: only its hash and ciphertext are stored
  const a = await db`select email_hash, email_enc from accounts`;
  check(!JSON.stringify(a).includes("marine@example.com"), "the address is not stored in the clear");
  const { decrypt } = await import("../lib/crypto");
  check(decrypt(a[0].email_enc as string) === "marine@example.com", "and decrypts with the data key");

  await db.end();
  if (failures) { console.error(`${failures} failure(s)`); process.exit(1); }
  console.log("smoke: all passed");
}

main().catch((e) => { console.error(e); process.exit(1); });
