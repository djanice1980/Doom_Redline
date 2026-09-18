// The run record as the game sends it, with the sanity limits from
// docs/online-and-releases.md section 5. A record that fails is refused with a
// reason; one that passes is normalised into the columns of `runs`.
import { isUuid } from "./crypto";

export const MONSTER_KINDS = 13;
export const WEAPONS = ["shotgun", "chaingun", "rocket_launcher", "plasma_rifle"] as const;

export interface RunRow {
  id: string;
  player_id: string;
  install_id: string;
  started_at: string | null;
  ended_at: string | null;
  version: string;
  platform: string;
  duration_s: number;
  score: number;
  level: number;
  lines: number;
  red_lines: number;
  fights: number;
  pieces: number;
  tetrises: number;
  best_chain: number;
  kills: number;
  kills_by_kind: number[];
  highest_kind: number;
  blocks_destroyed: number;
  pickups: number;
  weapons_owned: string[];
  shots: Record<string, number>;
  damage_taken: number;
  bfg_used: boolean;
  death_cause: string;
  killed_by: number;
  seed: number;
  voxels: boolean;
  brutal: boolean;
  input: { keyboard: number; mouse: number; gamepad: number };
  trophies: string[];
}

function int(v: unknown, def = 0, min = 0, max = 1_000_000_000): number {
  const n = typeof v === "number" ? Math.trunc(v) : typeof v === "string" ? parseInt(v, 10) : NaN;
  if (!Number.isFinite(n)) return def;
  return Math.min(max, Math.max(min, n));
}
function num(v: unknown, def = 0, min = 0, max = 1e12): number {
  const n = typeof v === "number" ? v : typeof v === "string" ? parseFloat(v) : NaN;
  if (!Number.isFinite(n)) return def;
  return Math.min(max, Math.max(min, n));
}
function str(v: unknown, max = 64): string {
  return typeof v === "string" ? v.slice(0, max) : "";
}
function iso(v: unknown): string | null {
  if (typeof v !== "string") return null;
  const t = Date.parse(v);
  return Number.isFinite(t) ? new Date(t).toISOString() : null;
}

export function validateRun(body: unknown, playerId: string): { ok: true; row: RunRow } | { ok: false; error: string } {
  if (!body || typeof body !== "object") return { ok: false, error: "not an object" };
  const b = body as Record<string, unknown>;
  if (!isUuid(b.run_id)) return { ok: false, error: "run_id must be a uuid" };
  if (b.player_id !== playerId) return { ok: false, error: "player_id does not match the token" };
  if (!isUuid(b.install_id)) return { ok: false, error: "install_id must be a uuid" };
  const kk = Array.isArray(b.kills_by_kind) ? (b.kills_by_kind as unknown[]).slice(0, MONSTER_KINDS).map((x) => int(x, 0, 0, 100_000)) : [];
  while (kk.length < MONSTER_KINDS) kk.push(0);
  const kills = kk.reduce((a, c) => a + c, 0);
  const shots: Record<string, number> = {};
  if (b.shots && typeof b.shots === "object") for (const w of WEAPONS) shots[w] = int((b.shots as Record<string, unknown>)[w], 0, 0, 1_000_000);
  const weapons = Array.isArray(b.weapons_owned) ? (b.weapons_owned as unknown[]).filter((w): w is string => typeof w === "string" && (WEAPONS as readonly string[]).includes(w)) : [];
  const trophies = Array.isArray(b.trophies) ? (b.trophies as unknown[]).filter((t): t is string => typeof t === "string" && /^[a-z0-9_]{1,32}$/.test(t)).slice(0, 32) : [];
  const input = (b.input && typeof b.input === "object" ? b.input : {}) as Record<string, unknown>;
  const row: RunRow = {
    id: b.run_id,
    player_id: playerId,
    install_id: b.install_id,
    started_at: iso(b.started_at),
    ended_at: iso(b.ended_at),
    version: str(b.version, 32),
    platform: str(b.platform, 32),
    duration_s: num(b.duration_s, 0, 0, 86_400 * 7),
    score: int(b.score, 0, 0, 2_000_000_000),
    level: int(b.level, 1, 1, 1000),
    lines: int(b.lines, 0, 0, 1_000_000),
    red_lines: int(b.red_lines, 0, 0, 100_000),
    fights: int(b.fights, 0, 0, 100_000),
    pieces: int(b.pieces, 0, 0, 10_000_000),
    tetrises: int(b.tetrises, 0, 0, 1_000_000),
    best_chain: int(b.best_chain, 0, 0, 1000),
    kills,
    kills_by_kind: kk,
    highest_kind: int(b.highest_kind, -1, -1, MONSTER_KINDS - 1),
    blocks_destroyed: int(b.blocks_destroyed, 0, 0, 10_000_000),
    pickups: int(b.pickups, 0, 0, 1_000_000),
    weapons_owned: weapons,
    shots,
    damage_taken: num(b.damage_taken, 0, 0, 1e9),
    bfg_used: b.bfg_used === true,
    death_cause: ["stack", "killed", "quit"].includes(str(b.death_cause, 16)) ? str(b.death_cause, 16) : "stack",
    killed_by: int(b.killed_by, -1, -1, MONSTER_KINDS - 1),
    seed: int(b.seed, 0, 0, 4_294_967_295),
    voxels: b.voxels === true,
    brutal: b.brutal === true,
    input: { keyboard: int(input.keyboard, 0, 0, 1e8), mouse: int(input.mouse, 0, 0, 1e8), gamepad: int(input.gamepad, 0, 0, 1e8) },
    trophies,
  };
  // Sanity limits: anything a real run cannot do.
  const minutes = Math.max(0.05, row.duration_s / 60);
  if (row.score / minutes > 400_000) return { ok: false, error: "score per minute is not plausible" };
  if (row.level > 1 + row.fights + 1) return { ok: false, error: "level does not match the number of fights" };
  if (row.lines > row.pieces * 4 + 8) return { ok: false, error: "lines do not match the pieces placed" };
  if (row.tetrises * 4 > row.lines + 4) return { ok: false, error: "tetrises exceed lines" };
  if (row.fights > 0 && row.duration_s < row.fights * 3) return { ok: false, error: "fights too fast" };
  return { ok: true, row };
}
