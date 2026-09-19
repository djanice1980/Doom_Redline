-- Approval by link and pick-up of the token by the game (docs/online-and-releases.md section 9).
--   link_hash  SHA-256 of the secret in the emailed approve link
--   poll_hash  SHA-256 of the secret the game got back from /api/register, used to poll
--   token_enc  the player's token, encrypted, parked here after a link approval until the game collects it
alter table registrations add column if not exists link_hash text;
alter table registrations add column if not exists poll_hash text;
alter table registrations add column if not exists token_enc text;
