// One Postgres connection pool for the API routes. DATABASE_URL is the Supabase
// "transaction" pooler string (port 6543) in production, or a plain local
// Postgres for the smoke test. Nothing else in the service talks to the database.
import postgres from "postgres";

declare global {
  // eslint-disable-next-line no-var
  var __redlineSql: ReturnType<typeof postgres> | undefined;
}

export function sql() {
  if (!globalThis.__redlineSql) {
    const url = process.env.DATABASE_URL;
    if (!url) throw new Error("DATABASE_URL is not set");
    globalThis.__redlineSql = postgres(url, {
      max: 4,
      idle_timeout: 20,
      prepare: false, // Supabase's transaction pooler does not support prepared statements
      ssl: url.includes("localhost") || url.includes("127.0.0.1") ? false : "require",
    });
  }
  return globalThis.__redlineSql;
}
