# FlatPace

*The only flatline you'll ever want to see.*

**FlatPace measures your games and applies the frame-rate limit that makes them run as steadily as possible.** It does for Windows what the Steam Deck does in its own system: a steady pace feels smoother than higher but uneven FPS.

Current version: **0.41.1** · Windows 10 (1803 or later) and Windows 11. [User guide](GUIDE.md) · [Technical documentation](TECHNICAL.md) · [License](LICENSE) · [Español](README.md). The changelog ([CHANGELOG.md](CHANGELOG.md)) is in Spanish; the app has an English interface (ES | EN selector).

## What it does

- **Measures each game** frame by frame while you play (with Intel's PresentMon), telling real frames apart from DLSS Frame Generation ones.
- **Decides the limit using the whole session** when you close the game, based on your priority (smoothness or stability), your display, and whether G-SYNC/FreeSync is active.
- **Applies it in the NVIDIA driver** (the game's profile, without touching the game), **checks that it works** next session and keeps it.
- **Keeps watch:** if the game stops holding it for a sustained period, it lowers it; isolated dips don't count.
- **Fully reversible:** each game can be undone on its own and goes back exactly to how it was.
- **Diagnostics:** stutters, CPU/GPU bottleneck, frame generation, presentation mode, VRR state, and an in-game overlay with the pacing graph.

## Quick start

1. Unzip the folder anywhere (no installation needed).
2. Open `FlatPace.exe` and accept the administrator prompt (PresentMon and the NVIDIA driver need it).
3. Play normally. When you close the game, FlatPace saves the limit for next time.
4. Next session, the app checks it works ("Limit working") and it becomes **accepted**.

If "Windows protected your PC" appears, click *More info* → *Run anyway*: the program isn't digitally signed.

## Compatibility

| | Measure and recommend | Apply limits |
|---|---|---|
| NVIDIA | Yes | Yes |
| AMD / Intel | Yes | Not yet |

- **Displays without VRR:** limits that evenly divide the real refresh rate (e.g. 240, 120, 80, 60 on a 240 Hz display).
- **G-SYNC / FreeSync / Adaptive-Sync:** limits at the exact pace the game holds, below the display's maximum.
- **Anti-cheat:** FlatPace never injects into games; it only uses the driver profile, like the NVIDIA Control Panel.

## Limitations

- The driver applies the limit **when the game starts**: changes show up in the next session.
- It doesn't fix the game's own stutters (shader compilation, loading); it detects and reports them.

## Building from source

You need Windows with Visual Studio 2022 (or CMake with MSVC), or MinGW-w64 on Linux. Dear ImGui is already included in `third_party/imgui`.

```
cmake -B build
cmake --build build --config Release
```

To run it, put `PresentMon-2.6.0-x64.exe` next to `FlatPace.exe` (download it from the [PresentMon releases](https://github.com/GameTechDev/PresentMon/releases)), or use the package in this repository's *Releases* section, which already includes it. Automated tests run with `sh tests/run_tests.sh` or `ctest`. More details in the [technical documentation](TECHNICAL.md#12-building).

## License

FlatPace (© 2026 KALAVER084) is released under the [MIT License](LICENSE): you may use, modify and redistribute it freely, keeping the copyright notice.

## Privacy

FlatPace works entirely offline. It keeps its data in `%LOCALAPPDATA%\FlatPace` and its logs next to the program.

## Third-party credits and licenses

- [PresentMon](https://github.com/GameTechDev/PresentMon) (Intel, MIT License): frame measurement.
- [NVAPI](https://github.com/NVIDIA/nvapi) (NVIDIA, MIT License): driver profiles and G-SYNC status.
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT License): interface.
