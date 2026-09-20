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
  for (const f of ["0001_init.sql", "0002_links_and_polling.sql", "0003_hardening.sql", "0004_linter_quiet.sql", "0005_approval.sql"]) await db.unsafe(readFileSync(join(__dirname, "..", "supabase", "migrations", f), "utf8"));
  for (const t of ["runs", "trophies", "player_machines", "registrations", "players", "accounts", "machines", "ratings", "mail_log", "rate_limits", "settings"]) await db.unsafe(`delete from ${t}`);

  const register = (await import("../app/api/register/route")).POST;
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
  const regReply = (await r.clone().json()) as { ok?: boolean; poll_secret?: string; token?: string; approved?: boolean };
  check(r.status === 200 && !!regReply.poll_secret, `register accepts and returns a poll secret (status ${r.status})`);
  check(!!regReply.token && regReply.approved === false, "register hands over a token at once, not yet approved");
  // Collected from the start: a run posts now and is stored, but shows up nowhere.
  const earlyRun = {
    run_id: randomUUID(), player_id: playerId, install_id: installId, display_name: "MARINE", version: "0.1.0", platform: "Linux",
    started_at: "2026-09-20T10:00:00Z", ended_at: "2026-09-20T10:04:00Z", duration_s: 240, score: 4321, level: 2, lines: 9, red_lines: 1, fights: 1, pieces: 40, tetrises: 0, best_chain: 1,
    kills_by_kind: [2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0], kills: 3, highest_kind: 1, blocks_destroyed: 4, pickups: 2, weapons_owned: ["shotgun"], shots: { shotgun: 20, chaingun: 0, rocket_launcher: 0, plasma_rifle: 0 },
    damage_taken: 20, bfg_used: false, death_cause: "killed", killed_by: 1, seed: 5, voxels: false, brutal: false, input: { keyboard: 50, mouse: 40, gamepad: 0 }, trophies: [],
    machine: { platform: "Linux", os: "CachyOS", gpu: "Radeon 8060S", cores: 16, ram: 65536, pad: "" },
  };
  r = await runs(post("/api/runs", earlyRun, { Authorization: `Bearer ${regReply.token}` }));
  const early = (await r.json()) as { ok?: boolean; approved?: boolean; rank?: number };
  check(r.status === 200 && early.approved === false && early.rank === undefined, "an unapproved run is accepted but gets no standing back");
  const storedEarly = await db`select count(*) as n from runs where player_id = ${playerId}`;
  check(Number(storedEarly[0].n) === 1, "and it is stored");
  r = await leaderboard(new Request("http://local/api/leaderboard?board=global"));
  check(((await r.json()) as { rows: unknown[] }).rows.length === 0, "but the board shows nothing for it");
  r = await player(new Request(`http://local/api/player/${playerId}`), { params: Promise.resolve({ id: playerId }) });
  check(r.status === 404, "and the player page is not there yet");
  const mail = await db`select detail from mail_log order by id desc limit 1`;
  check(!/type this code|code into the game/i.test(mail[0].detail as string), "the mail asks for no code, just the link");
  const linkParts = /\/confirm\/([0-9a-f-]+)\?t=([A-Za-z0-9_-]+)/.exec(mail[0].detail as string);
  check(!!linkParts, "an approve link was mailed (logged)");
  const poll = (await import("../app/api/registration/route")).POST;
  r = await poll(post("/api/registration", { player_id: playerId, poll_secret: regReply.poll_secret }));
  const pending = (await r.json()) as { status: string; approved?: boolean };
  check(r.status === 200 && pending.status === "pending" && pending.approved === false, "polling reports pending and not approved");
  // 2. approval, the only way in: one click on the emailed link
  const confirmLink = (await import("../app/api/confirm-link/route")).POST;
  r = await confirmLink(post("/api/confirm-link", { id: linkParts![1], t: "wrong-secret-wrong-secret", action: "approve" }));
  check(r.status === 404, "a link with the wrong secret is refused");
  r = await confirmLink(post("/api/confirm-link", { id: linkParts![1], t: linkParts![2], action: "approve" }));
  check(r.status === 200 && ((await r.json()) as { status: string }).status === "confirmed", "approving by link confirms");
  const token = regReply.token as string;
  r = await poll(post("/api/registration", { player_id: playerId, poll_secret: regReply.poll_secret }));
  const polled1 = (await r.json()) as { status: string; approved?: boolean };
  check(polled1.status === "confirmed" && polled1.approved === true, "and the game learns it is approved by polling");
  const approvedRow = await db`select approved from players where id = ${playerId}`;
  check(approvedRow[0].approved === true, "the player row is approved");
  r = await leaderboard(new Request("http://local/api/leaderboard?board=global"));
  check(((await r.json()) as { rows: { value: number }[] }).rows[0]?.value === 4321, "and the run collected before approval is now on the board");
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
  check(Number(runRows[0].n) === 2, "two run rows after the duplicate: the early one and this");
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
  check(ps.name === "MARINE" && ps.totals.runs === 2 && ps.totals.machines === 1 && ps.trophies.length === 2, "the player summary is right");
  // 6. ratings
  r = await cron(new Request("http://local/api/cron/ratings", { headers: { Authorization: "Bearer wrong" } }));
  check(r.status === 403, "the cron route needs the secret");
  r = await cron(new Request("http://local/api/cron/ratings", { headers: { Authorization: `Bearer ${process.env.CRON_SECRET}` } }));
  check(r.status === 200, "ratings recomputed");
  const rating = await db`select tier, rank from ratings where player_id = ${playerId}`;
  check(rating[0]?.tier === "DOOM SLAYER" && Number(rating[0]?.rank) === 1, "the only player is DOOM SLAYER");
  // 6b. approval by link for a second player + machine, then decline for a third
  const p2 = randomUUID(), m2 = randomUUID();
  r = await register(post("/api/register", { player_id: p2, install_id: m2, email: "second@example.com", display_name: "SECOND", machine_label: "WINDOWS / RTX", version: "0.3.0" }));
  const reg2 = (await r.json()) as { poll_secret?: string; token?: string };
  const mail2 = await db`select detail from mail_log order by id desc limit 1`;
  const link2 = /\/confirm\/([0-9a-f-]+)\?t=([A-Za-z0-9_-]+)/.exec(mail2[0].detail as string);
  check(!!link2, "second registration mailed a link");
  r = await confirmLink(post("/api/confirm-link", { id: link2![1], t: "wrong-secret-wrong-secret", action: "approve" }));
  check(r.status === 404, "a link with the wrong secret is refused");
  r = await confirmLink(post("/api/confirm-link", { id: link2![1], t: link2![2], action: "approve" }));
  check(r.status === 200 && ((await r.json()) as { status: string }).status === "confirmed", "approving by link confirms");
  r = await poll(post("/api/registration", { player_id: p2, poll_secret: reg2.poll_secret }));
  const polled = (await r.json()) as { status: string; approved?: boolean };
  check(polled.status === "confirmed" && polled.approved === true, "polling after a link approval reports approved");
  r = await runs(post("/api/runs", { ...run, run_id: randomUUID(), player_id: p2, install_id: m2, score: 100, level: 1, fights: 0, lines: 2, pieces: 10, tetrises: 0, kills_by_kind: [1,0,0,0,0,0,0,0,0,0,0,0,0], kills: 1, highest_kind: 0, death_cause: "stack", killed_by: -1, trophies: [] }, { Authorization: `Bearer ${reg2.token}` }));
  check(r.status === 200, "the link-approved player can post a run");
  const p3 = randomUUID(), m3 = randomUUID();
  r = await register(post("/api/register", { player_id: p3, install_id: m3, email: "third@example.com", display_name: "THIRD", machine_label: "", version: "0.3.0" }));
  const reg3 = (await r.json()) as { poll_secret?: string; token?: string };
  const mail3 = await db`select detail from mail_log order by id desc limit 1`;
  const link3 = /\/confirm\/([0-9a-f-]+)\?t=([A-Za-z0-9_-]+)/.exec(mail3[0].detail as string);
  r = await confirmLink(post("/api/confirm-link", { id: link3![1], t: link3![2], action: "decline" }));
  check(r.status === 200 && ((await r.json()) as { status: string }).status === "declined", "declining by link works");
  r = await poll(post("/api/registration", { player_id: p3, poll_secret: reg3.poll_secret }));
  check(((await r.json()) as { status: string }).status === "declined", "polling reports the decline");
  const p3rows = await db`select count(*) as n from players where id = ${p3}`;
  check(Number(p3rows[0].n) === 0, "declining deletes the player it collected for");
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

  // 9. deleting a profile and making a new one with the same name and address:
  //    the confirm reply offers the old player, adopt merges the two.
  const playerId2 = randomUUID(), installId2 = randomUUID();
  const mailsBefore = await db`select count(*) as n from mail_log`;
  r = await register(post("/api/register", { player_id: playerId2, install_id: installId, email: "marine@example.com", display_name: "MARINE", machine_label: "LINUX / RADEON", version: "0.5.0" }));
  const conf2 = (await r.json()) as { token?: string; approved?: boolean };
  const token2 = conf2.token as string;
  check(r.status === 200 && conf2.approved === true, "a second profile on a machine this address already approved needs no email");
  const mailsAfter = await db`select count(*) as n from mail_log`;
  check(Number(mailsAfter[0].n) === Number(mailsBefore[0].n), "and no mail was sent for it");
  const approved2 = await db`select approved, account_id from players where id = ${playerId2}`;
  check(approved2[0].approved === true && !!approved2[0].account_id, "it is approved and on the same account");
  // A profile on a machine the address has never approved still asks.
  const p5 = randomUUID(), m5 = randomUUID();
  r = await register(post("/api/register", { player_id: p5, install_id: m5, email: "marine@example.com", display_name: "MARINE", machine_label: "OTHER BOX", version: "0.5.0" }));
  const reg5 = (await r.json()) as { approved?: boolean; poll_secret?: string };
  check(r.status === 200 && reg5.approved === false && !!reg5.poll_secret, "but a new machine on the same address is asked");
  const accounts2 = await db`select count(distinct account_id) as n from players where id in (${playerId}, ${playerId2})`;
  check(Number(accounts2[0].n) === 1, "both players sit on one account");
  check(!conf2.existing?.some((e) => e.name === "SECOND"), "a player on a different address is not offered");
  // A run posted by the new profile before it adopts still ends up on the old player.
  r = await runs(post("/api/runs", { ...run, run_id: randomUUID(), player_id: playerId2, install_id: installId, score: 5000 }, { Authorization: `Bearer ${token2}` }));
  check(r.status === 200, "the new profile can post a run of its own");
  const adopt = (await import("../app/api/player/adopt/route")).POST;
  r = await adopt(post("/api/player/adopt", { player_id: playerId }, { Authorization: `Bearer ${token2}` }));
  const ad = (await r.json()) as { ok?: boolean; player_id?: string; runs_moved?: number };
  check(r.status === 200 && ad.player_id === playerId && ad.runs_moved === 1, `adopt moves the new profile onto the old player (${JSON.stringify(ad)})`);
  const gone = await db`select count(*) as n from players where id = ${playerId2}`;
  check(Number(gone[0].n) === 0, "the duplicate row is gone");
  const both = await db`select count(*) as n from runs where player_id = ${playerId}`;
  check(Number(both[0].n) === 3, "and every run belongs to the old player");
  const machines2 = await db`select count(*) as n from player_machines where player_id = ${playerId}`;
  check(Number(machines2[0].n) === 1, "still on the one machine it was made on");
  r = await runs(post("/api/runs", { ...run, run_id: randomUUID(), player_id: playerId, install_id: installId, score: 6000 }, { Authorization: `Bearer ${token2}` }));
  check(r.status === 200, "the token the game already has now posts as the old player");
  r = await adopt(post("/api/player/adopt", { player_id: randomUUID() }, { Authorization: `Bearer ${token2}` }));
  check(r.status === 404, "adopting a player that is not on the account is refused");
  r = await adopt(post("/api/player/adopt", { player_id: playerId }, {}));
  check(r.status === 401, "adopt needs a token");
  // 10. the nightly sweep: a registered profile that was deleted and never played
  const stale = randomUUID();
  await db`insert into players (id, account_id, display_name, token_hash, created_at, last_seen)
    select ${stale}, id, 'GHOST', 'x', now() - interval '60 days', now() - interval '60 days' from accounts limit 1`;
  const fresh = randomUUID();
  await db`insert into players (id, account_id, display_name, token_hash) select ${fresh}, id, 'NEWCOMER', 'y' from accounts limit 1`;
  r = await cron(new Request("http://local/api/cron/ratings", { headers: { Authorization: `Bearer ${process.env.CRON_SECRET}` } }));
  const sweep = (await r.json()) as { pruned?: number };
  check(r.status === 200 && sweep.pruned === 1, `the sweep prunes the month-old row with no runs (${JSON.stringify(sweep)})`);
  const kept = await db`select count(*) as n from players where id in (${fresh}, ${playerId})`;
  check(Number(kept[0].n) === 2, "and keeps the new one and the one that has played");
  // 11. deleting the online record on request
  const del = (await import("../app/api/player/delete/route")).POST;
  r = await del(post("/api/player/delete", {}, {}));
  check(r.status === 401, "delete needs a token");
  r = await del(post("/api/player/delete", {}, { Authorization: `Bearer ${token2}` }));
  const dl = (await r.json()) as { ok?: boolean; runs_deleted?: number };
  check(r.status === 200 && dl.runs_deleted === 4, `delete removes the player and its runs (${JSON.stringify(dl)})`);
  const after = await db`select (select count(*) from players where id = ${playerId}) as p, (select count(*) from runs where player_id = ${playerId}) as r`;
  check(Number(after[0].p) === 0 && Number(after[0].r) === 0, "the row and its runs are gone");

  await db.end();
  if (failures) { console.error(`${failures} failure(s)`); process.exit(1); }
  console.log("smoke: all passed");
}

main().catch((e) => { console.error(e); process.exit(1); });
