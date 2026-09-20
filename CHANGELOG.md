# Changelog

The game reads this file (through the service) for its WHAT'S NEW screen, so
keep the format: a `## <version> (<date>)` heading per release, `- ` bullets
under it, newest first. Plain sentences; no links or code in the bullets.

## 0.4.1 (2026-09-19)
- Deleting a player and making it again no longer leaves a second player behind on the leaderboard. Registering an address that already has players offers to carry on as one of them, keeping that history.
- Deleting a registered player now asks whether to remove its online record too, and says what that would erase. Keeping it is the default.
- The explosion smoke is smoke again: it was a white ball hanging in the air after the fireball went out.
- Shattered blocks throw fragments in their own colour instead of white ones.
- Menu text no longer runs off the edge or prints on top of itself: the delete confirm, the title menu and the dungeon boss line all have room now.
- F12 saves numbered screenshots instead of overwriting the same file.

## 0.4.0 (2026-09-19)
- The dungeon: clear the arena and the back wall comes down on a crypt of rooms and corridors, generated fresh for every fight and bigger at higher levels.
- Dungeon demons sleep until they see you; the boss at the far end scales with the level and with how big your stack was. Kill it to finish the fight.
- A new trophy, DUNGEON CRAWLER, for the first boss slain, and a DUNGEON switch in the options.
- The new-version notice is a tilted sticker beside the title menu that zooms to get your attention; click it for WHAT'S NEW.
- The test bot now plays the block phase properly.

## 0.3.0 (2026-09-19)
- The game tells you when a new version is out and has a WHAT'S NEW screen with this list.
- Online is on by default; nothing is posted until you approve your email address.
- Approve your registration by clicking the link in the email; typing the code still works too.
- Players who have not set up the leaderboard are asked once whether they want to.
- Ray tracing is on by default only on GPUs that support it.

## 0.2.0 (2026-09-18)
- Online leaderboard: register your email in OPTIONS and every finished game is posted with your world rank.
- LEADERBOARD screen on the title with five boards and your own row pinned.
- PLAYERS screen on every launch: several people on one machine, rename, edit email, delete.
- Trophies slide in as cards without interrupting play; fireworks for RIP AND TEAR.
- Arch-vile joins the Doom 2 variants with its flame attack: break line of sight before the hands clasp.
- Monsters have a real heading and chase the Doom way, so voxel monsters can be flanked.
- AI-upscaled menu font for large text.
- Blocks left floating after a clear now fall; separate music and effects volume sliders.
- The options screen scrolls; MAIN MENU on the game-over and pause menus.
- Installers for Windows, Arch, Debian and an AppImage.

## 0.1.0 (2026-09-18)
- First public release: the block game with red minos, the tip-over and the first-person fight, seven Doom monster classes plus five Doom 2 variants, four weapons.
- Ray-traced shadows, reflections and ambient occlusion on GPUs with ray queries, HDR bloom, volumetric light, 4x MSAA, voxel monsters, material maps.
- Brutal mode with the community gore pack, per-weapon deaths, blood, gibs, casings and decals.
- Doom's music through a built-in OPL3 synthesizer, or the rerelease's recorded soundtracks.
- Profiles, 20 trophies, high scores, gamepad support with rumble, mouse-driven menus, display and resolution options.
