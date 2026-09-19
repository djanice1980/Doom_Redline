// Columns added after 0001 are ensured on first use, so a deployment never
// breaks on a database that has not had the newest migration run by hand yet.
// Everything here is idempotent and cheap; it runs once per process.
import { sql } from "./db";

let ensured: Promise<void> | null = null;

export function ensureSchema(): Promise<void> {
  if (!ensured) {
    ensured = (async () => {
      const db = sql();
      await db.unsafe(`
        alter table registrations add column if not exists link_hash text;
        alter table registrations add column if not exists poll_hash text;
        alter table registrations add column if not exists token_enc text;
      `);
      // 0003: views as the caller, fixed search_path, nothing for the REST API roles.
      await db.unsafe(`
        alter view board_global set (security_invoker = on);
        alter view board_week set (security_invoker = on);
        alter view board_fights set (security_invoker = on);
        alter view board_level set (security_invoker = on);
        alter view board_kills set (security_invoker = on);
        alter view public_stats set (security_invoker = on);
        alter function recompute_ratings() set search_path = public;
        do $$ begin
          if exists (select 1 from pg_roles where rolname = 'anon') then
            revoke all on all tables in schema public from anon;
            revoke all on all sequences in schema public from anon;
            revoke execute on all functions in schema public from anon;
          end if;
          if exists (select 1 from pg_roles where rolname = 'authenticated') then
            revoke all on all tables in schema public from authenticated;
            revoke all on all sequences in schema public from authenticated;
            revoke execute on all functions in schema public from authenticated;
          end if;
        end $$;
      `);
      // 0004: nothing callable through the REST API's rpc, and explicit deny-all policies.
      await db.unsafe(`
        do $$ begin
          if exists (select 1 from pg_proc p join pg_namespace n on n.oid = p.pronamespace where n.nspname = 'public' and p.proname = 'rls_auto_enable') then
            execute 'revoke execute on function public.rls_auto_enable() from public';
          end if;
        end $$;
        do $$ declare t text; begin
          foreach t in array array['accounts', 'players', 'machines', 'registrations', 'player_machines', 'runs', 'trophies', 'ratings', 'settings', 'mail_log', 'rate_limits'] loop
            if not exists (select 1 from pg_policies where schemaname = 'public' and tablename = t and policyname = 'deny_all') then
              execute format('create policy deny_all on public.%I for all using (false) with check (false)', t);
            end if;
          end loop;
        end $$;
      `);
    })().catch((e) => { ensured = null; throw e; });
  }
  return ensured;
}
