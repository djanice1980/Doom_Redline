# RTX Remix and this project

## What Remix actually is

RTX Remix has two parts:

- **RTX Remix Runtime** (open source, `NVIDIAGameWorks/dxvk-remix`): a
  replacement `d3d9.dll` built on DXVK. It captures a Direct3D 9 game's draw
  calls, rebuilds the scene, and path-traces it (with DLSS, ReSTIR, etc.) on an
  RTX GPU. Windows only. On Linux it can be run *through Proton* as the D3D9
  layer of a Windows game.
- **RTX Remix Toolkit**: the Omniverse-based app for replacing captured assets
  with PBR versions.

For a *new* engine there is a third piece: the **Remix API**
(`public/include/remix/remix_c.h` in the runtime repo). It lets an application
skip D3D9 and hand meshes, materials, lights and a camera straight to the Remix
renderer each frame. It is still the Windows `d3d9.dll` you load, and it still
needs an RTX card, but it is the sanctioned way to use "features from the Remix
SDK" in code you write yourself.

There is no native Linux Vulkan library called the Remix SDK. That is why this
project has a native Vulkan renderer for the machine it is developed on, with
an interface shaped so a Remix backend is a drop-in.

## Mapping the renderer onto the Remix API

The exact struct/function names are in `remix_c.h`; the shapes below are what
the interface needs and how they line up.

| REDLINE (`src/render/renderer.h`) | Remix API |
|---|---|
| unit cube mesh (24 verts, 36 idx) | one `remixapi_MeshInfo` created at startup (`CreateMesh`) |
| `CubeInstance` (pos, scale, tint, emissive, atlas rect) | `remixapi_InstanceInfo` per cube (`DrawInstance`) with a transform; tint/emissive select a material |
| atlas + tint | one `remixapi_MaterialInfo` (+ `MaterialInfoOpaqueEXT`) per distinct (texture, tint, emissive) - materials are cached, not per-instance |
| roughness / metallic in `params` | `MaterialInfoOpaqueEXT.roughnessConstant / metallicConstant` |
| emissive rgb + strength | `MaterialInfoOpaqueEXT.emissiveColorConstant / emissiveIntensity` (this is what makes the red blocks light the arena in a path tracer) |
| world billboard `QuadInstance` | a 2-triangle mesh instance oriented per frame; material with `alphaTestThreshold` for the sprite cutout (Doom sprites) |
| `PointLight` | `remixapi_LightInfo` + `LightInfoSphereEXT` (`DrawLightInstance`) |
| sun (`sunDir`, intensity) | `LightInfoDistantEXT` |
| `FrameParams.view/proj` | `remixapi_CameraInfo` (`SetupCamera`) |
| screen-space HUD quads | not covered by the API; drawn with an ordinary D3D9 overlay after `Present`, or kept as a Vulkan/SDL overlay on top |
| `screenshot()` | read back from the D3D9 backbuffer |

Materials in Remix carry texture *file paths* (DDS/PNG on disk) rather than an
in-memory atlas. The Remix backend therefore writes the atlas regions it needs
to a `remix_textures/` folder once (the atlas packer already knows every region)
and references them by path.

## Build plan for the Windows target

1. `cmake -DREDLINE_REMIX=ON` adds `src/render/renderer_remix.cpp` implementing
   the same `Renderer` surface (`setAtlas`, `render`, `screenshot`) and links
   nothing extra: the Remix API is loaded at runtime via
   `remixapi_lib_loadRemixDllAndInitialize(L"d3d9.dll")`.
2. Copy the Remix runtime release (`d3d9.dll`, `NvRemixBridge*`, `.trex/`)
   next to `redline.exe`.
3. Window creation stays SDL3 (Win32 HWND handed to Remix `Startup`).
4. Everything else - rules, WAD reader, assets, FPS simulation, HUD layout - is
   unchanged and already platform-neutral (no Linux-only calls; SDL3 handles
   input/audio/windowing on Windows).

## What the native Vulkan backend can do meanwhile

*Done 2026-09-16.* The Radeon 8060S and the RTX 5070 Ti both expose
`VK_KHR_ray_query` (`VkContext::rayQuerySupported()`), and the Linux build now
uses it: one BLAS for the unit cube plus one per voxel mesh, a TLAS rebuilt per
frame from the instance lists, and a ray query in `common.glsl`'s `shade()`,
reached through `cube_rt.frag`, `mesh_rt.frag` and `quad_rt.vert`. OPTIONS >
RAY TRACING picks between the shadow map, sun shadows, all lights, and all
lights plus floor reflections. The look is converging with the path-traced
Windows target.

## RTX-class features worth adding (assessed 2026-09-16)

The game already detects `VK_KHR_ray_query` at start-up, so every item below
can be an OPTIONS toggle that only appears on a ray-tracing GPU (RTX 20+,
Radeon RX 6000+/RDNA3 iGPUs such as the 8060S, Intel Arc) with the current
renderer as the fallback. In order of payoff per day of work:

1. **Ray-traced shadows for every light** (2-3 days). *Done 2026-09-16: RAY
   TRACING mode 2 shadows every point light that contributes more than 0.015 of
   attenuation.* Before it, only the sun cast shadows, through a 2048^2 shadow
   map; the torches, lamps, muzzle flashes, fireballs and glowing red blocks lit
   things but cast nothing. One BLAS
   for the unit cube plus one per voxel-mesh frame, a TLAS rebuilt each frame
   from the instance lists (a few thousand instances, cheap), and a ray query
   per light in `common.glsl`'s `shade()`: hard shadows from torches on the
   stack, monsters shadowing each other, a muzzle flash throwing the room's
   shadows for a frame. Biggest visible change; the arena is small so the
   ray budget is modest.
2. **Ray-traced floor reflections** (+1 day on top of 1). The hex floor is
   the largest surface on screen; a single glossy bounce from it reflects the
   red blocks, torches and voxel monsters. Roughness comes from the existing
   material params. *Done 2026-09-16:* mode 3 of the RAY TRACING option.
   Floor cubes carry a reflective flag; `cube_rt.frag` fires one ray per
   floor pixel, resolves the hit through the TLAS custom index (cube
   instance buffer at binding 6, mesh vertex/index buffers by device address
   through a table at binding 7), and shades it with ambient, a sun shadow
   ray and unshadowed point lights. The reflection weight is
   `(1 - roughness)^2` times a Fresnel term. Torches are billboards and so
   are not in the reflection; their light on the reflected surfaces is.
   Cost on the Radeon 8060S at 1600x900: about 4 ms a frame. The material
   maps from the section below are wired in at the same time: cube vertices
   carry a tangent, cubes are drawn in ranges with a material slot push
   constant, and `cube_frag_body.glsl` perturbs the normal and modulates the
   roughness (the pack's roughness maps sit at 0.92-1.0 everywhere, so they
   modulate the instance roughness by their variation rather than replace it).
3. **Ray-traced ambient occlusion** (+1-2 days). *Done 2026-09-16* as three hemisphere rays per fragment in mode 3 (no temporal filter; a position hash rotates the pattern). Contact shadows where blocks
   meet the floor and between a monster's voxels. Cheap with a few rays and
   a small temporal blend; a full one-bounce GI needs a denoiser (NVIDIA's
   NRD is free to use and has a Vulkan path) and is the step that makes the
   scene look path-traced.
4. **Upscaling and frame generation** (2-3 days each). DLSS through NVIDIA's
   Streamline/NGX SDK (RTX only, licence agreement, redistributable DLL) or
   FSR 3 (open source, all GPUs; the ray-traced GZDoom port ships the same
   FidelityFX DLLs). Only matters once 1-3 make 4K expensive. DLSS Ray
   Reconstruction would double as the denoiser for item 3 on RTX cards.
5. **NVIDIA Reflex** (`VK_NV_low_latency2`, half a day): lower input lag in
   the fight. Small but free on RTX 40/50.

Not applicable: RTX Remix itself only attaches to D3D9 games; a native Vulkan
renderer has to do its own ray tracing, which is what 1-3 are. Mesh shaders
and opacity micromaps have no use here. Everything in 1-3 also runs on the
Radeon 8060S, so it can be developed and tested on the laptop and profiled
on the 5070 Ti when the eGPU is attached.

## Material data in the GZDoom: Ray Traced release (looked at 2026-09-16)

`gzdoom-rt-1.0.2.zip` (github.com/vs-shirokii/gzdoom-rt, GPLv3 code, built on
the MIT-licensed RTGL1 path tracer) carries two things useful for items 2
and 3 above:

- `rt/mat/`: 1,445 small KTX2 textures giving 914 Doom wall and flat
  textures a normal map (`<NAME>_remix_normal.ktx2`, BC5 or RGBA8, with
  mips) and a roughness map (`<NAME>_remix_roughness.ktx2`, RGBA8); a few
  hand-made `_n` / `_e` / `_orm` / `_h` sets for lights and doors. All three
  textures REDLINE uses (STARTAN3, FLOOR4_8, CEIL3_5) have both maps.
  KTX2 with no supercompression is trivial to read (48-byte header, level
  index at offset 80) and BC7/RGBA8 upload straight into Vulkan.
- `rt/data/textures.json` (JSON with `//` comments): 342 per-texture
  overrides: `emissiveMult` for lights, lava, nukage and fire textures,
  `isMirror` / `isMirrorIfSmooth` for water, blood and slime,
  `roughnessDefault` / `metallicDefault`, and light colour/intensity for
  emissive textures. None of our three textures has an entry, so they take
  the port's defaults.

Licensing: the `rt/` assets carry no licence text. The `_remix_` maps look
generated from id's textures with the RTX Remix Toolkit's AI PBR tool, which
makes them derivatives of id Software art. So: **do not bundle them**. Treat
them like extras.wad: an optional pack found on the player's disk and loaded
when present. *(That is not what happened; see the decision below. The maps
ship with the game and `REDLINE_NO_MATERIALS=1` turns them off.)* The fallback
that keeps the feature
available to everyone is to generate our own maps from the albedo at load
time (height from luminance, normal from its gradient, roughness from
inverse local contrast), which is a few dozen lines and has no licensing
question; it also covers the block tiles, which the port has nothing for.

Decision (David, 2026-09-16, done the same day: `assets/materials/`, six
files, CREDITS.txt, LICENSE note, installed as `materials/` next to the exe
and `share/redline/materials` on Linux): the maps are only meaningful alongside the
original textures, and the game only draws Doom art when the player supplies
their own WAD, so bundle them gated on that: they are loaded only when a WAD
is in use, never on the placeholder set. Keep it minimal, six files for the
three textures REDLINE actually uses (STARTAN3, FLOOR4_8, CEIL3_5; about
100 KB), under `assets/materials/` with a CREDITS file naming
github.com/vs-shirokii/gzdoom-rt and a note in LICENSE that those files are
derived from id Software textures and are not covered by the MIT licence.
Extend the set only when the arena gains textures. The albedo-generated
fallback still covers the block tiles and the placeholder art.

How that feeds items 2 and 3: reflections need a per-pixel roughness for the
floor (the roughness map or the generated one) and a metallic/mirror flag
per surface (from textures.json conventions: floor tiles semi-glossy, walls
rough, water-style mirrors not used here); ray-traced AO and GI need the
normal maps to look right on close-up walls. The renderer's cube instances
already carry roughness/metallic params; the missing piece is a second atlas
(or two extra channels) for the normal and roughness maps, sampled in
`cube_frag_body.glsl`.
