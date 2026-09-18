-- REDLINE online service: schema (docs/online-and-releases.md sections 4, 7 and 9).
-- Run this in the Supabase SQL editor (or `npm run migrate` against DATABASE_URL).
-- Every table has row-level security on with no policies: nothing is readable
-- through Supabase's public REST API. Only the Vercel API routes, connecting
-- with the database password, touch these tables.

create extension if not exists pgcrypto;

-- One per verified email address. The address itself is never stored in the
-- clear: email_hash is HMAC-SHA256(lower(email), EMAIL_HMAC_KEY) for lookups,
-- email_enc is AES-256-GCM(email, DATA_KEY) so it can be shown back to its owner.
create table if not exists accounts (
    id          uuid primary key default gen_random_uuid(),
    email_hash  text not null unique,
    email_enc   text not null,
    created_at  timestamptz not null default now()
);

-- One per game profile. The id is the player_id the game minted; token_hash is
-- SHA-256 of the per-player secret handed out when a registration is confirmed.
create table if not exists players (
    id            uuid primary key,
    account_id    uuid references accounts(id) on delete cascade,
    display_name  text not null,
    token_hash    text,
    created_at    timestamptz not null default now(),
    last_seen     timestamptz not null default now()
);
create index if not exists players_account_idx on players(account_id);

-- One per install id (a save folder). The hardware facts are encrypted.
create table if not exists machines (
    install_id  uuid primary key,
    label       text not null default '',
    facts_enc   text,
    first_seen  timestamptz not null default now(),
    last_seen   timestamptz not null default now()
);

-- Consent records: every player + machine + address combination asks once.
create table if not exists registrations (
    id            uuid primary key default gen_random_uuid(),
    email_hash    text not null,
    email_enc     text not null,
    player_id     uuid not null,
    install_id    uuid not null,
    display_name  text not null,
    machine_label text not null default '',
    code_hash     text not null,
    attempts      int not null default 0,
    status        text not null default 'pending' check (status in ('pending', 'confirmed', 'declined', 'expired')),
    agreed_text_version int not null default 1,
    method        text,
    created_at    timestamptz not null default now(),
    expires_at    timestamptz not null default now() + interval '24 hours',
    confirmed_at  timestamptz
);
create index if not exists registrations_player_idx on registrations(player_id, created_at desc);
create index if not exists registrations_email_idx on registrations(email_hash, created_at desc);

-- Which machines a player has been confirmed on.
create table if not exists player_machines (
    player_id   uuid not null references players(id) on delete cascade,
    install_id  uuid not null references machines(install_id) on delete cascade,
    confirmed_at timestamptz not null default now(),
    primary key (player_id, install_id)
);

-- One per finished game, as the game's run record.
create table if not exists runs (
    id             uuid primary key,
    player_id      uuid not null references players(id) on delete cascade,
    install_id     uuid not null,
    submitted_at   timestamptz not null default now(),
    started_at     timestamptz,
    ended_at       timestamptz,
    version        text not null default '',
    platform       text not null default '',
    duration_s     double precision not null default 0,
    score          bigint not null default 0,
    level          int not null default 1,
    lines          int not null default 0,
    red_lines      int not null default 0,
    fights         int not null default 0,
    pieces         int not null default 0,
    tetrises       int not null default 0,
    best_chain     int not null default 0,
    kills          int not null default 0,
    kills_by_kind  jsonb not null default '[]'::jsonb,
    highest_kind   int not null default -1,
    blocks_destroyed int not null default 0,
    pickups        int not null default 0,
    weapons_owned  jsonb not null default '[]'::jsonb,
    shots          jsonb not null default '{}'::jsonb,
    damage_taken   double precision not null default 0,
    bfg_used       boolean not null default false,
    death_cause    text not null default 'stack',
    killed_by      int not null default -1,
    seed           bigint not null default 0,
    voxels         boolean not null default false,
    brutal         boolean not null default false,
    input          jsonb not null default '{}'::jsonb,
    trophies       jsonb not null default '[]'::jsonb,
    verified       boolean not null default false
);
create index if not exists runs_player_idx on runs(player_id, submitted_at desc);
create index if not exists runs_score_idx on runs(score desc);
create index if not exists runs_submitted_idx on runs(submitted_at desc);

-- Trophies as the server has seen them unlocked (from run records).
create table if not exists trophies (
    player_id    uuid not null references players(id) on delete cascade,
    trophy_id    text not null,
    unlocked_at  timestamptz not null default now(),
    primary key (player_id, trophy_id)
);

-- Rating and tier per player, recomputed by the cron route (section 7).
create table if not exists ratings (
    player_id    uuid primary key references players(id) on delete cascade,
    rating       double precision not null default 0,
    best_score   bigint not null default 0,
    median_recent bigint not null default 0,
    runs_count   int not null default 0,
    rank         int,
    tier         text not null default 'ZOMBIE',
    computed_at  timestamptz not null default now()
);

-- Admin settings (the Microsoft Graph credentials), values encrypted with DATA_KEY.
create table if not exists settings (
    key         text primary key,
    value_enc   text not null,
    updated_at  timestamptz not null default now()
);

-- Every mail the service tried to send, for the admin page.
create table if not exists mail_log (
    id          bigserial primary key,
    to_hash     text not null,
    subject     text not null,
    status      int,
    detail      text,
    sent_at     timestamptz not null default now()
);

-- Rate limiting buckets (key = route:subject, window start).
create table if not exists rate_limits (
    key         text not null,
    window_start timestamptz not null,
    count       int not null default 0,
    primary key (key, window_start)
);

-- Lock everything away from the public API.
alter table accounts enable row level security;
alter table players enable row level security;
alter table machines enable row level security;
alter table registrations enable row level security;
alter table player_machines enable row level security;
alter table runs enable row level security;
alter table trophies enable row level security;
alter table ratings enable row level security;
alter table settings enable row level security;
alter table mail_log enable row level security;
alter table rate_limits enable row level security;

-- Boards: one row per player, their best by the board's measure.
create or replace view board_global as
select distinct on (r.player_id) r.player_id, p.display_name, r.score as value, r.level, r.red_lines, r.fights, r.kills, r.ended_at, r.id as run_id
from runs r join players p on p.id = r.player_id
order by r.player_id, r.score desc, r.ended_at asc;

create or replace view board_week as
select distinct on (r.player_id) r.player_id, p.display_name, r.score as value, r.level, r.red_lines, r.fights, r.kills, r.ended_at, r.id as run_id
from runs r join players p on p.id = r.player_id
where r.submitted_at > now() - interval '7 days'
order by r.player_id, r.score desc, r.ended_at asc;

create or replace view board_fights as
select distinct on (r.player_id) r.player_id, p.display_name, r.fights as value, r.level, r.red_lines, r.fights, r.kills, r.ended_at, r.id as run_id
from runs r join players p on p.id = r.player_id
order by r.player_id, r.fights desc, r.score desc;

create or replace view board_level as
select distinct on (r.player_id) r.player_id, p.display_name, r.level as value, r.level, r.red_lines, r.fights, r.kills, r.ended_at, r.id as run_id
from runs r join players p on p.id = r.player_id
order by r.player_id, r.level desc, r.score desc;

create or replace view board_kills as
select distinct on (r.player_id) r.player_id, p.display_name, r.kills as value, r.level, r.red_lines, r.fights, r.kills, r.ended_at, r.id as run_id
from runs r join players p on p.id = r.player_id
order by r.player_id, r.kills desc, r.score desc;

-- Rating: best score (60 %) + median of the last ten runs (40 %); tiers by percentile.
create or replace function recompute_ratings() returns int language plpgsql as $$
declare n int;
begin
    insert into ratings (player_id, rating, best_score, median_recent, runs_count, computed_at)
    select s.player_id, 0.6 * s.best + 0.4 * s.med, s.best, s.med, s.cnt, now()
    from (
        select r.player_id,
               max(r.score) as best,
               coalesce((select percentile_cont(0.5) within group (order by x.score) from (select score from runs where player_id = r.player_id order by submitted_at desc limit 10) x), 0)::bigint as med,
               count(*) as cnt
        from runs r group by r.player_id
    ) s
    on conflict (player_id) do update set rating = excluded.rating, best_score = excluded.best_score, median_recent = excluded.median_recent, runs_count = excluded.runs_count, computed_at = now();
    with ranked as (
        select player_id, rank() over (order by rating desc) as rk, count(*) over () as total from ratings
    )
    update ratings g set rank = ranked.rk,
        tier = case
            when ranked.rk = 1 then 'DOOM SLAYER'
            when ranked.rk <= greatest(1, ceil(ranked.total * 0.02)) then 'SPIDER MASTERMIND'
            when ranked.rk <= greatest(1, ceil(ranked.total * 0.07)) then 'CYBERDEMON'
            when ranked.rk <= greatest(1, ceil(ranked.total * 0.15)) then 'BARON'
            when ranked.rk <= greatest(1, ceil(ranked.total * 0.25)) then 'CACODEMON'
            when ranked.rk <= greatest(1, ceil(ranked.total * 0.40)) then 'DEMON'
            when ranked.rk <= greatest(1, ceil(ranked.total * 0.60)) then 'IMP'
            else 'ZOMBIE' end
    from ranked where ranked.player_id = g.player_id;
    select count(*) into n from ratings;
    return n;
end $$;

-- Aggregate statistics for the public stats page, without touching encrypted rows.
create or replace view public_stats as
select
    (select count(*) from players where token_hash is not null) as players,
    (select count(*) from runs) as runs,
    (select coalesce(sum(duration_s), 0) from runs) as seconds_played,
    (select coalesce(sum(kills), 0) from runs) as demons_slain,
    (select coalesce(sum(blocks_destroyed), 0) from runs) as blocks_destroyed,
    (select count(*) from runs where death_cause = 'killed') as deaths_in_fights,
    (select coalesce(avg(fights), 0) from runs) as avg_fights,
    (select count(*) filter (where (input->>'gamepad')::int > coalesce((input->>'keyboard')::int, 0) + coalesce((input->>'mouse')::int, 0)) from runs) as gamepad_runs,
    (select count(*) from runs where voxels) as voxel_runs,
    (select count(*) from runs where brutal) as brutal_runs;
