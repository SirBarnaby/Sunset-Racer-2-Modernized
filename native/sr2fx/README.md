# sr2fx: graphics layer

A 32-bit `d3d9.dll` that sits beside the game exe. It wraps the system Direct3D 9
and adds optional effects to the 3D scene. With every option off it passes each
call straight through, and the game renders exactly as it does without it. The
menu frame is pixel-identical with and without the dll.

## Files shipped beside the exe

| file | what |
|---|---|
| `d3d9.dll` | the layer (`out\d3d9.dll`) |
| `sr2fx-vk.dll` | DXVK 3.1.1's 32-bit d3d9.dll, renamed. Used only when `renderer=1` |

`build.ps1` (game packaging) should copy both from `native\sr2fx\out\` into the game folder.

## Build

`native\sr2fx\build.bat` (MSVC 2022 Build Tools plus the Windows 10/11 SDK's
`fxc`). It compiles the shaders in `shaders\post.hlsl` into headers, builds
`out\d3d9.dll`, and copies DXVK from `third_party\dxvk\dxvk-3.1.1\x32\`. From
PowerShell: `cmd /c native\sr2fx\build.bat`.

## Settings: `SunsetRacer2-fx.ini`

The game writes this file and the layer reads it. A missing file, a missing key
or a value that won't parse means 0 (the original game). The layer checks the
file's write time every 250 ms on its own thread. Effect changes apply on the
next frame. The renderer applies when the game next creates its device (race
start or exit, or a display change).

| key | values |
|---|---|
| `renderer` | 0 DirectX 9, 1 Vulkan (DXVK, with integrated GPUs hidden so a discrete GPU is used) |
| `aa` | 0 off, 1 the game's own 2x MSAA, 2 FXAA. With `aa=1` the back buffer always keeps MSAA, so menus, HUD and text are multisampled. When a depth effect is on, the 3D scene itself is drawn single-sampled (D3D9 cannot read multisampled depth) and gets FXAA instead |
| `ao` | 0 off, 1 GTAO at half res, 2 GTAO at full res with 3 directions instead of 2 (same look, cleaner edges, never darker). Fades with distance and the game's fog |
| `motionblur` | 0 off, 1 camera, 2 camera and moving objects (traffic, wheels) |
| `blurstrength` | 0..100 (shutter; 100 = one full frame) |
| `sunshafts` | 0/1, screen-space shafts from light 0 (the sun) |

Other keys in the file belong to the audio layer and are ignored here.

## How it works

- Shockwave 3D draws with fixed-function D3D9. When an effect is on, the layer
  points the main 3D scene at its own colour target and an INTZ depth texture.
  The game's back buffer (with its MSAA) is untouched and gets the result.
- The main pass is the camera that first makes 80 perspective, whole-window
  draws in a scene. It ends at the first draw that camera would not make:
  another lens (menu planes, HUD models), a partial viewport, an orthographic
  projection, a depth clear, a render target change or the end of the scene.
  The effects run there, so nothing drawn after it (HUD, menus, text) is ever
  post-processed. Frames with no main pass (menus, garage, loading) are not
  redirected and cost the layer only a settings copy.
- At the end of the main pass it runs
  linear depth (checkerboard nearest/farthest at half res), then GTAO with a
  4x4 interleaved pattern, a thin-occluder heuristic and a screen-space radius
  cap, and a slope-aware bilateral blur that averages one full period of the
  pattern (no grain); the composite upsamples it against full-res depth, falling
  back to the nearest-depth texel at edges (no halos), then sun mask and two radial
  blur passes at quarter res, then the composite, then motion blur (McGuire:
  velocity, tile max, neighbour max, reconstruct), then FXAA into the game's
  back buffer. AO is multiplied in before the first see-through draw when the
  scene has one, so glass, smoke and rain are not shaded.
- Camera velocity comes from reprojecting with last frame's view and
  projection. Moving objects (`motionblur=2`) are re-drawn into the velocity
  buffer with their current and previous world matrices. Objects are matched
  by vertex pointer, index pointer, counts, draw occurrence and a fingerprint
  of the mesh (first and last vertex, first indices), so an address Shockwave
  reuses for another model starts over instead of inheriting its motion. A jump
  or a change of scale also starts over. Blended draws (rain, glass, smoke) are
  skipped. The layer never changes the game's transforms or states: its passes
  run inside a captured state block and restore it.
- Reset, resize, device recreation and directToStage off (dev screenshots via
  GetRenderTargetData) are tested. A lost device is handled in code (no
  redirect until the game resets it) but was not induced in testing. If targets or shaders
  can't be created, that device runs as plain pass-through.

## Dev mode

Only when `SunsetRacer2-dev.txt` exists beside the exe:
- `SunsetRacer2-d3d9.log` with device parameters, per-second fps and frame
  times, and GPU time per effect (timestamp queries).
- DXVK's own log.
- `fxdebug=1|2|3` shows the AO, velocity or shaft buffer.
- `SunsetRacer2-d3d9-shot.txt` (holding an output `.bmp` path) saves the next
  presented frame.
- `SunsetRacer2-d3d9-ftrace.txt` (`start interval` in seconds since the layer
  loaded) logs one frame's full sequence at start and then every interval:
  cameras, viewports, clears, draws grouped by state, where the main pass ended
  and why, and the size and distance of moving objects.
- Settings are logged only when a real setting changes, not for the live keys
  the race rewrites several times a second.

## Measured (1080p, laptop: Intel UHD 620 runs DirectX 9, Quadro P500 runs Vulkan)

GPU ms per effect from the dev log, parked at the start line:

| effect | UHD 620 (DX9) | P500 (Vulkan) |
|---|---|---|
| AO half res (`ao=1`) incl. composite | 5.7 | 3.4 |
| AO full res (`ao=2`) incl. composite | 20.7 | 13.0 |
| sun shafts incl. composite | 2.3 | 0.7 |
| motion blur camera (`=1`) | 3.9 | 1.6 |
| motion blur full (`=2`), traffic in view | 10.5 to 12 | 3.2 to 6.8 |
| FXAA | 2.0 | 1.1 |

Whole-game fps, effects off, parked, same scene: DX9 on the UHD 620 59 fps
unlimited (17 ms) and 50 fps with the 60 cap. Vulkan on the P500 85 fps
unlimited (11.8 ms) and 57 fps with the 60 cap. Other programs were sharing
the machine, so treat fps as approximate.
