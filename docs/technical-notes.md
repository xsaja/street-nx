# street-nx technical notes

What FIFA Street needed on top of mvp-nx, and why. The game: USA release, title ID `45410072`, `default.xbe` SHA-1
`739a2fdb2bacee23b92726cc3b3b18b9f83cf664`, XDK 5849 (the same libraries as MVP Baseball 2005).

## XDK functions named by hand

`xbe_symbols` found 124 of 238 functions. The ones the game calls and the runtime replaces, but it missed, are named at
the top of `[symbols]` in `game/config/game.toml`; each ran its original code, which reaches for the Xbox's hardware:

| Address | Function | Without it |
|---|---|---|
| 0x25DC80 | `D3DDevice_SetBackBufferScale` (new HLE: keeps the scale in the device) | crash at 0.5 s in the library's frame buffer setup |
| 0x266D40 | `IDirectSoundBuffer_SetEG` (the twin xbe_symbols dropped for SetFilter, as in MVP) | crash at 0.9 s in the APU code |
| 0x251B80 / 0x251BA0 | `D3DDevice_BeginPush` / `EndPush` | crash at 1.3 s |
| 0x266DF4 | `IDirectSoundBuffer_Pause` (new HLE, as the stream's) | — |
| 0x2539B0 | `D3DDevice_SetPalette` | — |
| 0x270234 | `XInputGetCapabilities` | — |

`tools/unnamed_api.py` lists the library functions the game calls that still have no name; `tools/crash_report.py`
names a PC crash's function from its dump.

## Behaviour the port matches

- **Language**: the game takes it from the dashboard (`ExQueryNonVolatileSetting` 7). The `language` setting answers
  it; `"auto"` is the host's language.
- **Loading screen**: it draws only the new piece of its bar over the frame before. `gpu::g_swap_keeps_back_buffer`
  (set in `game_info.cpp`) copies each shown frame into the next back buffer, as `D3DSWAPEFFECT_COPY` would.
- **Thread priorities**: the game asks for them (its audio thread is time critical). `kernel::ApplyPriority` sets
  them on the Switch's threads; the runtime used to keep them without applying them.
- **EA's sound engine guard** (0x2FC184): a counter that was a lock only on the Xbox's single core. On the Switch's
  cores two threads entered together and the game called a voice not made yet (0x220C48). `game/src/game/hooks.cpp`
  makes the enter one compare-and-swap, and `[[hook]] SoundGuardLeave` puts a release fence before each leave.
  Other single-core assumptions may exist: a crash that only shows with a thread held back is the sign.

## Performance (Switch, stock clocks)

Matches hold 60 fps handheld and docked. What made the difference, with the log's `perf:` and `pacing:` lines to
check it:

- **The log** is written by its own thread (`core/log.cpp`): written at once, each line waited for the SD card under a
  lock every thread takes, which made 35 ms frames and 100-300 ms stalls.
- **`frame_pacing = "vsync"`**: with vsync, the screen paces the game instead of a second 60 Hz clock beside it.
- **Small files in memory** (`kernel/files.cpp`, `SmallFile`): disc files of 2 MB or less are read once whole. The
  game reads the same few KB of `data\datafile.big` again and again in a match, each read queued behind the audio
  streams.
- **Recording**: header groups are read again only when the game sets them, and a draw with the same resources as
  the one before skips marking them used (`render_thread.cpp`).

The profiler (`profile = true`, `tools/prof_report.py` with the NRO's ELF) shows the game's own code flat: no
function above 3 % of the title thread, so translating hot functions by hand would gain little. What is left are the game's
loads at kick-offs and goals, and a rare frame where the screen gives its image back late.

## Options left off

- **SMAA** (`smaa = true`): about 5 ms of GPU a frame at 960x720; docked it would need about 10 more.
- **Clocks**: not needed.
