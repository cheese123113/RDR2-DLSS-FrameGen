# RDR2 DLSS Frame Generation (DLSSFG)

Native **NVIDIA DLSS Frame Generation (2x)** for Red Dead Redemption 2's Vulkan renderer, as a single `.asi` plugin.

**An alternative to OptiScaler** for people who only want frame generation in RDR2. OptiScaler is a general toolkit that
replaces or extends a game's upscaler. DLSSFG leaves the game's own DLSS Super Resolution running untouched and adds
NVIDIA's DLSS Frame Generation on top, reusing the depth and motion vectors RDR2 already gives its built-in DLSS.

> **G-Sync (or another VRR mode) is strongly recommended.** See [Display setup](#display-setup-g-sync-recommended).

## Features

- Real DLSS Frame Generation (`nvngx_dlssg.dll`) through NVIDIA NGX on the game's own Vulkan device. No upscaler
  replacement and no DLSS spoofing.
- Uses the depth buffer and motion vectors from the game's own DLSS Super Resolution pass.
- Camera data comes from the game itself (via ScriptHookRDR2) and is matched to each frame using the game's Reflex frame
  IDs. On startup it is checked against the game's motion vectors, and the best timing and field of view are picked
  automatically.
- Frame pacing on a separate thread: generated and real frames are spaced evenly, and the game is capped just under half
  the refresh rate so the display never gets more frames than it can show.
- Menus and loading screens (where the game's DLSS isn't running) pass through untouched.
- **Insert** toggles frame generation on/off in game for comparison.
- A detailed log (`DLSSFG.log`) for troubleshooting.

## Requirements

- An NVIDIA GeForce **RTX 40 or 50 series** GPU and a recent driver. Developed and tested on an RTX 5070; RTX 40 cards
  should be supported by NVIDIA's frame generation but are **untested** with this mod.
- RDR2 (PC) in **story mode** (ScriptHook doesn't run online).
- In the game's graphics settings: **API: Vulkan**, **DLSS on** (any quality mode). **NVIDIA Reflex: On** is recommended.
- [Script Hook RDR2](http://www.dev-c.com/rdr2/scripthookrdr2/) with its ASI loader (`dinput8.dll`). It loads the plugin
  and provides the camera data.
- `nvngx_dlssg.dll` from the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) (not included here, see Install).

## Install

1. Copy `DLSSFG.asi` and `DLSSFG.ini` (from the [Releases](../../releases) page) into the RDR2 game folder, next to
   `RDR2.exe`.
2. Get `nvngx_dlssg.dll` (Windows x64) from the NVIDIA DLSS SDK: `lib/Windows_x86_64/rel/nvngx_dlssg.dll`. Put it in the
   game folder too.
3. Start the game and load story mode. Frame generation turns on once you're in the world. Press **Insert** to compare
   with it off.

To uninstall, delete `DLSSFG.asi` (or set `Enabled=0` in `DLSSFG.ini`).

## Display setup (G-Sync recommended)

The pacing thread sends each frame to the screen at a calculated moment: the generated frame, then the real frame half a
game frame later.

- **With G-Sync / VRR**, the monitor refreshes as each frame arrives, so the spacing reaches the screen as intended, even
  while the game's frame rate moves around. This is the recommended setup.
  - In the NVIDIA Control Panel, under "Set up G-SYNC", enable G-SYNC / G-SYNC Compatible for **windowed and full screen
    mode** (RDR2's borderless mode needs the windowed option). Enable Adaptive-Sync / FreeSync in the monitor's menu.
  - Keep **V-sync on** in the game. With G-Sync it only stops frames going over the refresh rate. No extra frame cap is
    needed: the mod's pacing already caps the game.
- **Without G-Sync**, frames snap to the monitor's fixed refresh rhythm, and the timing becomes uneven (judder) unless the
  game can hold exactly half the refresh rate. If you can't use VRR, run at **120 Hz** (in exclusive fullscreen pick
  120 Hz in the game's settings; in borderless set Windows to 120 Hz): the game then locks to an even 60 fps and frame
  generation fills in every other frame.

Also recommended: **motion blur off**, since it smears into generated frames.

## Settings (`DLSSFG.ini`)

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | `1` | `0` turns the mod off completely (it only logs). |
| `Pacing` | `1` | `1` spaces generated/real frames evenly on a pacer thread. `0` presents them back to back. |

## What it changes

DLSSFG only touches the game's graphics setup, never save data:

- **Vulkan device:** enables `VK_KHR_buffer_device_address` (needed by DLSS Frame Generation) and `VK_EXT_memory_budget`
  (for the log).
- **Swapchain:** adds transfer usage and three extra images.
- **Depth and motion-vector images:** adds transfer-source usage so they can be copied each frame.

## Known limitations

- No HUD-less input yet, so the minimap and prompts can smear slightly during fast motion.
- The camera's near/far clip planes aren't available from the game. Fixed values are used, which only affects frame
  generation's handling of camera movement (not rotation).
- 2x only (one generated frame per real frame).
- Vulkan only. The DX12 renderer isn't supported.

## Building

Requires Visual Studio 2022 or newer (C++ x64 tools) and the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS).

```bat
set DLSS_SDK=C:\path\to\DLSS
build.bat
```

The output is `bin\DLSSFG.asi`. The Vulkan headers are bundled in `third_party/`.

## Troubleshooting

Everything goes to `DLSSFG.log` next to the `.asi`. Useful lines:

- `fg: NGX Frame Generation available`: frame generation started.
- `fg: last 10 s: ... game frames/s with a generated frame`: running stats every 10 seconds.
- `fg: camera check ...`: the automatic camera calibration.
- `fg: NGX ...` errors / `ngx-log` lines: NVIDIA's own messages if something fails.

## License

MIT (see [LICENSE](LICENSE)). Third-party notices are in [NOTICE.md](NOTICE.md). Not affiliated with NVIDIA or Rockstar
Games. DLSS is a trademark of NVIDIA.
