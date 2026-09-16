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

The Radeon 8060S and the RTX 5070 Ti both expose `VK_KHR_ray_query`
(`VkContext::rayQuerySupported()`), so ray-traced shadows and reflections for
the cube scene are a natural next step for the Linux build: one BLAS for the
unit cube, a TLAS rebuilt per frame from the instance list, and a ray query in
`cube.frag` for sun/emissive-block shadows. That keeps the look converging with
the path-traced Windows target.

## RTX-class features worth adding (assessed 2026-09-16)

The game already detects `VK_KHR_ray_query` at start-up, so every item below
can be an OPTIONS toggle that only appears on a ray-tracing GPU (RTX 20+,
Radeon RX 6000+/RDNA3 iGPUs such as the 8060S, Intel Arc) with the current
renderer as the fallback. In order of payoff per day of work:

1. **Ray-traced shadows for every light** (2-3 days). Today only the sun casts
   shadows, through a 2048^2 shadow map; the torches, lamps, muzzle flashes,
   fireballs and glowing red blocks light things but cast nothing. One BLAS
   for the unit cube plus one per voxel-mesh frame, a TLAS rebuilt each frame
   from the instance lists (a few thousand instances, cheap), and a ray query
   per light in `common.glsl`'s `shade()`: hard shadows from torches on the
   stack, monsters shadowing each other, a muzzle flash throwing the room's
   shadows for a frame. Biggest visible change; the arena is small so the
   ray budget is modest.
2. **Ray-traced floor reflections** (+1 day on top of 1). The hex floor is
   the largest surface on screen; a single glossy bounce from it reflects the
   red blocks, torches and voxel monsters. Roughness comes from the existing
   material params.
3. **Ray-traced ambient occlusion** (+1-2 days). Contact shadows where blocks
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
