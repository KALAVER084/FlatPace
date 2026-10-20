# FlatPace 0.36.2 technical documentation

This document explains how FlatPace works inside: architecture, algorithms, formulas, data formats and the sources behind its decisions. Some names (log lines, data files and keys) are in Spanish, as they appear in the program.

## 1. Architecture

| Component | Thread | Role |
|---|---|---|
| Window (Dear ImGui 1.91.5 + Direct3D 11) | Main | Interface, ~60 FPS in the foreground and ~10 in the background or minimized |
| Engine | Own | Game detection, statistics, decisions, driver; one pass per second |
| PresentMon reader | Own | Runs PresentMon 2.6.0 as a child process (`--output_stdout`) and parses its CSV line by line |
| Overlay | Main | Its own transparent, always-on-top, click-through window (custom anti-aliased drawing + `UpdateLayeredWindow`), up to 60 updates per second |

Threads communicate through mutex-protected structures (window state, overlay data, undo requests). The engine never blocks the interface.

**Stack:** C++20, Win32, Dear ImGui, Direct3D 11, NVAPI (loaded at runtime from `nvapi64.dll`), PresentMon. Code in two parts: `main.cpp` (~5,200 lines: Windows, interface, engine, driver and overlay) and `src/core/` (~660 lines: the pure logic, with no Windows dependencies, in eight modules: parameters, statistics, analysis and recommendations, EDID and pacing graph). `main.cpp` includes each module at the same point where its code used to be. Builds with MinGW-w64 (cross-compiling) or with CMake + MSVC.

## 2. Measurement

### 2.1 Real display refresh rate
`IDXGIOutput::WaitForVBlank` for 5 s. The average rate and the spread of the intervals are computed; a measurement with missed refreshes or high spread is repeated up to 3 times. If none is valid, the last good one saved in `pantalla.txt` is used.

### 2.2 Real and generated frames
- **Frame type** (PresentMon's `FrameType`): `Application` is real; anything else is generated.
- **Simulation markers** (`MsBetweenSimulationStart`, from NVIDIA Reflex / PC Latency): if a game emits them for 60 frames in a row, they are trusted; a frame without a marker is generated, and the real interval is the simulation one.
- Without markers, the **real interval** adds up the presents since the last real frame. The GPU work of generated frames is added to the real frame they accompany.
- **Generation factor** *k* = presents / real frames, rounded between 2 and 4 when more than 5 % are generated. A change in frame-generation state only counts if it lasts 3 s in a row.

### 2.3 Statistics
20 s moving window. Median, 5th, 95th and 99th percentiles, CPU and GPU work.
- **Work per frame** = min(max(CPU busy, GPU busy), real interval). The clamp corrects Reflex and in-game limiters, which count their waits as CPU busy time.
- **Stutter:** a real frame more than 2.5 times the median and at least 8 ms longer. Above 250 ms it's treated as a pause (loading, menu) and doesn't count.
- **Animation error:** PresentMon's `MsAnimationError` column (the mismatch between the pace of the simulation and that of the display); mean of its absolute value, percentage of frames above 1 ms and maximum. Only available in games where PresentMon can compute it.
- **Graph data:** the interval between presents of each real frame (sum of `MsBetweenPresents` since the previous real frame), the point the limiter regulates; Reflex simulation markers are used for analysis, not for the graph.
- **Deviation graph:** stretches with |median − reference| ≤ tolerance are drawn on the reference; tolerance = ±13 % of the frame time, between 0.5 and 2 ms (same rule with and without a limit).
- **Graph timeline:** each frame gets its timestamp when it arrives (previous one + its duration) and it never changes. A playback clock advances at real speed towards "latest data, extrapolated to the current clock, minus the buffer" (correction between 0.8× and 1.25×; it slows down smoothly within 0.4 s of the latest data). The buffer follows the largest recent interval between PresentMon batches (+0.35 s; it drops 0.05 s per second). Each 3-pixel stretch (fixed in time) takes the median of its frames, or its maximum if it contains a stutter, and is classified only once, when complete. Without a limit, the reference is the median of the last 10 s, smoothed over ~1.5 s. Vertical scale from target − r to target + 1.6 r, with r = max(2 ms, 50 % of the target).
- **Pace swing:** mean deviation of the medians of consecutive 100 ms windows from their median; it measures slow oscillations, which are what a limit corrects. The value from each game's first session without a limit is saved in `equipo.txt` (`ritmo-antes|game.exe`) as a reference.
- **Consistency:** mean |Δt| between consecutive real frames and the share of changes below 2 ms (the same consecutive-variation metric as CapFrameX). It's also computed over everything presented (generated frames included) and accumulated for the session summary.
- **Regime change:** if the median changes abruptly (settings, area, frame generation), measurement restarts after 3 s of settling.

### 2.4 Limit detector
A limit is active if the GPU isn't busy all the time (less than 90 % of the interval), frames are almost never faster than the median (p5 ≥ 0.9 × median, or 0.8 if the value matches a typical one within 0.5 %) and the median matches a divisor of the refresh rate (1 %) or a common limit (1.5 %).

## 3. Recommendation

Let *P* = 1000 / target real rate (ms) and *k* the generation factor.

### 3.1 Without VRR: divisors
Candidates: on-screen rate = Hz / *n* (*n* = 1…8·*k*), real rate = on-screen rate / *k* ≥ 30.
- **Stability:** the highest candidate with *P* ≥ p99 work × 1.05.
- **Tight:** if none qualifies, the highest one where at most 2 % of frames exceed *P* (without a limit, the pace would be irregular, which is worse).
- **Smoothness:** the highest candidate that qualifies using the real times of the last 3 minutes: p70 ≤ *P* × 1.08 and fewer than 3 % above *P* × 1.5.
- **With an active limit** (the game's or the app's), GPU work is misleading (clocks drop): if the game meets its pace with at most 2 % of frames above *P* × 1.25, that limit is sustainable and nothing lower is recommended.

### 3.2 With VRR (G-SYNC, FreeSync, Adaptive-Sync)
- **Ceiling:** `floor(min(Hz − 3, Hz − Hz²/4096))`; if the monitor's VRR range ends below its refresh rate, the same formula is applied to that maximum. At high refresh rates the formula wins (240 Hz → 225; 144 Hz → 138); at low ones, the classic 3 FPS margin (60 Hz → 57).
- **Smoothness:** real rate = 1000 / p70; **Stability:** 1000 / (p99 × 1.05).
- On-screen rate = min(ceiling, real rate × *k*), rounded down to a whole multiple of *k*.
- **Demonstrated pace:** if the game holds a limit (with few late frames), the recommendation never goes below that pace, and if the limit is the display's maximum, the ceiling is used. This avoids the bias of measuring with a limit in place (no frame can be faster than the limit, but some can be slower).

### 3.3 Alternative display refresh rate (without VRR)
If the monitor offers other refresh rates at the current resolution and one of them allows at least 10 % more steady FPS, it's planned for the next session (a temporary change with `ChangeDisplaySettingsEx` and `CDS_FULLSCREEN`, restored when the game or the app closes, or on undo).

## 4. Decision, verification and acceptance

1. **Decision when the game closes**, using the whole session: real frame times from the frame-generation state played the most, VRR if it was active more than half the time, demonstrated pace if the game was limited by itself most of the time. "Apply now" decides immediately with the data available.
2. **Verification** in the next session: the real pace matches the expected one (±1.5 %) and fewer than 5 % of frames are faster than the limit; with frame generation, real ±2 % and on-screen ±4 %.
3. **Acceptance:** a verified limit becomes accepted (it's saved) and is no longer recalculated.
4. **Manual limits** (set by the user): applied the same way (with VRR, limit + driver VSync) and verified, but they skip the decision on close and the failure rule; the app only reports if the game didn't hold them.
5. **Failure rule** (when each session with an accepted limit closes): the limit is lowered only if the game ran more than 13 % below it, in stretches of at least 5 s, for at least 10 % of the time and 60 s in total. With VRR, down to the pace exceeded 90 % of the time (10th percentile of per-second rates); without VRR, to the next divisor. It's never raised automatically.

## 5. Applying limits in the NVIDIA driver (NVAPI)

- DRS session → the game's profile (the existing one or a new one named `FlatPace - game.exe`).
- Settings used:
  - `FRL_FPS` (0x10835002): FPS limiter (whole numbers).
  - `VSYNCMODE` (0x00A879CF): `FORCEON` with VRR; 1/2, 1/3 or 1/4 refresh intervals without VRR when presentation is direct and the system supports it.
  - `PRERENDERLIMIT` (0x007BA09E) = 1, together with interval VSync.
- Before changing a setting its original value is saved; **undo** restores it or deletes the app's own profile.
- **Per-system learning:** whether the driver enforces its VSync (checked when the game reopens) and whether, with frame generation, the limiter acts on what's shown on screen or on the real frames (checked with frame generation stable for at least 8 s).

## 6. VRR detection

- **NVIDIA:** `NvAPI_Disp_GetVRRInfo` every 2 s; the state changes after 3 matching readings.
- **Any brand:** PresentMon timings. For each interval *x* between presents and between display changes (`MsBetweenDisplayChange`), its distance to the nearest refresh multiple is computed. If more than 30 % of presents are not multiples and the display shows them the same way (also more than 30 %), there's VRR; if the display rounds them (fewer than 10 %), there isn't. If the game already runs at an exact multiple, nothing is decided.
- **VRR range used:** the EDID's FreeSync range if present; otherwise the refresh range. Below the minimum, if the monitor has no LFC (maximum < 2 × minimum), the VRR recommendation is discarded and a divisor is used.
- **EDID** (Windows registry, from the monitor path given by `QueryDisplayConfig`): name, refresh range (0xFD descriptor) and, if present, the FreeSync range from AMD's block (OUI 00-00-1A).

## 7. Files

| File | Location | Contents |
|---|---|---|
| `cambios-aplicados.txt` | `%LOCALAPPDATA%\FlatPace` | One game per line (see format) |
| `equipo.txt` | `%LOCALAPPDATA%\FlatPace` | `idioma` (language), `prioridad` (priority), `overlay`, `divisores-siempre` (advanced setting), game names (`nombre|exe`), each game's swing without a limit (`ritmo-antes|exe`), learned driver VSync and limiter behavior with frame generation, display rate pending restore |
| `pantalla.txt` | `%LOCALAPPDATA%\FlatPace` | Last real refresh rate measured per monitor and mode |
| `registro-YYYYMMDD-HHMMSS.txt` | Program folder | Full log of each run (in Spanish) |

**`cambios-aplicados.txt` format (v2), tab-separated:**
`v2  exe  created(0/1)  method(0 limit / 1 VSync)  divisor  fps  n  [id_hex  had(0/1)  original_hex]×n` followed by optional fields: `fg k frl`, `hz rate`, `ok 1` (accepted), `base real_fps`, `vrr 1`, `manual 1` (set by the user: the app doesn't change it).

## 8. Advanced settings

No interface; in `%LOCALAPPDATA%\FlatPace\equipo.txt` (one `key=value` line), with the app closed:

| Key | Effect |
|---|---|
| `divisores-siempre=si` | Always use divisors of the refresh rate, even when G-SYNC/FreeSync is active. For displays that flicker with VRR (some OLED and VA panels). The *System* section shows it in the *VRR* row. |

Limits already applied don't change: to recalculate a game with the new setting, use *Undo*.

## 9. Foundations and sources

- **Refresh divisors:** without VRR, each image occupies a whole number of refreshes; only divisors give an even pace. Google applies the same rule in Android's Game Mode.
- **Variation versus average:** average FPS alone doesn't predict perceived quality when frame times vary (WPI, CHI 2023).
- **13 % perceptible threshold:** frame-rate perception is roughly logarithmic (Weber's law); a 13-15 % change is needed to notice it half of the time (frame-rate JND study in VR and desktop, 2025).
- **VRR ceiling:** Blur Busters' 3 FPS rule (G-SYNC 101) and the Hz − Hz²/4096 formula, inspired by Special K and matching Reflex's automatic cap.
- **Measurement:** PresentMon (Intel) and NVIDIA's Reflex marker methodology.

## 10. Known limitations and future work

- **No live changes:** the driver reads the limit when the game opens. An in-game limiter (DLL) would allow it, at a risk in games with anti-cheat.
- **AMD:** detection and recommendation work; applying limits is pending (ADLX: FRTC or Radeon Chill).
- **Censored data:** with an active limit, the game's real capacity is hidden; today this is handled with rules ("the demonstrated pace wins"); censored-data statistical methods (Kaplan-Meier, Tobit) could estimate it.
- **Phase detection** (BOCPD, Adams and MacKay 2007) to segment areas, menus and settings changes rigorously.
- Validation on more systems, monitors and games.

## 11. Building

MinGW-w64 (from Linux):
```
x86_64-w64-mingw32-windres app.rc -O coff -o app.res
x86_64-w64-mingw32-g++-posix -std=c++20 -municode -mwindows -O2 -DNDEBUG -Ithird_party/imgui \
  -Ithird_party/imgui/backends main.cpp imgui*.o app.res -o FlatPace.exe \
  -ld3d11 -ldxgi -ld3dcompiler_47 -ldwmapi -lgdi32 -lshell32 -luser32 -ladvapi32 -lversion \
  -limm32 -lwinmm -static -s
```
Windows: `cmake -B build && cmake --build build --config Release`. Dear ImGui (with the Win32 and DX11 backends) goes in `third_party/imgui`.

## 12. Automated tests

`tests/test_core.cpp` tests the logic in `src/core` without needing Windows (56 checks): percentiles and pace swing, consistency, the limit detector, recommendations with and without VRR (ceiling, the monitor's VRR range and LFC, Smoothness), EDID parsing and the pacing graph (tolerance, scale, stutters and stability of stretches while scrolling). Each test pins a behavior already validated with real sessions or in simulation.

- Linux/macOS: `sh tests/run_tests.sh`
- Windows (MSVC): `tests\run_tests.bat`, or `ctest` after building with CMake.

They were checked to catch deliberately introduced bugs similar to real regressions in the project (graph tolerance, VRR ceiling and stretch classification).
