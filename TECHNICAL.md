# FlatPace 0.62.2 technical documentation

This document explains how FlatPace works inside: architecture, algorithms, formulas, data formats and the sources behind its decisions. Some names (log lines, data files and keys) are in Spanish, as they appear in the program.

## 1. Architecture

| Component | Thread | Role |
|---|---|---|
| Window (Dear ImGui 1.91.5 + Direct3D 11) | Main | Interface, ~60 FPS in the foreground and ~10 in the background or minimized |
| Engine | Own | Game detection, statistics, decisions, driver; one pass per second. It also reads CPU performance (the Windows *% Processor Performance* counter) and checks that PresentMon keeps delivering data |
| PresentMon reader | Own | Runs PresentMon 2.6.0 as a child process (`--output_stdout`) and parses its CSV line by line |
| Overlay | Main | Its own transparent, always-on-top, click-through window (custom anti-aliased drawing + `UpdateLayeredWindow`), up to 60 updates per second; fixed backgrounds and texts kept between frames, integer pixel blending. Since 0.44 the GPU draws it: Dear ImGui (its own context, same Direct3D 11 device) into a composition swap chain (`CreateSwapChainForComposition`, premultiplied alpha) shown with DirectComposition in a layered, click-through `WS_EX_NOREDIRECTIONBITMAP` window; if it fails, it falls back to CPU drawing (`overlay=clasico` forces it) |

**Priority:** since 0.41, the FlatPace process runs at low priority (`BELOW_NORMAL_PRIORITY_CLASS`) and in Windows 11 efficiency mode (EcoQoS), so it doesn't compete with the game. Exceptions: the PresentMon reader thread (no efficiency mode, low priority; the pipe to PresentMon has a 1 MB buffer so it never blocks) and the window and overlay thread (no efficiency mode, low priority). PresentMon is also launched at low priority and in efficiency mode, and without input tracking (`--no_track_input`), which FlatPace doesn't use. Each session summary reports FlatPace's cost (engine, whole process, PresentMon and overlay).

Threads communicate through mutex-protected structures (window state, overlay data, undo requests). The engine never blocks the interface.

**Stack:** C++20, Win32, Dear ImGui, Direct3D 11, NVAPI (loaded at runtime from `nvapi64.dll`), PresentMon. Code in two parts: `main.cpp` (~6,700 lines: Windows, interface, engine, driver and overlay) and `src/core/` (~1,500 lines: the pure logic, with no Windows dependencies, in several modules: parameters, statistics, analysis and recommendations, the decision cycle (`decision.h`), statistical models, EDID and pacing graph). `main.cpp` includes each module at the same point where its code used to be. Builds with MinGW-w64 (cross-compiling) or with CMake + MSVC. The limit is requested from the driver in a vendor-neutral way (`LimitRequest`, in `src/core/limit_request.h`) and each vendor translates it to its driver through a common interface (`LimitBackend` in `main.cpp`); there are three: the NVIDIA one (automatic, with NVAPI); the RivaTuner one (`RtssBackend`, since 0.57: with RTSS's `RTSSHooks64.dll` library, it loads the `game.exe` profile, changes `FramerateLimit`, saves it and asks RTSS to reload profiles; to undo, it restores the profile's previous limit or deletes the one it created), used if the user chooses it as the limiter on *Games*, only with AMD or Intel (since 0.59; never without choosing it); and the guided one (`GuidedBackend`), used without NVIDIA when there is an AMD or Intel GPU: it records the limit without touching any driver, the window shows the user what to set and where, and the usual check (measuring the game) confirms that it works. Log messages are written in Spanish in the code and translated as they're written, based on the app's language, with the table in `src/core/log_text.h`.

## 2. Measurement

### 2.1 Real display refresh rate
`IDXGIOutput::WaitForVBlank` for 5 s. The average rate and the spread of the intervals are computed; a measurement with missed refreshes or high spread is repeated up to 3 times. If none is valid, the last good one saved in `pantalla.txt` is used.

### 2.2 Real and generated frames
- **Frame type** (PresentMon's `FrameType`): `Application` is real; anything else is generated.
- **Simulation markers** (`MsBetweenSimulationStart`, from NVIDIA Reflex / PC Latency): if a game emits them for 60 frames in a row, they are trusted; a frame without a marker is generated, and the real interval is the simulation one. Since 0.51.1, if more than 4 frames in a row arrive without a marker (with frame generation there are never more than 3 generated frames in a row), they stop being trusted until they return: some games (Space Marine 2) stop emitting them when frame generation is turned off, and also in menus and loading screens.
- Without markers, the **real interval** adds up the presents since the last real frame. The GPU work of generated frames is added to the real frame they accompany.
- **Generation factor** *k* = presents / real frames, rounded between 2 and 4 when more than 5 % are generated. A change in frame-generation state only counts if it lasts 3 s in a row.

### 2.3 Statistics
20 s moving window. Median, 5th, 95th and 99th percentiles, CPU and GPU work.
- **Work per frame** = min(max(CPU busy, GPU busy), real interval). The clamp corrects Reflex and in-game limiters, which count their waits as CPU busy time.
- **Stutter:** a real frame more than 2.5 times the median and at least 8 ms longer. Above 250 ms it's treated as a pause (loading, menu) and doesn't count.
- **Animation error:** PresentMon's `MsAnimationError` column (the mismatch between the pace of the simulation and that of the display); mean of its absolute value, percentage of frames above 1 ms (compared with the game's first session without a limit and without frame generation, saved as `movimiento-antes|game.exe`) and maximum. Only available in games where PresentMon can compute it.
- **Motion with a flat pace:** percentage of real frames that last the same as the previous one (±0.3 ms), i.e. perfect on the graph, whose animation error exceeds 13 % of the frame. It measures motion judder the graph can't show. In the recordings: Greylock and Megabonk with a limit, ~2 %; Greylock without a limit, 16 %; Space Marine 2 with frame generation, 17-37 %, and without it, 0.5 % in the same session. With frame generation and 10 % or more, the summary attributes it to frame generation (not to the limit). It only reports: it doesn't take part in any decision, because in games without frame generation it rises and falls along with the 1 % low, which is already used.
- **Graph data:** the interval between presents of each real frame (sum of `MsBetweenPresents` since the previous real frame), the point the limiter regulates; Reflex simulation markers are used for analysis, not for the graph.
- **Deviation graph:** stretches with |median − reference| ≤ tolerance are drawn on the reference; tolerance = ±13 % of the frame time, between 0.5 and 6 ms (same rule with and without a limit; up to 0.39.1 the maximum was 2 ms, stricter than 13 % below ~65 FPS). With frame generation (seen in the session or with which the limit was applied), the graph draws each image's on-screen interval (`MsBetweenDisplayChange`), with the on-screen limit as reference. In that mode, each stretch takes the mean image duration over ~125 ms (5 stretches) and only an image over 2.5× is a stutter (with real frames: the stretch median and a stutter at 1.5×).
- **Graph timeline:** each frame gets its timestamp when it arrives (previous one + its duration) and it never changes. A playback clock advances at real speed towards "latest data, extrapolated to the current clock, minus the buffer" (gentle correction, between 0.95× and 1.05×; it slows down within 0.25 s of the latest data). The buffer follows the largest recent interval between PresentMon batches (+0.6 s; it drops 0.05 s per second). Up to 0.42.1, with +0.35 s and a 0.8-1.25× correction, the clock slowed down before each batch and sped up after it: the advance per draw varied between 12.4 and 20.8 ms (now between 15.8 and 17.5). Each 3-pixel stretch (fixed in time) takes the median of its frames, or its maximum if it contains a stutter, and is classified only once, when complete, also fixing its value; only complete stretches are drawn. Without a limit, the reference is the median of the last 10 s, smoothed over ~1.5 s. Vertical scale from target − r to target + 1.6 r, with r = max(2 ms, 50 % of the target).
- **Pace swing:** mean deviation of the medians of consecutive 100 ms windows from their median; it measures slow oscillations, which are what a limit corrects. The value from each game's first session without any limit is saved in `equipo.txt` (`ritmo-antes2|game.exe`) as a reference.
- **Consistency:** mean |Δt| between consecutive real frames and the share of changes below 2 ms (the same consecutive-variation metric as CapFrameX). It's also computed over everything presented (generated frames included) and accumulated for the session summary.
- **Regime change:** if the median changes abruptly (settings, area, frame generation), measurement restarts after 3 s of settling.

### 2.4 Limit detector
A limit is active if the GPU isn't busy all the time (less than 90 % of the interval), frames are almost never faster than the median (p5 ≥ 0.9 × median, or 0.8 if the value matches a typical one within 0.5 %) and the median matches a divisor of the refresh rate (1 %) or a common limit (1.5 %).

### 2.5 Session summary diagnostics
- **Slow moments:** frames more than 13 % above the limit's frame time (without a limit, the slowest 10 %). They're classified as CPU-limited if their CPU work exceeds their GPU work by 10 %, and vice versa. With 70 % or more of either, the summary gives the matching advice (CPU: lowering graphics won't help; GPU: lowering resolution or graphics will). In the test laptop's recordings, the CPU limits the slow moments of Greylock, Megabonk and Space Marine 2 (82-100 %); in Control Resonant, both do.
- **CPU performance:** Windows *% Processor Performance* counter (100 = nominal frequency), once per second: median, minimum, and in slow seconds versus the rest. If the CPU limits and lowers its frequency by more than 10 % right in the slow seconds, a possible heat or power problem is reported.
- **GPU queue wait** (`MsGPULatency`): with the limit working, median and worst 1 %.
- **PC latency** (`MsPCLatency`): median and worst 1 %, with and without the limit. PresentMon computes it from the Reflex SDK's *PC Latency Stats* events; although PresentMon provides the column, it has always arrived empty in the games tested (including Space Marine 2, which emits the simulation markers).

## 3. Recommendation

Let *P* = 1000 / target real rate (ms) and *k* the generation factor.

### 3.1 Without VRR: divisors
Candidates: on-screen rate = Hz / *n* (*n* = 1…8·*k*), real rate = on-screen rate / *k* ≥ 30.
- **Steady pace:** the highest candidate with *P* ≥ p99 work × 1.05.
- **Tight:** if none qualifies, the highest one where at most 2 % of frames exceed *P* (without a limit, the pace would be irregular, which is worse).
- **More FPS:** the highest candidate that qualifies using the real times of the last 3 minutes: p70 ≤ *P* × 1.08 and fewer than 3 % above *P* × 1.5.
- **With an active limit** (the game's or the app's), GPU work is misleading (clocks drop): if the game meets its pace with at most 2 % of frames above *P* × 1.25, that limit is sustainable and nothing lower is recommended.

### 3.2 With VRR (G-SYNC, FreeSync, Adaptive-Sync)
- **Ceiling:** `floor(min(Hz − 3, Hz − Hz²/4096))`; if the monitor's VRR range ends below its refresh rate, the same formula is applied to that maximum. At high refresh rates the formula wins (240 Hz → 225; 144 Hz → 138); at low ones, the classic 3 FPS margin (60 Hz → 57).
- On real frame times **without stutters** (> 2.5 × median) from the last minutes (or the whole session when deciding on close): **More FPS:** real rate = 1000 / p90; **Steady pace:** (1 + 0.13) × 1000 / p99, that is, the slowest 1 % arrives at most 13 % late. Validated in 0.39 with real sessions: in Greylock the previous More FPS rule (p70) left the game perceptibly below the limit 29 % of the seconds, and the Steady pace one (1000 / (p99 × 1.05) with stutters) gave 78-85 FPS; with the new ones, 100 and 91, and at 100 the game-caused 1 % low was 96 FPS.
- **With an active limit** (censored data), the recommendation never exceeds the limit, uses game-caused times and only goes down if the game falls perceptibly below it (> 13 %); without that hysteresis, the limiter's jitter lowered it 1-2 FPS per session. With a demonstrated pace (the game locked by its own limit or VSync), exactly that pace is used.
- On-screen rate = min(ceiling, real rate × *k*), rounded down to a whole multiple of *k*.
- **Demonstrated pace:** if the game holds a limit (with few late frames), the recommendation never goes below that pace, and if the limit is the display's maximum, the ceiling is used. This avoids the bias of measuring with a limit in place (no frame can be faster than the limit, but some can be slower).

### 3.3 Alternative display refresh rate (without VRR)
If the monitor offers other refresh rates at the current resolution and one of them allows at least 10 % more steady FPS, it's planned for the next session (a temporary change with `ChangeDisplaySettingsEx` and `CDS_FULLSCREEN`, restored when the game or the app closes, or on undo).

## 4. Decision, verification and acceptance

1. **Decision when the game closes**, using the whole session: real frame times from the frame-generation state played the most, VRR if it was active more than half the time, demonstrated pace if the game was limited by itself most of the time. "Apply now" decides immediately with the data available.
2. **Verification** in the next session: the real pace matches the expected one (±1.5 %) and fewer than 5 % of frames are faster than the limit; with frame generation, real ±2 % and on-screen ±4 %.
3. **Acceptance:** a verified limit becomes accepted (it's saved) and is no longer recalculated.
4. **Manual limits** (set by the user): applied the same way (with VRR, limit + driver VSync) and verified, but they skip the decision on close and the failure rule; the app only reports if the game didn't hold them.
5. **Failure rule** (when each session with an accepted limit closes, not counting the last minute before the game closes, which is usually menus, exiting or hangs): the limit is lowered only if the game ran more than 13 % below it, in stretches of at least 5 s, for at least 10 % of the time and 60 s in total. With VRR, down to the pace exceeded 90 % of the time (10th percentile of per-second rates); without VRR, to the next divisor. The failure rule only lowers; raises are described in the next point.
6. **Automatic raise** (`DecideClimb` in `src/core/decision.h`; since 0.45, with VRR and without frame generation, non-manual limits): on close, if the game had headroom **the whole session** (≥ 3 min with the limit working, no stretches below, ≤ 1 % of isolated seconds more than 13 % below, and > 98 % of the session's frames at the limit according to the censored-data model), the limit rises to the next step: min(limit × 1.13, VRR ceiling). With *More FPS* one session is enough; with *Steady pace*, two in a row. For these limits, with *Steady pace* priority, the failure rule also applies to the game-caused 1 % low (20 s window; with *More FPS* only the median counts, since 0.48.2), because a raise can keep the median and sink only the heavy areas (Greylock at 125: 34-48 % of the time more than 13 % below; at 111, 2.5 %); if only the 1 % low fails and there's no saved previous limit, it lowers one step (limit / 1.13), and "headroom" requires it not to have sunk. Ping-pong protections: if a raise fails, it goes back to the previous limit instead of the pace exceeded 90 % of the time; the limit that failed is kept as a ceiling for 30 days and it won't rise up to it. Per-game state in `equipo.txt` (`escalada|game.exe`: ceiling;day;previous;streak); *Undo* clears it. Validated with the recordings: Megabonk at 195 (headroom, rises to 220) and Greylock at 83, 90 and 100 (no raise: 97-98 % at the limit). Since 0.55, the app only changes limits and priority on its own with the *Automatic* general priority and in games without their own priority (`AppActsAlone`); otherwise, each raise, lowering (including the failure rule) or switch to *Steady pace* is saved as a **recommendation** and shown on the game's row with a button to apply it. A recommendation to lower is urgent (in red) if the limit uses interval VSync: without G-SYNC, a limit that isn't held drops to the next step and makes motion worse. It's withdrawn automatically if the game holds its limit in a later session. A new game's first decision is applied in every mode, because that's what *Apply limits* is for.
7. **Per-game priority** (`UpdateAutoPriority` in `src/core/decision.h`; since 0.49, for the same limits as the automatic raise): the global priority is the starting point; with *More FPS*, if the game-caused 1 % low fails the failure rule (without the last minute) in two sessions in a row, the game moves to *Steady pace*; a clean session of at least 3 minutes breaks the streak. Only in that direction (with a limit that holds, the game looks stable because of the limit itself). Measured with the limit in place: without a limit, instability is exaggerated. State in `equipo.txt` (`prioridad|game.exe`: chosen;streak); *Undo* clears it; with a fixed general priority (*Steady pace* or *More FPS*, since 0.54) it doesn't act. Users can fix each game's priority from its row (`prioridad-fija|game.exe`: fluidez or estabilidad); a fixed priority takes precedence and the app doesn't change it.

## 5. Applying limits in the NVIDIA driver (NVAPI)

- DRS session → the game's profile (the existing one or a new one named `FlatPace - game.exe`).
- Settings used:
  - `FRL_FPS` (0x10835002): FPS limiter (whole numbers).
  - `VSYNCMODE` (0x00A879CF): `FORCEON` with VRR; 1/2, 1/3 or 1/4 refresh intervals without VRR when presentation is direct and the system supports it.
  - `PREFERRED_PSTATE` (0x1057EB71) = 1, *Prefer maximum performance*, with every limit: it keeps the driver from lowering the GPU clocks until it barely keeps up (in Greylock at 90 FPS, work per frame almost doubled at the same 85 % utilization, and heavy scenes went over the limit). When the app opens it's added to changes that lacked it. Can be disabled with `rendimiento-maximo=no`. On laptops, CPU and GPU share power: in Greylock, with it the CPU runs 10 % slower (123-129 % of its nominal frequency versus 136-138 %), but without it the GPU relaxes and, with the areas properly separated, the 1 % low gets 9 % worse in light areas and only 5 % better in heavy ones (CPU-limited). A relaxed GPU also skews the measurements the limit is decided with (the spiral of the first versions). That's why it's always kept; deciding it per game was studied and discarded.
  - `PRERENDERLIMIT` (0x007BA09E) = 1, *Low Latency Mode: On* (a one-frame render queue), with every limit since 0.50 (before, only with interval VSync, which it's part of): when the game can't reach the limit and the GPU saturates, frames don't pile up in the queue. In a simulation with a saturated GPU, PC latency in those areas dropped from 40 to 20 ms with no FPS cost; with the limit held nothing changes. In Greylock it had no measurable effect, because its heavy areas are CPU-limited and the queue never fills. In games with Reflex, Reflex takes precedence. It's added at startup to limits that lacked it; it can be disabled with `baja-latencia=no`.
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
| `equipo.txt` | `%LOCALAPPDATA%\FlatPace` | Preferences (`idioma`, `prioridad`: `auto`, `estabilidad` (steady pace) or `fluidez` (more FPS), `overlay`, `grabar-fotogramas` and the advanced settings in section 8) and per-game state: `nombre|exe`, no-limit references (`ritmo-antes2|`, `latencia-antes|`, `movimiento-antes|`), memory (`memoria3|`), automatic raise (`escalada|`), pending recommendations (`sugerencia|`: limit;type;current limit, with type 0 raise, 1 lower and 2 urgent lower; `prioridad-sugerida|`), priority (`prioridad|`, `prioridad-fija|`); what was learned about the PC (`vsync-driver|`, `limite-con-generacion|`, `gsync-visto|display`: G-SYNC seen working on that display, to warn if it stops) and a display rate pending restore. At startup, the app removes keys from previous versions that are no longer used |
| `pantalla.txt` | `%LOCALAPPDATA%\FlatPace` | Last real refresh rate measured per monitor and mode |
| `registro-YYYYMMDD-HHMMSS.txt` | Program folder | Full log of each run (in Spanish) |
| `grabaciones/grabacion-game.exe-YYYYMMDD-HHMMSS.csv` | Program folder | Optional: one frame per row (see format) |

**`cambios-aplicados.txt` format (v2), tab-separated:**
`v2  exe  created(0/1)  method(0 limit / 1 VSync)  divisor  fps  n  [id_hex  had(0/1)  original_hex]×n` followed by optional fields: `fg k frl`, `hz rate`, `ok 1` (accepted), `base real_fps`, `vrr 1`, `manual 1` (set by the user: the app doesn't change it).

**Recording format (CSV):** header lines starting with `#` (version, game, real refresh rate, VRR range, priority and limit at start) and one row per game frame with these columns (unavailable values are left empty):

| Column | Content |
|---|---|
| `t_llegada_s` | When the data arrived (PresentMon delivers it in batches) |
| `t_acumulado_s` | Sum of presents: session time |
| `presentacion_ms` | Interval between presents (`MsBetweenPresents`) |
| `real_ms` | Real frame interval (with Reflex markers, the simulation one); empty for generated frames |
| `real_presentacion_ms` | Interval between presents of real frames |
| `generado` | 1 if the frame is generated |
| `cpu_ms`, `gpu_ms` | CPU and GPU work (`MsCPUBusy`, `MsGPUBusy`) |
| `pantalla_ms` | Interval between image changes on screen (`MsBetweenDisplayChange`) |
| `error_animacion_ms` | `MsAnimationError` |
| `vrr`, `limite_confirmado` | State at that moment (0/1) |
| `latencia_pc_ms` | `MsPCLatency` (0.47; see 2.5) |
| `espera_gpu_ms` | `MsGPULatency`: from when the game hands over the frame until the GPU starts it (0.50) |
| `cpu_rendimiento_pct` | CPU performance in that second, in % of its nominal frequency (0.51) |

## 8. Advanced settings

No interface; in `%LOCALAPPDATA%\FlatPace\equipo.txt` (to open the folder: Windows + R, type `%LOCALAPPDATA%\FlatPace` and Enter), with the app closed. Each setting is one `key=value` line; the app writes its own with a tab instead of the `=`, and since 0.50.1 it accepts both forms (before, a line with `=` was ignored). If a key isn't there, its default value is used:

| Key | Effect |
|---|---|
| `limitador=` | The limiter chosen on *Games*, only with AMD or Intel: `recomendar` (guided mode) or `rtss`. With NVIDIA the driver is always used (`driver`); if limits were left in RTSS from 0.58, they're moved back to the driver at startup. When it's changed from the window, each limit is undone with the limiter that applied it and the same value is applied with the new one (interval VSync becomes an FPS limit). With RTSS, without "GPU at max" or the driver's low latency, and without the 1 % low rule. |
| `baja-latencia=no` | Don't pair limits with *Low Latency Mode* (a one-frame render queue); at startup, the app also removes it from limits that already had it (except with interval VSync, where it's part of the method). |
| `rendimiento-maximo=no` | Don't pair limits with *Prefer maximum performance*; at startup, the app also removes it from limits that already had it. |
| `overlay=clasico` | Always use the classic overlay (drawn by the CPU) instead of the GPU one. |
| `divisores-siempre=si` | Always use divisors of the refresh rate, even when G-SYNC/FreeSync is active. For displays that flicker with VRR (some OLED and VA panels). The *System* section shows it in the *VRR* row. |

Limits already applied don't change: to recalculate a game with the new setting, use *Undo*.

## 9. Statistical models (0.37)

The censored-data model already takes part in one decision: it decides whether the game has headroom for the automatic raise (point 6 of section 4). The rest report and suggest.

- **With frame generation** (seen in the session or with which the limit was applied), capacity, the game-caused 1 % low, memory and suggestions aren't computed: they assume the limit always applies to real frames, and with intermittent frame generation it doesn't.
- **Censored data** (`src/core/censored.h`): with a limit of period *c*, the limiter's variation is measured in each session (median absolute deviation × 1.4826 of frames between 0.85 *c* and 1.15 *c*), and frames ≤ *c* + max(3 % of *c*, 3 × variation) are treated as censored ("would take at most *c*"); the rest as measurements. A log-normal is fitted by maximum likelihood (Nelder-Mead), without pauses (> 250 ms) or stutters (> 2.5 × median). It needs ≥ 200 frames, between 5 % and 98 % censoring and ≥ 30 visible frames; above 98 % the game "has plenty of headroom". The p99 is reliable; the median only with ≤ 90 % censoring. **Validation with real sessions (0.37.1):** with the driver limiter, frame times are spread symmetrically around the limit (±0.2 ms in Megabonk, ±0.4 ms in Greylock); with the previous threshold (fixed +3 %), Megabonk at 195 FPS looked capable of a ~205 FPS median when it ran at ~300; now it reports "plenty of headroom", and in Greylock at 90 FPS the estimated p99 (74 FPS) matches the measured one (76).
- **Game-caused 1 % low** (`GameCausedPercentileMs`, in `src/core/analysis.h`): with an active limit of period *c*, a frame slower than *c* counts as such only if its work (max of CPU and GPU busy) exceeds 0.95 × *c*; otherwise it counts as *c*, because the delay came from the limiter's timer. Validated with real sessions: Megabonk at 195 FPS, 176 measured → 195 from the game; Greylock at 90, 76 → 83. Used for the 1 % low in the window, the overlay and the log.
- **Per-game memory** (`src/core/game_memory.h`): weighted mean and variance (Welford) of the log of the pace the game holds in each session (the Steady pace criterion: 1.13 × 1000 / p99 without stutters; with a limit, only if the game-caused slowest 1 % falls more than 13 % below the limit, as protection against the downward spiral), weighted by minutes played (up to 10) and forgetting ×0.8 per session. Uncertainty = √(between-session variance + 0.08² / sessions); 80 % lower bound = exp(mean − 0.84 × uncertainty). Saved in `equipo.txt` (`memoria3|game.exe`).
- **Phases** (`src/core/bocpd.h`): BOCPD with a normal-gamma prior (κ₀ = 0.02, α₀ = 2, prior standard deviation 0.03 on the log scale), change hazard 1/60 per second and runs truncated at 300 s, over the log of each second's median filtered with a 3 s median. A change is proposed when the most likely run returns to ≤ 3 s for two steps after having been ≥ 8 s; the phase is confirmed if, after 10 s, its median differs ≥ 8 % from the previous one.
- **Suggestion** (`src/core/suggest.h`): next step = with VRR, min(limit × 1.13, ceiling); without VRR, the next divisor. It's suggested if the game has plenty of headroom, or if the estimated capacity reaches the step and, with two or more sessions in memory, its lower bound reaches 95 % of the step.

## 10. Foundations and sources

- **Refresh divisors:** without VRR, each image occupies a whole number of refreshes; only divisors give an even pace. Google applies the same rule in Android's Game Mode.
- **Variation versus average:** average FPS alone doesn't predict perceived quality when frame times vary (WPI, CHI 2023).
- **13 % perceptible threshold:** frame-rate perception is roughly logarithmic (Weber's law); a 13-15 % change is needed to notice it half of the time (frame-rate JND study in VR and desktop, 2025).
- **VRR ceiling:** Blur Busters' 3 FPS rule (G-SYNC 101) and the Hz − Hz²/4096 formula, inspired by Special K and matching Reflex's automatic cap.
- **Measurement:** PresentMon (Intel) and NVIDIA's Reflex marker methodology.

## 11. Known limitations and future work

- **No live changes:** the driver reads the limit when the game opens. An in-game limiter (DLL) would allow it, at a risk in games with anti-cheat.
- **AMD and Intel: *Recommend only*** (guided mode, since 0.56) by default, or **limits with RTSS** (since 0.57) if the user chooses it as the limiter. The app measures, decides and checks the limit, but the user applies it (in AMD Software, with Radeon Chill; in Intel Graphics Software, with its FPS limiter; or with the game's own limiter), following the instruction shown on the game's row. Automatic mode isn't possible with the AMD kit (ADLX: it forbids redistributing its files and requires an end-user agreement forbidding copying and studying the program) or the Intel one (IGCL: only for Intel platforms and with an indemnity clause), because their licenses don't fit an open-source MIT project (see the README). Without "GPU at max", low latency or interval VSync, which are NVIDIA driver settings: the instruction reminds the user to turn on the driver's VSync with FreeSync or Adaptive-Sync, and the vendor's low latency option. Without "GPU at max", the GPU relaxes with the limit and the 1 % low can look worse than it is (the spiral of the first versions), so in guided mode the 1 % low rule doesn't lower limits or change the priority (since 0.56.1). Not yet tested on those GPUs.
- **PresentMon 2.6.0** has known bugs with the options FlatPace uses (PresentMon issue 695): it can accumulate internal data and stop delivering frames, for example when frame generation is turned off in some games. Since 0.51.3, FlatPace restarts it if it stops delivering data for more than 6 s with the game in the foreground. It should be updated once Intel publishes a fixed version.
- **PC latency:** the measurement is integrated, but PresentMon hasn't delivered it in the games tested (section 2.5).
- **Frame generation:** it causes motion judder even when the pace is flat (section 2.3); no limit fixes it, and FlatPace only reports it.
- **Validate the statistical models with real recordings** and then let them influence decisions (raise limits with the estimated capacity, decide with the game's memory and the most demanding phase).
- Validation on more systems (desktops, no VRR, AMD), monitors and games: everything measured comes from a laptop with an RTX 4080 and a 240 Hz G-SYNC display, and the system can change the conclusions (as happened with "GPU at max").

## 12. Building

MinGW-w64 (from Linux):
```
x86_64-w64-mingw32-windres app.rc -O coff -o app.res
x86_64-w64-mingw32-g++-posix -std=c++20 -municode -mwindows -O2 -DNDEBUG -Ithird_party/imgui \
  -Ithird_party/imgui/backends main.cpp imgui*.o app.res -o FlatPace.exe \
  -ld3d11 -ldxgi -ld3dcompiler_47 -ldwmapi -ldcomp -lgdi32 -lshell32 -luser32 -ladvapi32 -lversion \
  -limm32 -lwinmm -lpdh -static -s
```
Windows: `cmake -B build && cmake --build build --config Release`. Dear ImGui (with the Win32 and DX11 backends) goes in `third_party/imgui`, and the NVAPI declarations FlatPace uses (from NVIDIA, MIT License) in `third_party/nvapi/nvapi_min.h`: all third-party code lives in `third_party/`.

## 13. Automated tests

`tests/test_core.cpp` tests the logic in `src/core` without needing Windows (417 checks, including that each log translation keeps its original's slots; among them, that the translation to NVIDIA produces exactly the settings of previous versions): percentiles and pace swing, consistency, the limit detector, recommendations with and without VRR (ceiling, the monitor's VRR range and LFC, More FPS), EDID parsing, the pacing graph (tolerance, scale, stutters, on-screen mode and stability of stretches while scrolling and at both edges), the statistical models (capacity recovery from censored data, phase detection and its resistance to isolated spikes, per-game memory and suggestion rules) and the decision cycle (verification, failure rule, lowering after a failure, the decision on close and multi-session simulations that check there is no downward spiral). Each test pins a behavior already validated with real sessions or in simulation.

- Linux/macOS: `sh tests/run_tests.sh`
- Windows (MSVC): `tests\run_tests.bat`, or `ctest` after building with CMake.

They were checked to catch deliberately introduced bugs similar to real regressions in the project (graph tolerance, VRR ceiling and stretch classification).
