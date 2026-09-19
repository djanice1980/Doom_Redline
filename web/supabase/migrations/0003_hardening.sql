-- Hardening after Supabase's database linter (2026-09-19):
--  * views run as the caller, not the creator, so RLS on the tables applies to them
--  * the ratings function has a fixed search_path
--  * the REST API roles (anon, authenticated) can read nothing and call nothing in
--    the public schema; the service connects as the database owner through the pooler
alter view board_global set (security_invoker = on);
alter view board_week set (security_invoker = on);
alter view board_fights set (security_invoker = on);
alter view board_level set (security_invoker = on);
alter view board_kills set (security_invoker = on);
alter view public_stats set (security_invoker = on);

alter function recompute_ratings() set search_path = public;

do $$
begin
    if exists (select 1 from pg_roles where rolname = 'anon') then
        revoke all on all tables in schema public from anon;
        revoke all on all sequences in schema public from anon;
        revoke execute on all functions in schema public from anon;
        alter default privileges in schema public revoke all on tables from anon;
        alter default privileges in schema public revoke all on sequences from anon;
        alter default privileges in schema public revoke execute on functions from anon;
    end if;
    if exists (select 1 from pg_roles where rolname = 'authenticated') then
        revoke all on all tables in schema public from authenticated;
        revoke all on all sequences in schema public from authenticated;
        revoke execute on all functions in schema public from authenticated;
        alter default privileges in schema public revoke all on tables from authenticated;
        alter default privileges in schema public revoke all on sequences from authenticated;
        alter default privileges in schema public revoke execute on functions from authenticated;
    end if;
end $$;
