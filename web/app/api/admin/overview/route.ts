// GET /api/admin/overview: the owner's view of the service, which is deliberately
// more than the public one. The public pages show approved players only; this shows
// everything that has been collected, including players still waiting on their
// email, so it is possible to tell whether anyone is actually playing.
//
// Behind ADMIN_PASSWORD (HTTP Basic, see middleware.ts). Addresses are shown in
// full, which is safe to do because of how they are kept: the database holds only
// AES-256-GCM ciphertext plus a one-way HMAC used for lookup, and the key that
// undoes it lives in the environment, not in the database. Reading one back is a
// real decryption by a request that passed the admin password, not a lookup.
import { sql } from "@/lib/db";
import { decrypt } from "@/lib/crypto";
import { adminChallenge, isAdmin, json } from "@/lib/auth";
import { ensureSchema } from "@/lib/schema";

export const runtime = "nodejs";

export async function GET(req: Request) {
  if (!isAdmin(req)) return adminChallenge();
  await ensureSchema();
  const db = sql();

  const [totals] = await db`select
      (select count(*) from players) as players,
      (select count(*) from players where approved) as approved,
      (select count(*) from accounts) as accounts,
      (select count(*) from machines) as machines,
      (select count(*) from runs) as runs,
      (select count(*) from runs r join players p on p.id = r.player_id where not p.approved) as hidden_runs,
      (select coalesce(sum(duration_s), 0) from runs) as seconds,
      (select coalesce(sum(kills), 0) from runs) as kills,
      (select coalesce(sum(blocks_destroyed), 0) from runs) as blocks,
      (select coalesce(max(score), 0) from runs) as best_score,
      (select count(*) from runs where submitted_at > now() - interval '24 hours') as runs_today,
      (select count(*) from runs where submitted_at > now() - interval '7 days') as runs_week`;

  const registrations = await db`select status, count(*) as n from registrations group by status order by n desc`;
  const daily = await db`select to_char(date_trunc('day', submitted_at), 'YYYY-MM-DD') as day, count(*) as runs,
      count(distinct player_id) as players
    from runs where submitted_at > now() - interval '14 days'
    group by 1 order by 1`;
  const versions = await db`select version, count(*) as runs, count(distinct player_id) as players, max(submitted_at) as last
    from runs group by version order by last desc`;
  const platforms = await db`select platform, count(*) as runs from runs group by platform order by runs desc`;

  // Players, approved or not, with what they have done. No addresses here: a player
  // is not an account, and several players can share one.
  const players = await db`select p.id, p.display_name, p.approved, p.created_at, p.last_seen,
      (select count(*) from runs r where r.player_id = p.id) as runs,
      (select coalesce(max(score), 0) from runs r where r.player_id = p.id) as best,
      (select coalesce(sum(duration_s), 0) from runs r where r.player_id = p.id) as seconds,
      (select count(*) from player_machines m where m.player_id = p.id) as machines,
      (select count(*) from trophies t where t.player_id = p.id) as trophies
    from players p order by p.last_seen desc limit 200`;

  // Accounts with their address and what hangs off each. Decrypting here is the only
  // way to read one: the column holds ciphertext, and the key is in the environment.
  const accountRows = await db`select a.id, a.email_enc, a.created_at,
      (select count(*) from players p where p.account_id = a.id) as players,
      (select coalesce(sum((select count(*) from runs r where r.player_id = p.id)), 0) from players p where p.account_id = a.id) as runs,
      (select max(p.last_seen) from players p where p.account_id = a.id) as last_seen
    from accounts a order by a.created_at desc limit 200`;
  const accounts = accountRows.map((a) => {
    let email = "";
    try { email = decrypt(a.email_enc as string); } catch { email = ""; }
    return {
      id: a.id as string, email: email || "unreadable", players: Number(a.players), runs: Number(a.runs),
      created_at: new Date(a.created_at as string).toISOString(),
      last_seen: a.last_seen ? new Date(a.last_seen as string).toISOString() : "",
    };
  });

  // Registrations still waiting, so it is clear who is one click from appearing.
  const waitingRows = await db`select r.id, r.display_name, r.machine_label, r.email_enc, r.created_at, r.expires_at, r.player_id,
      (select count(*) from runs x where x.player_id = r.player_id) as runs
    from registrations r where r.status = 'pending' order by r.created_at desc limit 50`;
  const waiting = waitingRows.map((w) => {
    let email = "";
    try { email = decrypt(w.email_enc as string); } catch { email = ""; }
    return {
      id: w.id as string, player_id: w.player_id as string, name: w.display_name as string, machine: (w.machine_label as string) || "",
      email: email || "unreadable", runs: Number(w.runs),
      created_at: new Date(w.created_at as string).toISOString(), expires_at: new Date(w.expires_at as string).toISOString(),
    };
  });

  // The machine facts are stored encrypted and never shown per player anywhere else.
  // The owner gets them in aggregate: what people are actually running the game on.
  const machineRows = await db`select facts_enc from machines where facts_enc is not null limit 500`;
  const os: Record<string, number> = {}, gpu: Record<string, number> = {}, pads: Record<string, number> = {};
  let cores = 0, ram = 0, counted = 0;
  for (const m of machineRows) {
    try {
      const f = JSON.parse(decrypt(m.facts_enc as string)) as { os?: string; gpu?: string; cores?: number; ram?: number; pad?: string };
      if (f.os) os[f.os] = (os[f.os] ?? 0) + 1;
      if (f.gpu) gpu[f.gpu] = (gpu[f.gpu] ?? 0) + 1;
      if (f.pad) pads[f.pad] = (pads[f.pad] ?? 0) + 1;
      if (f.cores) cores += f.cores;
      if (f.ram) ram += f.ram;
      counted++;
    } catch { /* a row written with another key: skip it */ }
  }
  const top = (r: Record<string, number>) => Object.entries(r).sort((a, b) => b[1] - a[1]).slice(0, 12).map(([name, n]) => ({ name, n }));

  const recent = await db`select r.id, r.submitted_at, r.ended_at, r.score, r.level, r.fights, r.kills, r.red_lines, r.duration_s,
      r.version, r.platform, r.death_cause, r.voxels, r.brutal, p.display_name, p.approved
    from runs r join players p on p.id = r.player_id order by r.submitted_at desc limit 40`;

  const trophyRows = await db`select trophy_id, count(*) as n from trophies group by trophy_id order by n desc`;

  return json({
    totals: {
      players: Number(totals.players), approved: Number(totals.approved), waiting: Number(totals.players) - Number(totals.approved),
      accounts: Number(totals.accounts), machines: Number(totals.machines),
      runs: Number(totals.runs), hidden_runs: Number(totals.hidden_runs),
      seconds: Number(totals.seconds), kills: Number(totals.kills), blocks: Number(totals.blocks), best_score: Number(totals.best_score),
      runs_today: Number(totals.runs_today), runs_week: Number(totals.runs_week),
    },
    registrations: registrations.map((r) => ({ status: r.status as string, n: Number(r.n) })),
    daily: daily.map((d) => ({ day: d.day as string, runs: Number(d.runs), players: Number(d.players) })),
    versions: versions.map((v) => ({ version: (v.version as string) || "unknown", runs: Number(v.runs), players: Number(v.players), last: new Date(v.last as string).toISOString() })),
    platforms: platforms.map((p) => ({ platform: (p.platform as string) || "unknown", runs: Number(p.runs) })),
    players: players.map((p) => ({
      id: p.id as string, name: p.display_name as string, approved: p.approved === true,
      runs: Number(p.runs), best: Number(p.best), seconds: Number(p.seconds), machines: Number(p.machines), trophies: Number(p.trophies),
      created_at: new Date(p.created_at as string).toISOString(), last_seen: new Date(p.last_seen as string).toISOString(),
    })),
    accounts,
    waiting,
    hardware: { counted, os: top(os), gpu: top(gpu), pads: top(pads), avg_cores: counted ? cores / counted : 0, avg_ram_gb: counted ? ram / counted / 1024 : 0 },
    recent: recent.map((r) => ({
      id: r.id as string, at: new Date(r.submitted_at as string).toISOString(), name: r.display_name as string, approved: r.approved === true,
      score: Number(r.score), level: Number(r.level), fights: Number(r.fights), kills: Number(r.kills), red_lines: Number(r.red_lines),
      duration_s: Number(r.duration_s), version: (r.version as string) || "", platform: (r.platform as string) || "",
      death_cause: r.death_cause as string, voxels: r.voxels === true, brutal: r.brutal === true,
    })),
    trophies: trophyRows.map((t) => ({ id: t.trophy_id as string, n: Number(t.n) })),
  });
}
