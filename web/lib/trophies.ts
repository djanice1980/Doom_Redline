// The game's trophy catalogue (src/game/trophies.cpp), for the pages.
export const TROPHIES: { id: string; name: string; description: string }[] = [
  { id: "first_blood", name: "FIRST BLOOD", description: "Kill your first demon" },
  { id: "red_line", name: "RED LINE", description: "Survive a fight" },
  { id: "tetris", name: "TETRIS", description: "Clear four lines at once" },
  { id: "combo3", name: "ON A ROLL", description: "Three clearing pieces in a row" },
  { id: "chain", name: "CHAIN REACTION", description: "A clear caused by the collapse" },
  { id: "untouchable", name: "UNTOUCHABLE", description: "Win a fight without taking damage" },
  { id: "boss", name: "BOSS KILLER", description: "Kill a baron or bigger" },
  { id: "cyber", name: "CYBER SLAYER", description: "Kill a cyberdemon" },
  { id: "mastermind", name: "MASTERMIND", description: "Kill a spider mastermind" },
  { id: "arsenal", name: "FULL ARSENAL", description: "Own all four weapons" },
  { id: "bfg", name: "PANIC BUTTON", description: "Fire the BFG9000" },
  { id: "invuln", name: "GOLDEN", description: "Win the invulnerability roll" },
  { id: "level5", name: "VETERAN", description: "Reach level 5" },
  { id: "level10", name: "DOOMED", description: "Reach level 10" },
  { id: "survivor", name: "SURVIVOR", description: "Survive five fights in one game" },
  { id: "demolition", name: "DEMOLITION", description: "Destroy 50 blocks in one game" },
  { id: "collector", name: "COLLECTOR", description: "Pick up 20 items in one game" },
  { id: "grown", name: "TOO SLOW", description: "Kill a demon that has grown" },
  { id: "dungeon", name: "DUNGEON CRAWLER", description: "Slay a dungeon boss" },
  { id: "doom_slayer", name: "DOOM SLAYER!", description: "Set a new high score" },
  { id: "rip_and_tear", name: "RIP AND TEAR!!!", description: "Earn every other trophy" },
];

export const MONSTER_NAMES = ["Zombieman", "Imp", "Demon", "Cacodemon", "Baron", "Cyberdemon", "Spider Mastermind", "Chaingunner", "Hell Knight", "Revenant", "Mancubus", "Arachnotron", "Arch-vile"];

export function formatDuration(seconds: number): string {
  const s = Math.max(0, Math.round(seconds));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
  if (h > 0) return `${h}h ${String(m).padStart(2, "0")}m`;
  if (m > 0) return `${m}m ${String(s % 60).padStart(2, "0")}s`;
  return `${s}s`;
}

export function formatDate(iso: string | null): string {
  if (!iso) return "";
  return new Date(iso).toISOString().slice(0, 10);
}
