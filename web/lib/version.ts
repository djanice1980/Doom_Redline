// The latest release and the changelog, for the game's update notice and its
// WHAT'S NEW screen. Both come from GitHub (the Releases API and CHANGELOG.md on
// main) and are cached in memory for three minutes, so a new release reaches the game
// within a few minutes of being published.
const REPO = "djanice1980/Doom_Redline";

export interface ChangelogEntry { version: string; date: string; items: string[] }
export interface VersionInfo { latest: string; url: string; published_at: string | null; changelog: ChangelogEntry[] }

let cache: { at: number; info: VersionInfo } | null = null;

// "## 0.2.0 (2026-09-18)" headings with "- " bullets under them; anything else is ignored.
export function parseChangelog(md: string): ChangelogEntry[] {
  const out: ChangelogEntry[] = [];
  let cur: ChangelogEntry | null = null;
  for (const raw of md.split(/\r?\n/)) {
    const h = /^##\s+v?(\d+\.\d+\.\d+)\s*(?:\(([^)]*)\))?/.exec(raw);
    if (h) { cur = { version: h[1], date: h[2] ?? "", items: [] }; out.push(cur); continue; }
    const b = /^\s*[-*]\s+(.*\S)\s*$/.exec(raw);
    if (b && cur) cur.items.push(b[1].replace(/`/g, "").replace(/\*\*/g, ""));
  }
  return out;
}

export async function versionInfo(): Promise<VersionInfo> {
  if (cache && Date.now() - cache.at < 3 * 60 * 1000) return cache.info;
  let latest = "", url = `https://github.com/${REPO}/releases/latest`, published: string | null = null;
  try {
    const r = await fetch(`https://api.github.com/repos/${REPO}/releases/latest`, { headers: { Accept: "application/vnd.github+json", "User-Agent": "redline-online" }, cache: "no-store" });
    if (r.ok) {
      const j = (await r.json()) as { tag_name?: string; html_url?: string; published_at?: string };
      latest = (j.tag_name ?? "").replace(/^v/, "");
      if (j.html_url) url = j.html_url;
      published = j.published_at ?? null;
    }
  } catch { /* GitHub unreachable: keep the defaults */ }
  let changelog: ChangelogEntry[] = [];
  try {
    const r = await fetch(`https://raw.githubusercontent.com/${REPO}/main/CHANGELOG.md`, { cache: "no-store" });
    if (r.ok) changelog = parseChangelog(await r.text());
  } catch { /* same */ }
  const info: VersionInfo = { latest, url, published_at: published, changelog };
  if (latest) cache = { at: Date.now(), info };
  return info;
}
