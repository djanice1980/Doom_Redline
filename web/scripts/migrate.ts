// Applies supabase/migrations/*.sql in order to DATABASE_URL (idempotent SQL).
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import postgres from "postgres";

const url = process.env.DATABASE_URL;
if (!url) { console.error("DATABASE_URL is not set"); process.exit(2); }
const sql = postgres(url, { max: 1, prepare: false, ssl: url.includes("localhost") || url.includes("127.0.0.1") ? false : "require" });
const dir = join(__dirname, "..", "supabase", "migrations");
(async () => {
  for (const f of readdirSync(dir).filter((f) => f.endsWith(".sql")).sort()) {
    process.stdout.write(`applying ${f}... `);
    await sql.unsafe(readFileSync(join(dir, f), "utf8"));
    console.log("ok");
  }
  await sql.end();
})().catch((e) => { console.error(e); process.exit(1); });
