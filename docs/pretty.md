# Making it pretty: the visual to-do list

Assessed 2026-09-16 after ray-traced shadows and floor reflections landed.
Effort is a rough estimate. Tick items off here as they land, with the commit.

## Foundation

- [x] **HDR + bloom + tone mapping** (done 2026-09-16). Render to a 16-bit float target,
      two-pass bloom, ACES-style tone map with exposure. Torches, muzzle flashes,
      glowing red blocks and the BFG glow instead of clipping. Half the items
      below look better through this.
- [x] **Anti-aliasing** (done 2026-09-16: 4x MSAA on the world pass, OPTIONS toggle). 4x MSAA on the main pass (dynamic
      rendering makes this cheap); FXAA as the low-end fallback.

## Atmosphere

- [ ] **A sky** (half a day). Doom's SKY1 from the WAD as a dome, or a starfield
      with the red-line motif, above the open arena.
- [ ] **Emissive wall strips** (half a day). Lit panels from the WAD (LITE3,
      TLITE flats) as a trim row along the walls, emissive plus point lights.
- [ ] **Volumetric light** (1-2 days). Sun shafts through the open roof and haze
      around torches, ray-marched against the shadow map.
- [ ] **Ray-traced ambient occlusion** (1-2 days). Contact shadows where blocks
      meet the floor and between a monster's voxels; needs a temporal blend.
- [ ] **Baked voxel AO** (half a day). Per-vertex darkening in crevices when a
      voxel mesh is built.

## Motion and feedback

- [~] **Particles** (blood, chunks, casings, blood clouds and smoke puffs done 2026-09-16 as Brutal mode with the community gore pack; sparks, embers, dust still open). Sparks on a line clear, embers from torches, blood
      on hits, dust when the BFG drops the stack, shell casings. Reuses the
      billboard path.
- [ ] **Line-clear and landing effects** (half a day). Flash and ring shockwave
      on a clear, dust puff on landing, a soft light following the falling piece.
- [x] **Decals** (done 2026-09-16: blood pools, wall splats, bullet holes; Brutal mode). Bullet holes on walls and blood on the floor that stay.
- [~] **Weapon feel** (screen blood on damage done; bob/sway/recoil still open). Bob and sway while walking, recoil on the
      shotgun, Doom-style red screen tint on damage.
- [ ] **Glossy blocks and frame** (an hour). Extend the reflective flag to the
      board frame and the blocks.

## Texture quality

- [ ] **Pixel-art filtering** (small). Per-texel anti-aliased sampling so the
      64-128 px Doom textures keep their look without shimmering.
- [ ] **Colour grading** (trivial after the post-process pass). A lookup-table
      pass toward Doom's warm palette.

## Bugs found on the way

- [x] Shotgun flash floated beside the gun: the flash sprite is cut for frame A but
      the pump frames B-D (other origins) were the firing frames. Fires on A now.

- [x] Plasma rifle showed two guns and a stuck frame while the trigger was held:
      PLSGB0 (the vent frame, different origin) was used as the firing frame under
      the PLSF flash, which already draws the barrel. Fires on A now, B after release.

## Agreed order

HDR bloom + tone mapping + MSAA, then sky + emissive strips, then particles +
line-clear effects, then ray-traced ambient occlusion. The rest as time allows.
