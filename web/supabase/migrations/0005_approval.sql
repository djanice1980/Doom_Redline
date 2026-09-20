-- Consent moves from "nothing is stored until you confirm" to "stored from the
-- start, shown to nobody until you approve".
--
-- The game is given a token as soon as it asks to register, so finished games are
-- posted straight away. Every player therefore carries an `approved` flag, and the
-- public views (the boards, a player page, the stats page) only ever show approved
-- ones. Approving is one click in the email; declining deletes what was collected.
--
-- Permission belongs to the address, not the profile: a second profile made on a
-- machine this address has already approved is approved with it, and deleting a
-- profile takes its own permission away with it.
alter table players add column if not exists approved boolean not null default false;

-- Everything registered before this migration went through the old code or link
-- confirmation, so it is approved by definition.
update players set approved = true where token_hash is not null and account_id is not null;

create index if not exists players_approved_idx on players(approved) where approved;

-- The boards show approved players only. The column lists must stay exactly as
-- migration 0001 wrote them: "create or replace view" cannot rename or reorder.
create or replace view board_global as
select distinct on (r.player_id) r.player_id, p.display_name, r.score as value, r.level, r.red_lines, r.fights, r.kills, r.ended_at, r.id as run_id
from runs r join players p on p.id = r.player_id
where p.approved
order by r.player_id, r.score desc, r.ended_at asc;

create or replace view board_week as
select distinct on (r.player_id) r.player_id, p.display_name, r.score as value, r.level, r.red_lines, r.fights, r.kills, r.ended_at, r.id as run_id
from runs r join players p on p.id = r.player_id
where p.approved and r.submitted_at > now() - interval '7 days'
order by r.player_id, r.score desc, r.ended_at asc;

create or replace view board_fights as
select distinct on (r.player_id) r.player_id, p.display_name, r.fights as value, r.level, r.red_lines, r.fights, r.kills, r.ended_at, r.id as run_id
from runs r join players p on p.id = r.player_id
where p.approved
order by r.player_id, r.fights desc, r.score desc;

create or replace view board_level as
select distinct on (r.player_id) r.player_id, p.display_name, r.level as value, r.level, r.red_lines, r.fights, r.kills, r.ended_at, r.id as run_id
from runs r join players p on p.id = r.player_id
where p.approved
order by r.player_id, r.level desc, r.score desc;

create or replace view board_kills as
select distinct on (r.player_id) r.player_id, p.display_name, r.kills as value, r.level, r.red_lines, r.fights, r.kills, r.ended_at, r.id as run_id
from runs r join players p on p.id = r.player_id
where p.approved
order by r.player_id, r.kills desc, r.score desc;

-- Views inherit the caller's rights and nothing is granted to the REST API roles
-- (migration 0003), so these stay invisible outside our own routes either way.
alter view board_global set (security_invoker = on);
alter view board_week set (security_invoker = on);
alter view board_fights set (security_invoker = on);
alter view board_level set (security_invoker = on);
alter view board_kills set (security_invoker = on);

-- The stats page counts approved players and their runs only.
create or replace view public_stats as
select
    (select count(*) from players where approved) as players,
    (select count(*) from runs r join players p on p.id = r.player_id where p.approved) as runs,
    (select coalesce(sum(r.duration_s), 0) from runs r join players p on p.id = r.player_id where p.approved) as seconds_played,
    (select coalesce(sum(r.kills), 0) from runs r join players p on p.id = r.player_id where p.approved) as demons_slain,
    (select coalesce(sum(r.blocks_destroyed), 0) from runs r join players p on p.id = r.player_id where p.approved) as blocks_destroyed,
    (select count(*) from runs r join players p on p.id = r.player_id where p.approved and r.death_cause = 'killed') as deaths_in_fights,
    (select coalesce(avg(r.fights), 0) from runs r join players p on p.id = r.player_id where p.approved) as avg_fights,
    (select count(*) filter (where (r.input->>'gamepad')::int > coalesce((r.input->>'keyboard')::int, 0) + coalesce((r.input->>'mouse')::int, 0)) from runs r join players p on p.id = r.player_id where p.approved) as gamepad_runs,
    (select count(*) from runs r join players p on p.id = r.player_id where p.approved and r.voxels) as voxel_runs,
    (select count(*) from runs r join players p on p.id = r.player_id where p.approved and r.brutal) as brutal_runs;
alter view public_stats set (security_invoker = on);
