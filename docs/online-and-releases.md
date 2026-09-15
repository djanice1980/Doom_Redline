# Notes: GitHub releases and an online leaderboard

Written 2026-09-15 as an assessment before any of this work starts. Short
answers first, details below.

| Question | Answer |
|---|---|
| Is the project ready to push to GitHub? | Yes; the clean-up is done (section 1). No Doom data or secrets are tracked; the repo is under 2 MB of tracked files. |
| Is it scoped and ready for a web leaderboard? | The game side is *not* yet: it has no network code, no player identity beyond a typed name, and no per-run record. All of that is a bounded amount of work (about a day) and is listed below. |
| Is enough information available to publish? | Almost. Score, level, lines, red lines, trophies with dates, kills, blocks destroyed, pickups, fight time and damage are already counted. Missing: kills by monster class, run duration, weapons used, cause of death, the seed, the game version and a per-install id. All are one-liners to add. |
| Can we gather it appropriately? | Yes. Everything is gameplay statistics plus the name the player typed. No email or system information is needed. Make it opt-in and say what is sent. |
| Will GitHub publish a Windows installer and a CachyOS installer? | Yes. GitHub Actions can build both on every tagged release: `redline-x.y.z-setup.exe` (Inno Setup is on the Windows runners) and `redline-x.y.z-1-x86_64.pkg.tar.zst` (built in an Arch container, installs on CachyOS with `pacman -U`). An AppImage for other distros is a third job. GitHub Releases hosts the files for free. |

## 1. Pushing to GitHub

Clean-up done 2026-09-15: the build-story deck and its generator are
untracked (side project; they stay on disk under `docs/` and `tools/` and are
git-ignored), no machine paths remain in tracked files, and the README states
that the game never touches the network. `.gitignore` excludes `build/`,
`*.wad`, `wads/`; the Voxel Doom pack is found in `~/Downloads`, never copied.
Ready to `git remote add origin ... && git push -u origin master`.

## 2. Releases from GitHub Actions

One workflow, `.github/workflows/release.yml`, triggered by tags matching
`v*`, with three jobs uploading to the same GitHub Release:

**Windows** (`windows-latest`): install the Vulkan SDK (the LunarG installer
supports `--accept-licenses --default-answer --confirm-command install` for
unattended runs; cache `C:\VulkanSDK`), `vcpkg` comes preinstalled with
`VCPKG_ROOT` set, then exactly the steps in `docs/packaging.md`:
`cmake --preset windows-release`, build, `ctest`, `cmake --install`, then
`ISCC.exe packaging\windows\redline.iss`. Upload `build-win\redline-*-setup.exe`.
Expect the first run to surface Windows compile errors, since the Windows
build has never been compiled; fix them in the code, not the workflow.

**Arch / CachyOS** (`ubuntu-latest` running `container: archlinux:base-devel`):
`pacman -Syu --noconfirm cmake ninja sdl3 glm vulkan-headers vulkan-icd-loader shaderc libvorbis pkgconf git`,
create a non-root user (`makepkg` refuses root), `cd packaging/arch && makepkg -s --noconfirm`,
upload the `.pkg.tar.zst`. CachyOS is Arch; the package installs with
`sudo pacman -U redline-*.pkg.tar.zst`. The PKGBUILD depends on `sdl3`, which
is in the Arch repos since 2025.

**Other Linux** (`ubuntu-24.04`): Ubuntu 24.04 has no `libsdl3-dev`, so
either use `ubuntu-25.04`/later runners when available or build SDL3 from
source in the job, then `cpack -G TGZ` and, better, an AppImage with
`linuxdeploy` so SDL3 and libvorbis travel with the binary. Skip `.deb`
until a runner has SDL3 packages; a `.deb` that depends on a library the
distro does not have is worse than a tarball.

Release notes: `softprops/action-gh-release` with `generate_release_notes: true`
and the three artefacts. Version comes from the tag; wire `PROJECT_VERSION`
into the code (a generated `version.h`) so the title screen, the log and the
leaderboard submissions all report the same string.

## 3. Leaderboard: what exists, what is missing

Already counted per run (`src/game/app.cpp`, `highscores.h`, `trophies.h`):
score, level, lines, red lines survived, trophies with unlock dates, per-fight
kills / blocks destroyed / pickups, per-level card (kills, blocks, seconds,
damage taken, score), blocks destroyed and pickups per game, the seed
(`opts_.seed` or the time-based one in `newGame`), BFG used, died in FPS or
in the stack, profile name.

Missing, all cheap to add in `App`:
- kills by monster tier (seven counters) and the highest tier killed
- run duration in seconds, number of fights, pieces placed, tetrises, best chain
- weapons picked up, shots fired per weapon (accuracy is a nice stat)
- cause of game over (stack overflow vs. killed, and by what)
- game version, platform (`linux`/`windows`), and whether voxels were on
- a random install id (UUID written once to `<pref>/install.txt`), so one
  player's runs can be grouped without an account

Package all of it into one `RunRecord` struct filled at game over, written as
JSON to `<pref>/runs/<timestamp>.json` **regardless** of online status. That
file is the unit of submission and doubles as a local history for a stats
page in the game later.

## 4. Leaderboard: proposed shape

- **Supabase** (free tier): Postgres with tables `players` (install_id uuid
  primary key, display_name, created_at, country optional), `runs` (id, player,
  submitted_at, version, platform, score, level, lines, red_lines, fights,
  duration_s, kills jsonb by tier, blocks, pickups, damage, bfg, death_cause,
  seed, voxels), `trophies` (player, trophy_id, unlocked_at). Row-level
  security on: **no direct inserts from the game**. Views for the boards:
  top scores all-time / this week, most fights survived, fastest to level 10,
  trophy completion percentage per trophy.
- **Vercel** (free tier): a Next.js site with `/` (top 100), `/player/[id]`,
  `/trophies`, `/stats`, and one API route `POST /api/submit` that holds the
  Supabase service key, validates the JSON against a schema, applies sanity
  limits (score per minute, level vs. fights, lines vs. duration), rate-limits
  by install id and IP, and inserts. The game only ever talks to this route.
  Supabase Edge Functions can host the same logic if you prefer everything in
  one place; Vercel is the better fit for the pages.
- **Domain**: `redline-<something>.vercel.app` is free and fine to start. A
  real domain is about $10/year (Cloudflare or Porkbun); genuinely free ones
  (`eu.org`, `is-a.dev`) take weeks and look odd on a leaderboard. Vercel
  handles the certificate either way.
- **Game side**: an HTTP client. SDL3 has none. `libcurl` is the standard
  choice on both platforms (`curl` in vcpkg; `curl` is already on every Arch
  system). Submit at game over on a background thread; on failure leave the
  JSON in `<pref>/runs/pending/` and retry next launch. OPTIONS gains
  `ONLINE LEADERBOARD: OFF/ON` (default OFF) and the first game over asks once.
  Show the player's rank on the game-over screen when the submit succeeds.

## 5. Fairness and privacy

- Anything the client sends can be forged; a shared secret in the binary only
  deters casual edits. Two real mitigations are available here and worth
  noting: the block phase is fully deterministic from the seed and the input
  log (the rules engine in `src/core` has no dependencies and could be
  compiled to WebAssembly for server-side replay), while the FPS phase is
  not deterministic (frame-time driven), so replay verification would cover
  lines, red lines and level progression but not kills. Server-side sanity
  limits plus a "verified" badge for replay-checked runs is a reasonable
  first design; ban-by-install-id handles the rest.
- Collect only what is listed above. The display name is the profile name the
  player typed; add a profanity filter server-side. No email, no hardware
  identifiers, no IP stored beyond rate limiting. Say all of this in one
  sentence on the opt-in prompt and in the README.

## 6. Telemetry: what can be collected, and what should be

Technically almost everything is one call away, most of it through SDL3 and
Vulkan which the game already links. Whether it *should* be collected is a
separate question; the middle column is my recommendation.

| Data | How | Collect? |
|---|---|---|
| Input method (keyboard+mouse vs. gamepad, and the mix) | count which device produced moves, rotates, drops and shots per run; the game already routes both | Yes: this is the question you actually asked and it is not personal |
| Gamepad model | `SDL_GetGamepadName`, vendor/product ids | Yes (model only) |
| OS family and version | `SDL_GetPlatform`; `/etc/os-release` on Linux (distro name, e.g. CachyOS); `RtlGetVersion` or `SDL_GetVersion` plus the registry on Windows | Yes, family + version, no build numbers or hostnames |
| GPU model, driver version, Vulkan version | already read from `VkPhysicalDeviceProperties` for the log | Yes |
| CPU core count, RAM | `SDL_GetNumLogicalCPUCores`, `SDL_GetSystemRAM` | Yes, bucketed (8 GB / 16 GB / 32 GB+) |
| Display resolution, refresh, fullscreen mode, VSync | already known to the app | Yes |
| Voxels on/off, music set, sound on/off, resolution chosen | settings | Yes |
| Language / country | `SDL_GetPreferredLocales` | Coarse only (language, country) |
| Session length, launches, crashes | app timers; a crash marker file left at start and cleared at clean exit | Yes |
| Logged-in user name | `getenv("USER")` / `USERNAME` | **No.** It is personal data, it is often a real name, and it adds nothing the install id does not already give you. The typed profile name is the only name that should ever leave the machine. |
| Hostname, MAC address, serial numbers, IP | trivial | **No.** Hardware identifiers are the definition of tracking; the random install id groups a player's runs without any of them. Keep the IP for rate limiting only and do not store it. |
| Steam account, file paths, the WAD location | known to the app | **No.** Paths contain the user name and say where their games live. |

Mechanics: add a `SessionRecord` (machine profile above, written once per
launch) next to the `RunRecord` from section 3, both JSON in the save folder,
both shown to the player under an OPTIONS > DATA screen with a "SEND
GAMEPLAY STATS: OFF/ON" toggle. Send only when ON. Treat the machine profile
as a dimension of the leaderboard (filter by GPU, OS, input method) rather
than as a per-person log; aggregate views such as "38% of runs on a gamepad"
are the useful output. This keeps you on the right side of GDPR-style rules
if anyone in the EU plays, and it is exactly what Steam's hardware survey
does: hardware and software facts, opt-in, no identity.

## 7. Identity: why not email as the profile id

Proposed 2026-09-15: make the email address the real profile id, show the
typed name, and publish everything. Recommendation: **do not key on email.**

- An email typed into a game is not verified. Anyone can enter someone else's
  address and play under their record, or fill the board with fake accounts.
  Email only means something once a login exists (Supabase Auth's magic link
  does this well, and that is the moment to ask for it, not before).
- A public dataset keyed by email is a spam and harassment list. It is also
  personal data under GDPR / CCPA with the strongest obligations attached:
  purpose limitation, deletion on request, breach notification. A random id
  carries none of that.
- The game is offline-first. A profile has to exist before the network does.

What gives the same outcome safely:

- **`player_id`**: a random UUID minted when a profile is created, stored in
  the profile folder, never shown. This is the primary key everywhere.
- **`install_id`**: a random UUID per machine in the save folder. Machines
  are a *dimension* of a player's data (this run was on machine 3, on a
  gamepad, for 40 minutes), not identities.
- **Account (later, optional)**: email + magic link through Supabase Auth
  links one or more `player_id`s to an account so progress follows the
  player. The email is a login credential held by the auth provider, stored
  hashed, never in the public tables, never shown. Public pages show the
  display name and gameplay statistics only. Cross-machine sync then means
  the server holds the profile (scores, trophies, settings) and a new
  machine's `player_id` is merged into it on first login.

Counterpoints raised the same day: the address will be verified before it
counts, emails are never published, and the profile + address exist locally
first. Agreed: that is an account, and it is fine. The one implementation
detail kept is that the database key is the random `player_id`, with the
verified email as a unique login attribute (which is also how Supabase Auth
models users), so a player changing address keeps their history.

**Prep done 2026-09-15** (`src/game/playerstats.cpp`, `stats_test`): every
profile has a `player_id` UUID and an optional email (asked once after a new
name, editable under OPTIONS > PLAYER EMAIL, marked NOT VERIFIED until the
account feature exists); every save folder has an `install_id`; play time is
counted per phase, lifetime and per machine; keyboard, mouse and gamepad
actions are counted the same way; each machine file records platform, OS,
GPU, cores, RAM and gamepad model. Nothing leaves the machine. Still to do
when the service exists: the verification flow (email a six-digit code the
player types into the game, since the game is not a browser), the run
record, and the opt-in upload.

### Encryption at rest (decided 2026-09-15)

Emails, machine names and operating-system details are stored **encrypted**
in the online database, not merely access-controlled. Working notes for when
the schema is written:

- **Email.** Let Supabase Auth hold the login email (it encrypts its own
  tables and never exposes them through the public API). In our own tables
  keep only a *blind index* of the address, `HMAC-SHA256(lowercase email,
  server key)`, so a player can be looked up by address without the address
  being readable, plus the ciphertext if we need to show it back to its owner.
- **Machine and OS facts.** Encrypt the `machines` row fields (install id,
  platform, OS string, GPU, cores, RAM, pad model) with `pgsodium` /
  Supabase Vault (`pgsodium.crypto_aead_det_encrypt` with a per-column key
  held in Vault, decrypted only inside SQL views that the service role can
  read). Aggregate statistics ("38 % of runs on a gamepad", GPU share) are
  computed once a day into a separate plain table so the dashboard never
  touches the encrypted rows.
- **Key handling.** Keys live in Supabase Vault and the Vercel project's
  encrypted environment variables only; never in the repo, never in the game
  binary. The game sends plain JSON over TLS to the submit route, which
  encrypts before insert.
- **Also do:** row-level security denies all direct reads of `players` and
  `machines`; public pages read only from views that expose display name and
  gameplay numbers; the account deletion path wipes the encrypted rows and
  the blind index together.

### In-game leaderboard screen and rankings (idea, 2026-09-15)

Once submissions work, the title screen gets a **LEADERBOARD** item (the
local cross-profile table stays as it is). One screen, tabs cycled with
Left/Right, data fetched on open and cached in the save folder with a
"fetched 3 minutes ago" line so it also works offline:

- **GLOBAL** top 100 by score, your own row pinned at the bottom with your
  absolute rank even when you are 4,000th.
- **THIS WEEK / THIS MONTH** the same table over a rolling window, so new
  players can be on a board that is not owned by day-one records.
- **BOARDS** other single numbers worth ranking: most fights survived in
  one run, highest level reached, most demons slain in one run, fastest to
  level 10, longest chain, blocks destroyed. Each is a Postgres view.
- **PLAYERS** everyone who has opted in, with trophy count, play time, and
  the machine split (controller versus keyboard percentage) that section 6
  collects.

Ranking scheme, so a player has one standing rather than a pile of tables:

- **Rating** = best run score (60 %) + median of the last ten runs (40 %),
  so one lucky run does not define a player and a consistent one is
  rewarded. Recomputed nightly.
- **Rank tiers**, named after the game's own ladder and awarded by rating
  percentile: ZOMBIE (bottom 40 %), IMP (to 60 %), DEMON (to 75 %),
  CACODEMON (to 85 %), BARON (to 93 %), CYBERDEMON (to 98 %), SPIDER
  MASTERMIND (top 2 %), and DOOM SLAYER for the current #1 alone. The tier
  shows next to the name everywhere: leaderboard, players list, title status
  strip, and as a small badge on the game-over screen when it changes
  ("PROMOTED: BARON"). Promotion and demotion are events worth a sound.
- **Verified badge** for runs whose block phase replayed clean (section 5);
  an unverified-only filter for the sceptics.
- **Seasons** later, if there is a player base: a quarterly reset of the
  windowed boards with the season's #1 getting a permanent trophy.

## 8. Order of work when you pick this up

1. Clean-ups in section 1, push, tag `v0.1.0`, and get the release workflow
   green (expect a round of Windows compile fixes).
2. `RunRecord` + JSON on disk + version header (no network yet). Tag `v0.2.0`.
3. Supabase project and schema; Vercel site reading it (seed it by hand from a
   few JSON files to build the pages before the game can submit).
4. `libcurl` submit with opt-in, pending queue, rank on the game-over screen.
5. Replay log for the block phase and the verified badge, if wanted.
