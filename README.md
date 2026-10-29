<p align="center"><img src="docs/images/banner.jpg" alt="FIFA Street" width="100%"></p>

<h1 align="center">street-nx</h1>

<p align="center">
  <b>FIFA Street</b> (Xbox, 2005) on the <b>Nintendo Switch</b>,<br>
  statically recompiled from the original Xbox executable — no emulator.
</p>

The game's x86 code is translated to C++ and built as a native Switch program. A runtime stands in for the Xbox
kernel, its Direct3D 8 (drawn with **deko3d**, the Switch's own graphics API, or with Vulkan/NVK) and its audio.
street-nx is built on **[mvp-nx](https://github.com/xsaja/mvp-nx)**, the MVP Baseball 2005 port, which starts from
TGP482's **[XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp)**, maddog847's
**[MVP05 XBE Recomp](https://github.com/maddog847/MVP-Baseball-2005-XBERecomp)** and the Switch layer of
**[BLACK-NX-PORT](https://github.com/gerisson1996/BLACK-NX-PORT)**.

> [!NOTE]
> You need your own copy of *FIFA Street* for the original Xbox, **USA** release (title ID `45410072`): the disc
> image (`.iso`) or the extracted disc (the folder with `default.xbe`). No game files are in this repository, and none
> are distributed.

## What works

Tested on a Switch (Atmosphère, title mode), handheld and docked:

- Boot, the EA logos and the intro movie, the menus, Game On, Rule the Street, saving and loading
- Matches at **60 fps**: 1280x720 handheld, 1920x1080 docked, with sound and music
- English or Spanish, following the Switch's system language
- Leaving from sphaira without errors

## Installation

> [!TIP]
> **Installer page: [xsaja.github.io/street-nx](https://xsaja.github.io/street-nx/)**. Pick your disc image or game
> folder, get `street-nx.zip`, extract it into `sdmc:/switch/`. Everything runs in your browser: your game files are
> never uploaded.

By hand:

1. Copy the `street-nx` folder of the package to `SD:/switch/street-nx/` (`street.nro`, `street-settings.toml`,
   `LICENSES.txt`).
2. Copy the files of your disc to `SD:/switch/street-nx/game/`: `default.xbe` and the `data` folder (1.67 GB).
3. Start it in **title mode** (hold R while starting a game and pick it in hbmenu, or use sphaira or a forwarder). Applet
   mode (the album) does not have enough memory and says so.

The first start compiles the deko3d shaders (a few seconds of black screen); they are kept in `shadercache/`.

## Controls

| Switch | Xbox |
|---|---|
| B / A / Y / X | A / B / X / Y (by position: the button the game shows sits where you press) |
| L / R | left / right trigger |
| ZL / ZR | white / black |
| + / − | Start / Back |
| sticks, stick clicks | sticks, stick clicks |

`buttons_by_name = true` in the settings maps A/B/X/Y by letter instead.

## Settings

`street-settings.toml` beside the NRO, commented. The ones a player may want:

- `language`: `"auto"` (the Switch's) or `"en"`, `"es"`...
- `anisotropic_filtering`: 2 by default; 4 or 8 sharpen the ground further.
- `smaa`: SMAA anti-aliasing, off by default (about 5 ms of GPU a frame; docked it can drop frames).
- `frame_pacing`: `"vsync"` by default (the screen paces the game); `"timer"` as older builds.
- `renderer`: `"deko3d"` (default) or `"vulkan"`.
- `cpu_mhz`, `gpu_mhz`, `mem_mhz`: 0 = the console's clocks. FIFA Street does not need more.

Logs go to `SD:/switch/street-nx/logs/`.

## Building

Windows, with Visual Studio 2026 (its Clang), CMake and Python for the PC builds, and devkitPro (MSYS2, `switch-dev`,
deko3d, uam) for the Switch. Put the disc's files in `game/assets` (or a junction to them).

```
tools\build_pc.cmd                      # STREET-d3d12.exe, STREET-vk.exe; recompiles default.xbe into game/generated
switch/build.sh                         # in MSYS2: build-switch/street.nro (VERBOSE=ON for a full log)
```

The first PC build writes `game/config/game.toml` with `xbe_symbols`; the one here also names by hand the XDK
functions it missed (see `docs/technical-notes.md`). `nvk_sdk/` is the Mesa 26.2.4 Horizon build (NVK) the Vulkan
renderer links.

The installer page is `installer/`, adapted from mvp-nx's (itself from defjam-sw-installer and nfsmw-nx-installer).
`.github/workflows/pages.yml` publishes it with the `street.nro` of a release, checked against
`installer/release/manifest.json`. To try it locally, put `street.nro`, `street-settings.toml` and `LICENSES.txt` in
`installer/release/` and run `python installer/serve.py 8195 installer`.

## Credits

- **TGP482**: XboxXBE-Recomp, the recompiler and the runtime
- **maddog847**: MVP05 XBE Recomp, whose SDK fixes mvp-nx and street-nx carry
- **gerisson1996**: BLACK-NX-PORT, the Switch layer
- **xsaja**: mvp-nx
- **ChanseyIsTheBest**: the deko3d guide of sa2-nx, uam as a library, the Ryujinx fork used for testing
- **fincs**: deko3d and uam; **danfromtico**, **NaGaa95**: mesa-switch and its 26.2.4 build
- **R-drg**: defjam-sw, whose profiler the Switch profiler is adapted from
- **Jorge Jimenez et al.**: SMAA
- Banner (hero and logo) and NRO icon: community art from **[SteamGridDB](https://www.steamgriddb.com/)**

Every license is in `LICENSES.txt`.

## Legal

FIFA Street is a trademark of Electronic Arts. This project has no relation to Electronic Arts, FIFA or Nintendo. It
contains no game data; `street.nro` contains code translated from the player's own `default.xbe`. The code of street-nx
is licensed under the GNU General Public License version 3 (`LICENSE`), as mvp-nx's is.
