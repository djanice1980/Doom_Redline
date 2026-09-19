-- Quieting the last two kinds of Supabase linter findings. Both are cosmetic:
--  * rls_auto_enable() is Supabase's own helper (the "auto-enable RLS" feature);
--    functions are executable by PUBLIC by default, so the 0003 revoke for the
--    API roles left it callable through /rest/v1/rpc. Revoke from everyone; the
--    event trigger it serves runs as the owner and is unaffected.
--  * "RLS enabled, no policy" is exactly our design (no policy = no access), but
--    an explicit deny-everything policy states that intent and clears the note.
do $$
begin
    if exists (select 1 from pg_proc p join pg_namespace n on n.oid = p.pronamespace where n.nspname = 'public' and p.proname = 'rls_auto_enable') then
        execute 'revoke execute on function public.rls_auto_enable() from public';
        if exists (select 1 from pg_roles where rolname = 'anon') then execute 'revoke execute on function public.rls_auto_enable() from anon'; end if;
        if exists (select 1 from pg_roles where rolname = 'authenticated') then execute 'revoke execute on function public.rls_auto_enable() from authenticated'; end if;
    end if;
end $$;

do $$
declare t text;
begin
    foreach t in array array['accounts', 'players', 'machines', 'registrations', 'player_machines', 'runs', 'trophies', 'ratings', 'settings', 'mail_log', 'rate_limits'] loop
        if not exists (select 1 from pg_policies where schemaname = 'public' and tablename = t and policyname = 'deny_all') then
            execute format('create policy deny_all on public.%I for all using (false) with check (false)', t);
        end if;
    end loop;
end $$;
