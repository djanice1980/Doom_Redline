# Changelog

The game reads this file (through the service) for its WHAT'S NEW screen, so
keep the format: a `## <version> (<date>)` heading per release, `- ` bullets
under it, newest first. Plain sentences; no links or code in the bullets.

## 0.7.1 (2026-09-21)
- The Linux downloads run on any x86-64 machine again. They were built on a CachyOS box, which stamps its own processor level into everything it links, so on an ordinary CPU they refused to start with "CPU ISA level is lower than required". Releases are now built in a stock container.
- A controller works when the game is launched from Steam. Steam takes the pad you plugged in away and hands over one of its own, and the game was holding on to the one it saw first.
- The door into the boss hall looks like a door: a Doom door with a pile of skulls on it, lit so it reads from down the corridor. It stays solid until it opens, so nothing in the hall can see you or shoot you through it.
- Bosses have far less health. What is dangerous about the hall now is the court around them, several of the heaviest demons the level can field.
- The boss hall is sized for its boss. The spider mastermind gets a room twice the floor it used to have.
- Zombies and chaingunners stop shooting you from across the crypt: they hold fire past sixteen and twenty-four metres and close in instead.
- A health bar only floats over a demon you can actually see.
- UNTOUCHABLE is for clearing the arena without taking damage. The crypt and its boss no longer count against it.
- The test bot looks where it is walking instead of at the ceiling, and goes to the door when the hall is sealed.

## 0.7.0 (2026-09-20)
- REDLINE opens on a title card in Doom's own shape, held for a moment and then melted away a column at a time to leave the game behind it. Any key cuts the wait short.
- The card is built when the game loads out of the art your copy of Doom has, so the logo, the letters and the monsters on it are yours.
- The same page, shaded blue, sits behind the new CONTROLS screen and behind a page on the way out that shows what you scored and where the next version lives.
- CONTROLS lists everything the keyboard, the mouse and a gamepad do, side by side, and the key hints are gone from the corner of the play field.
- The high scores on the title screen moved up to the top of the page with them.
- The leaderboard is back on the title menu: a version check was overwriting it with a second WHAT'S NEW.
- WHAT'S NEW, the leaderboard question, the adopt screen and the delete confirm can be closed again. Nothing answered a key on any of them.

## 0.6.0 (2026-09-20)
- Setting up the leaderboard is one click now. The email has a single Approve button and there is no code to type.
- Your scores are posted from the moment you set an address, but nobody can see them until you click that button: the boards, player pages and the site totals show approved players only. Declining deletes everything collected.
- Permission belongs to your email address rather than to one profile, so another profile on a machine you have already approved needs no second email. Deleting a profile takes its permission with it.

## 0.5.0 (2026-09-20)
- The crypt is built from the game that made it: every room is shaped like one of the seven pieces, drawn from the ones you actually dropped, and the whole layout is seeded from how you played.
- The boss hall is sealed. Find the skull key, hidden in the room furthest from it, and the door opens as you reach it.
- What the crypt leaves on the floor is worked out by simulating the fight ahead with the weapons you are carrying, and a roster you could not possibly survive is cut back until you could.
- Six texture sets dress the rooms, from grey crypt to marble tomb to something fleshier, with the torch colour following the room.
- Columns, candles, stalagmites, hanging corpses and skull piles stand against the walls, and the bigger rooms have pillars to fight around.

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
