# FlatPace

**FlatPace measures your games and applies the frame-rate limit that makes them run as steadily as possible.** The idea behind it: a steady pace feels smoother than higher but uneven FPS.

Current version: **0.67.3** · Windows 10 (1803 or later) and Windows 11. [User guide](GUIDE.md) · [Technical documentation](TECHNICAL.md) · [License](LICENSE) · [Español](README.es.md). The changelog ([CHANGELOG.md](CHANGELOG.md)) is in Spanish; the app has an English interface (ES | EN selector).

![FlatPace's overlay in Greylock, without a limit and with the limit the app chose](docs/img/overlay-antes-despues.png)

*The overlay in Greylock. Left: without a limit, 225 FPS but uneven (pace swing ±0.9 ms; the slowest 1 % at 150). Right: with the limit FlatPace chose, a completely flat 129 FPS (±0.1 ms; the slowest 1 % also at 129).*

**[Download the latest version](https://github.com/KALAVER084/FlatPace/releases/latest)** · Automatic with NVIDIA · Guided with AMD and Intel (not tested yet)

## Quick start

1. Download `FlatPace-x.x.x.zip` from the [latest release](https://github.com/KALAVER084/FlatPace/releases/latest) and unzip it anywhere (nothing to install).
2. Run `FlatPace.exe` and accept the administrator prompt (PresentMon and the NVIDIA driver need it).
3. Play normally. When you close the game, FlatPace saves the limit for the next session (with AMD or Intel, unless you choose RivaTuner as the limiter, it tells you on *Games* what to set and where).
4. In the next session, the app checks that it works ("Limit working") and it becomes **accepted**.

If Windows shows "Windows protected your PC", click *More info* → *Run anyway*: it appears because the program isn't digitally signed.

## What it does

- **Measures each game** while you play, frame by frame.
- **Picks the limit your game can actually hold** when you close it, using the whole session.
- **Applies it in the NVIDIA driver,** without touching the game, and checks that it works.
- **Keeps adjusting it:** lowers it if the game can't hold it, raises it if it has headroom.
- **Shows you why:** stutters, whether the CPU or the GPU holds the game back, an in-game overlay with the pacing graph and, in *Games*, where each limit comes from, with its formula and charts.
- **Fully reversible and offline,** in English and Spanish.

## What it looks like

Screenshots with the English interface.

**In the game**, the overlay shows the real FPS, the slowest 1 %, the limit's status and the pacing graph of the last few seconds:

![Greylock with the 129 FPS limit and FlatPace's overlay](docs/img/partida-con-limite.jpg)

**Now** shows a one-sentence verdict, the full-width pacing graph (blue once the limit is checked; stutters in amber) and the session's numbers:

![The Now section: pacing graph and session numbers](docs/img/ahora.png)

**Games** brings together the priority and each game with its limit, described in words, and its buttons (its own priority, manual limit and undo). Unfolding a game shows where its limit comes from: the rule and the formula with its numbers, the curve of the session that decided it, the last session second by second against the 13 % threshold and each rule's result:

![Greylock's dropdown in Games: the limit's formula, the curve of the deciding session and the check of the last session, with their legends](docs/img/juegos.png)

## Requirements and compatibility

| | Measure and decide | Apply the limit | Check that it works |
|---|---|---|---|
| NVIDIA | Yes | Automatic, in the driver | Yes |
| AMD / Intel (not tested yet) | Yes | *Recommend only*: you apply it following the app's instructions; or automatic, if you choose RivaTuner (RTSS) as the limiter (experimental) | Yes |

- **Displays without VRR:** limits that divide the real refresh rate exactly (for example 240, 120, 80, 60 on a 240 Hz display).
- **G-SYNC / FreeSync / Adaptive-Sync:** limits at the exact pace the game sustains, below the display's maximum.
- **Laptops in hybrid mode:** they work, but without G-SYNC; the app warns you and explains how to turn it on.
- **Games with anti-cheat:** FlatPace doesn't hook into the game; it only uses the driver profile, like the NVIDIA Control Panel does.

## Limitations

- The driver applies the limit **when the game starts**: changes show up in the next session.
- With AMD and Intel, "GPU at max" and the driver's low latency mode aren't available, and you turn on the driver's VSync yourself; without RTSS, you also apply the limit yourself.
- It doesn't remove the game's own stutters (shader compilation, loading): it detects and shows them.
- Frame generation can make motion uneven even when the pace is steady: FlatPace measures and explains it, but no limit fixes it.
- PC latency only appears if the game provides the necessary data (some games with Reflex; among those tested, Control Resonant).
- Without a limit (a game's first session, or after *Undo*), the game keeps the GPU fully busy and the overlay may stutter: its images wait behind the game's. With the limit applied it's smooth.
- It uses PresentMon 2.6.0, which has known bugs; if it stops sending data mid-session, FlatPace restarts it.
- With FlatPace in front, the *Print Screen* key doesn't open the Windows 11 snipping tool: FlatPace runs as administrator, and Windows doesn't let the tool see the keyboard while an administrator window has the focus. *Win + Print Screen* (full screen) and *Win + Shift + S* do work.
- Tested mostly on a laptop with an RTX 4080 and a 240 Hz G-SYNC display, with a few games (Greylock, Megabonk, Metal Eden, Space Marine 2 and Control Resonant). RTSS limits and guided mode haven't been tested on AMD or Intel GPUs yet: reports are welcome.

## How it works

For the curious: the rules behind each decision, with their sources. Click a title to open it.

<details>
<summary><b>The 13 % rule (Weber's law)</b></summary>

Weber's law (Weber, 1846) says that the smallest difference we perceive in a stimulus isn't a fixed amount, but a proportion of its size. A 2025 study (Naylor-Lovell, Chalmers and Debattista, 2025) measured it for on-screen motion: the rate at which a moving object is updated has to change **by 13 to 15 %** for people to notice that it moves more or less smoothly half of the time, in virtual reality and on a desktop, and that proportion stayed constant across the range studied, with one exception explained below. The study measures an object's update rate, not the duration of each frame in a game, but it's a very close phenomenon, and studies on frame rate find perceptible steps of the same order: from 50 to 60 Hz, or from 100 to 125 Hz (Denes et al., 2020).

**How FlatPace applies it.** The app never compares fixed milliseconds: every *"noticeable / not noticeable"* threshold is **13 % of the current pace**, the strictest end of the study. That way the same rule works at any speed: a frame 1 ms slower is a perceptible difference at 240 FPS (24 % of the 4.2 ms it lasts), but not at 60 FPS (6 % of 16.7 ms). And since 13 % fewer FPS means frames 15 % longer, the threshold falls within the study's range whether it's measured in FPS or in duration. It's used in:
- **The failure rule:** a limit fails if the game stays more than 13 % below it in a sustained way (in stretches of at least 5 seconds, adding up to a minute and 10 % of the session). Shorter drops count as isolated dips and are ignored.
- **Raises:** one 13 % step, just enough to be noticeable. Lowerings by the *Steady pace* formula don't go beyond one step per session either.
- ***Steady pace*:** the slowest 1 % of frames can arrive at most 13 % late, so the limit is 1.13 times the FPS of that slowest 1 %. When each session closes, the same formula is applied to the whole session's slowest 1 % to check the limit. And if, with *More FPS*, that 1 % stays more than 13 % below the limit in two sessions in a row, the game moves to *Steady pace*.
- **Out-of-step motion:** a frame is out of step if it's drawn with an animation error larger than 13 % of its duration.
- **The pacing graph:** its band marks ±13 % of the frame time (with a minimum of 0.5 ms and a maximum of 6 ms, which only kick in above about 260 FPS or below about 22).

**What doesn't come from Weber's law:** the other figures (the 90th percentile of *More FPS*, the ±1.5 % of the check, the 98 % of *"has headroom"*, stutters of more than 2.5 times the usual…) are measurement margins and design criteria, tuned with the sessions measured during development, not perception thresholds.

The same study (Naylor-Lovell, Chalmers and Debattista, 2025) found something else FlatPace applies: the threshold **grows a lot when the display's refresh rate isn't a whole multiple of the motion's rate**, because some images last one refresh and others two, and that uneven rhythm masks the differences. That's why, without G-SYNC, FlatPace only uses limits that divide the display's Hz exactly.

**References**
- Weber, E. H. (1846). *Der Tastsinn und das Gemeingefühl*. In R. Wagner (ed.), *Handwörterbuch der Physiologie*, vol. 3. The work in which Weber describes that the perceptible difference is proportional to the stimulus.
- Naylor-Lovell, W., Chalmers, A. and Debattista, K. (2025). [*Tick rate-frame rate cross interaction and the impact upon just noticeable differences in virtual reality*](https://doi.org/10.1007/s10055-025-01227-9). *Virtual Reality*, 29, 149. Open access. The source of the 13-15 % and of the non-whole-multiple exception.
- Denes, G., Jindal, A., Mikhailiuk, A. and Mantiuk, R. K. (2020). [*A perceptual model of motion quality for rendering with adaptive refresh-rate and resolution*](https://doi.org/10.1145/3386569.3392411). *ACM Transactions on Graphics*, 39(4), article 133. The perceptible steps in frame rate.

</details>

<details>
<summary><b>How it chooses, checks and adjusts the limit</b></summary>

**How it chooses the limit.** When you close the game, with the data from the whole session (leaving out stutters, frames more than 2.5 times the usual, which no limit fixes):
- **More FPS:** the pace 90 % of the session's frames reach (not counting stutters).
- **Steady pace:** a limit the game holds in its worst moments too.
- **With G-SYNC/FreeSync,** the limit can be any value, always below the display's maximum so it stays in range (for example, at most 225 FPS on a 240 Hz display).
- **Without VRR,** the limit has to divide the display's real refresh rate exactly (240, 120, 80, 60… on a 240 Hz display), so every image always lasts the same number of refreshes.
- **With frame generation** (DLSS Frame Generation and similar), the app separates real frames (the ones the game computes) from generated ones, and applies the formulas only to the real ones, from the stretches with the session's most common frame generation: menus, which many games show without it, are left out. The limit is set on screen: with ×2, 31 real are 62 on screen. Applying them to the images on screen wouldn't give the same result: the game hands over the real and the generated frame almost together (in Control Resonant, 0.4 ms apart) and, without a limit, they show up on screen very unevenly (some 5 ms after the previous one, others 22 ms). The formulas would measure that unevenness, not what the game sustains: in Control Resonant they would have given 45 on screen instead of 63. With frame generation, the adjustment rules don't change the limit on their own, because real frames change pace with it: the app only checks it.

**How it checks that it works.** In the next session, the limit is confirmed (*"Limit working"*) if the game runs at its pace within ±1.5 % and fewer than 10 % of frames arrive more than 7 % early. This check is done by measuring the game, so it works with any GPU.

**How it adjusts it afterwards.** When each session closes:
- **If the limit fails** (the 13 % rule), it lowers it to the pace the game did hold: with G-SYNC/FreeSync, the one it exceeded in 90 % of the session's seconds; without VRR, the next divisor of the Hz. With *Steady pace*, its formula is also applied to the whole session (1.13 × its slowest 1 %): if it doesn't reach the limit (with a 5 % margin), it lowers it to what the formula gives, at most one 13 % step per session, in case it was an unusual session.
- **If the game has headroom the whole session** (at least 3 minutes with the limit, no stretch below it and more than 98 % of frames waiting on the limiter), it raises it one step: after one such session with *More FPS*, or two in a row with *Steady pace*. If the raise doesn't hold, it goes back to the previous limit and doesn't try again for 30 days. To know that, it uses censored data (see below).
- **If with *More FPS* the session's slowest 1 % stays more than 13 % below the limit in two sessions in a row,** the game is unstable with its limit and moves to *Steady pace*. A clean session of at least 3 minutes resets the streak (a shorter one doesn't count), so a single session through a heavy area isn't enough: it has to repeat. For example, with a limit of 126 (threshold: 110), a session whose slowest 1 % is 103 counts as "1 of 2", and if the next one gives 114 over 12 minutes, the streak goes back to zero. It only acts with the *Automatic* priority (if you choose the game's priority by hand, the app recommends it on its row without changing it), and only in that direction, so it doesn't alternate between the two; *Undo* forgets it.
- **Closing the game doesn't count:** neither the last 15 seconds (exiting a game usually takes under 8) nor, before them, a final stretch far below the limit (under 70 %), which is a hang or a slow menu on the way out. A heavy area is somewhat below and does count, even if it lasts until the game closes.

With the *Automatic* priority, the app makes these changes on its own. If you choose the priority yourself, it doesn't change anything: it recommends them on each game's row, with a button to apply them. The details of each rule are in the [user guide](GUIDE.md) and the [technical documentation](TECHNICAL.md).

</details>

<details>
<summary><b>Censored data: how much the game could give with the limit in place</b></summary>

With a limit, most frames wait on the limiter, so they all seem to last the same: the limit's period. All you know about them is that the game would have taken *"at most"* that, not how long it really would. It's like trying to learn a car's top speed while it drives down a street limited to 50: if it's almost always at 50, you know it could go faster, but not by how much. In statistics this is called **censored data**, and the app treats it as such. First, it measures how much the limiter itself wobbles in each session, so it doesn't mistake that noise for a game that can't keep up. Then it uses the data in two ways:
- **To decide whether to raise the limit:** if more than 98 % of frames wait on the limiter, the game has headroom and could run faster. That's what a regular limiter can't know.
- **To estimate the game's capacity without the limit,** with a statistical model (Tobit) that combines the slow frames, which are measured exactly, with the waiting ones, of which only the maximum is known. It appears in each session's summary as *estimated capacity*. It's informational only: the estimated slowest 1 % is reliable (in Greylock, 74 FPS estimated versus 76 measured), and the median is only shown when few frames are waiting, because otherwise it would be an extrapolation.

</details>

<details>
<summary><b>Everything it does, in detail</b></summary>

- **Measures every game** while you play, frame by frame (with Intel's PresentMon), separating real frames from those generated by DLSS Frame Generation.
- **Decides the limit using the whole session** when you close the game, based on your priority, your display, and whether G-SYNC/FreeSync is active.
- **Applies it in the NVIDIA driver** (the game's profile, without touching the game), **checks that it works** in the next session, and keeps it. Along with the limit it turns on **"GPU at max"**, which stops the GPU from lowering its clocks with the limit in place and improves the 1 % low, and **low latency mode**, which keeps frames from piling up when the game can't reach the limit. With AMD and Intel, **it tells you what to set and where** and checks that it works, or applies it itself with **RivaTuner (RTSS)** if you choose it as the limiter, an experimental and not yet tested feature (see below).
- **Adjusts it on its own, or recommends it:** if the game stops holding it for a sustained period, it lowers it (isolated dips don't count); if it has headroom, it raises it step by step to what the game really holds. In automatic mode the app does it; if you choose the priority yourself, it recommends it with a button and you decide.
- **An automatic priority per game:** every game starts with *More FPS* and moves to *Steady pace* on its own if it turns out to be unstable with its limit. You can also pick one for all games or set it by hand for each one.
- **Fully reversible:** each game can be undone separately, and goes back exactly to how it was.
- **Diagnostics:** stutters, what limits the slow moments (CPU or GPU) with the matching advice, CPU performance, motion judder caused by frame generation, presentation mode, VRR state, an in-game overlay with the pacing graph, and optional per-frame recording to analyze sessions.
- **Why each limit:** in *Games*, unfolding a game shows the rule and the formula its limit was decided with, the curve of the deciding session (what share of frames reached each pace), the last session second by second against the 13 % threshold and each rule's result.
- **Lightweight:** it runs at low priority and in Windows 11 efficiency mode, so it doesn't compete with the game. The overlay is what uses the most, since it draws 60 times per second; to save more, turn it off.
- **In English and Spanish:** the interface and the log, in the language you choose.

</details>

<details>
<summary><b>Why automatic mode only works with NVIDIA</b></summary>

To apply a limit on its own, FlatPace needs the official kit each vendor provides for other programs to control its driver. Their licenses are very different:
- **NVIDIA (NVAPI)** is published under the **MIT license**, the same as FlatPace: it can be used and redistributed in an open-source project without restrictions.
- **AMD (ADLX)** forbids redistributing its files, and requires the program using it to be distributed under an agreement that forbids copying it and studying its code. That's incompatible with an open-source project like FlatPace.
- **Intel (IGCL)** allows redistributing its files, but only for use on Intel platforms, and includes a clause by which its user agrees to indemnify Intel. Those terms don't fit FlatPace's MIT license.

That's why, with AMD and Intel, FlatPace starts in ***Recommend only***: it measures the game, decides the limit, tells you exactly where to set it (in AMD Software or Intel Graphics Software, or in the game's own limiter) and checks that it works. If you have **RivaTuner Statistics Server (RTSS)**, a free and very widespread program (it comes with MSI Afterburner) whose limiter works with any GPU, you can choose it as the limiter on *Games* (**experimental:** it's implemented, but hasn't been tested with RTSS on an AMD or Intel GPU yet): the app will write the limit into each game's RTSS profile, without using any restrictively licensed kit. It never uses it unless you choose it, because RTSS hooks into games and you may use it for other things. It has to be running while you play. *This is our reading of the licenses, not legal advice.*

</details>

## Privacy

FlatPace works entirely offline. It keeps its data in `%LOCALAPPDATA%\FlatPace` and its logs next to the program.

## Building from source

You need Windows with Visual Studio 2022 (or CMake with MSVC), or MinGW-w64 on Linux. Dear ImGui is already included in `third_party/imgui`.

```
cmake -B build
cmake --build build --config Release
```

To run it, put `PresentMon-2.6.0-x64.exe` next to `FlatPace.exe` (download it from the [PresentMon releases](https://github.com/GameTechDev/PresentMon/releases)), or use the package in this repository's *Releases* section, which already includes it. Automated tests run with `sh tests/run_tests.sh` or `ctest`. More details in the [technical documentation](TECHNICAL.md#12-building).

## License

FlatPace (© 2026 KALAVER084) is released under the [MIT License](LICENSE): you may use, modify and redistribute it freely, keeping the copyright notice.

## Third-party credits and licenses

- [PresentMon](https://github.com/GameTechDev/PresentMon) (Intel, MIT License): frame measurement.
- [NVAPI](https://github.com/NVIDIA/nvapi) (NVIDIA, MIT License): driver profiles and G-SYNC status. Its declarations live in `third_party/nvapi`.
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT License): interface.
- [RivaTuner Statistics Server](https://www.guru3d.com/download/rtss-rivatuner-statistics-server-download/) (Unwinder, free): if installed, FlatPace uses its library to apply limits; no RTSS file is included.
