# FlatPace 0.50 user guide

This guide describes the current version. Changes between versions are listed in the [changelog](CHANGELOG.md) (in Spanish). The app has an English interface: switch it with **ES | EN** at the bottom of the sidebar.

## 1. Installation

1. Unzip the folder anywhere. It contains:
   - `FlatPace.exe`: the program.
   - `PresentMon-2.6.0-x64.exe`: Intel's tool that measures frames (it must stay in the same folder).
   - `Deshacer cambios.bat`: removes every applied limit without opening the app.
2. Open `FlatPace.exe`. Windows will ask for administrator rights.
3. If "Windows protected your PC" appears, click *More info* → *Run anyway*.

Nothing needs to be installed and no Windows settings need to change. To uninstall, delete the folder and, if you like, `%LOCALAPPDATA%\FlatPace` (click "Undo all changes" first).

## 2. What happens when the app opens

1. **It measures your display's real refresh rate** for 5 seconds (for example 239.986 Hz even if Windows says 240). If the PC is busy and the measurement is noisy, it uses the last good one.
2. **It checks your system:** GPUs, display, NVIDIA driver, available refresh rates and whether G-SYNC is available.
3. **It waits for you to open a game.** It detects it on its own, even if it was already running, and starts measuring when you bring it to the front.

## 3. The window

The sidebar has four sections:

- **Now:** the current game and the status tag of its limit (*LOCKED* in blue; *UNCONFIRMED* or *NO LIMIT* in red); a card with the real FPS, four metrics (1 % low, consistency, stutters and pace swing) and the **pacing graph**; and two cards: *Limit* (applied, recommended and status, with *Apply now*, *Manual limit…* and *Undo*) and *Details* (VRR, presentation, the game's own limit, bottleneck and display). The graph stays visible when you switch to the app.
- **Games:** the priority, the games with a limit (each with *Set…* and *Undo* buttons), *Undo all changes* and *Choose game…*.
- **System:** GPU, display, driver and VRR status with the monitor's refresh range.
- **Log:** everything the app does and measures, with a button to open the log file and the **Record per-frame data** checkbox, which saves a CSV file per session in the `grabaciones` folder (*Open recordings* button), useful for analyzing the pace in detail. In games with Reflex, each session summary also includes **PC latency** (from when the game reads your input until the image is sent to the display), with and without the limit, so you can see whether the limit improves or worsens it.

At the bottom of the sidebar: **Apply limits** (when off, the app only measures), **In-game overlay** (the app remembers your choice) and the language (**ES | EN**).

## 4. A game's cycle

| Session | What FlatPace does | What you do |
|---|---|---|
| **First** | Measures the whole session. When you close the game, it decides and saves the limit: *"Limit for the next session of …"* | Play normally |
| **Second** | Checks that the game runs at the limit's pace: **"Limit working"**. The limit becomes **accepted** | Nothing |
| **Later** | Measures and keeps watch. When you close the game, if it didn't hold the limit for a sustained period, it lowers it; if it had **headroom** the whole session, it **raises** it by 13 % (without going over your display's maximum) | Nothing |

**Want it sooner?** On *Now*, the **Apply now** button saves the current recommendation. Restart the game so the driver applies it. When you close the game, the app refines it with the whole session.

**When is an accepted limit lowered?** Only if the game ran more than 13 % below the limit, in stretches of at least 5 seconds, for at least 10 % of the session and one minute in total. Shorter dips are treated as isolated events. The last minute before the game closes doesn't count: it's usually menus, exiting or a possible hang.

**When is it raised?** With G-SYNC/FreeSync and without frame generation, if the game had **headroom** the whole session: at least 3 minutes with the limit, no stretch below it (neither the median nor the 1 % low) and almost all frames waiting on the limiter. With *Smoothness* one such session is enough; with *Stability* it takes two in a row. The rules pick the limit cautiously, and many games perform better with a limit than without one, so the app climbs step by step to what the game really holds. If a raise doesn't hold (with *Stability*, even if only in the heavy areas), it goes back to the previous limit and won't try again for 30 days. If you'd rather decide yourself, choose **Suggest only** in *Games* → *Limit adjustment*: the app will tell you the game has headroom and show an **Apply suggestion** button, with the same protections. Lowering is automatic in both modes, because a limit the game can't hold always worsens the pace; for full control over a game, use *Set…*.

## 5. Priority: Smoothness or Stability

On *Games*:

- **Smoothness** (default, recommended): the limit the game reaches **90 % of the time**, accepting brief drops in the most demanding moments. More FPS; in testing, perceptible drops took about 1 % of the time.
- **Stability:** a limit the game holds even in its worst moments **without perceptible drops** (its slowest 1 % arrives at most 13 % late, a difference you can't notice). It gives the steadiest pace and the flattest line in the graph.

With G-SYNC/FreeSync, neither counts **stutters** (frames over 2.5 times the usual): no limit fixes them. Without VRR, the previous rules are kept for now.

**The priority is each game's starting point.** With *Smoothness*, if a game turns out to be unstable with its limit (its slowest frames sink in a sustained way in two sessions in a row), the app moves it to *Stability* and shows it on its *Games* row: *"· Stability (chosen by the app)"*. That way stable games make the most of your PC and unstable ones keep a steady pace. It only changes in that direction; *Undo* forgets it. If you'd rather decide yourself, each game's priority button on *Games* lets you choose **Automatic**, **Always Smoothness** or **Always Stability**; the app doesn't change the last two.

Limits already accepted don't change when you switch priority: undo the game to recalculate it with the new criterion.

## 6. Manual limit

If you prefer a specific value (for example 120 in a game the app would set higher, or 220 in one you know your PC can hold), you can set it yourself:

- **A game in the list:** the *Set…* button in its row, on *Games*.
- **The game that's running:** the *Manual limit…* button on *Now*.

Type the value and click *Apply*; then restart the game if it was running. The window warns you if the value doesn't fit: with G-SYNC, if it's above your display's recommended maximum; without G-SYNC, if it doesn't divide its refresh rate (and it suggests values that do). With frame generation, the value is what you see on screen.

The app **never changes** a manual limit: it only checks that it works (the overlay turns blue) and, when you close the game, tells you if the game didn't hold it. To go back to automatic mode, click *Undo*.

## 7. G-SYNC, FreeSync and displays without VRR

- **With NVIDIA, every limit comes with "GPU at max"** (*Prefer maximum performance* in the game's profile): with a limit, the driver would lower the GPU clocks until it barely keeps up, and it wouldn't make it in time in heavy scenes, worsening the 1 % low. The GPU still protects itself against temperature and power. It's undone together with the limit.
- **With VRR active**, the limit matches the exact pace the game holds (for example 116 or 122), always below your display's ceiling (225 on a 240 Hz display, 138 on a 144 Hz one). The app also turns on the driver's VSync, the recommended combination with G-SYNC.
- **The monitor's VRR range** (read from its EDID) is taken into account: if its VRR range ends below its refresh rate, the limit stays below the end of the range; and if a game runs below the bottom of the range on a monitor that can't double frames (no LFC), the app uses a divisor of the refresh rate instead. *System* shows the range and whether LFC is available.
- **Without VRR**, the limit divides the real refresh rate exactly (240, 120, 80, 60…), so every image stays on screen for the same time.
- **Everything is automatic:** nothing to turn on. If your display flickers with VRR (some OLED and VA panels do) and you prefer divisors at all times, there's an advanced setting described in the technical documentation.
- The app detects whether VRR **really acts** with each game: with NVIDIA by asking the driver; with other brands by analyzing when each image appears on screen.

## 8. Frame generation (DLSS Frame Generation)

- The app separates real frames from generated ones. The limit is shown as *"122 on screen (61 real)"*.
- Real frames never go below 30: below that, latency is poor.
- If frame generation turns on or off (many games do this in menus), the app measures again. If the game turns it on and off by itself frequently (a dynamic or automatic mode), the app warns once on *Games* and stops logging each change: setting that mode in the game gives a steadier pace.
- With frame generation, the graph shows the images **on screen** (section 9), and the app doesn't estimate the game's capacity or suggest raising the limit, because real frames change pace with frame generation.
- The first time, it learns what the driver's limiter acts on in your PC (what you see on screen or the real frames).

## 9. In-game overlay

In a corner of the game: FPS, 1 % low, a tag with the limit status and the pacing graph of the last 3 seconds.

- **Blue ("LOCKED 225 fps"):** the limit is confirmed; the game runs at its pace.
- In games with Reflex, the numbers row includes **PC latency** (*"latency 24 ms"*). With a locked limit and no frame generation, it compares it with the game's latency without a limit: *"latency 18 ms (before 22)"*. If the row doesn't fit, secondary data is dropped first.
- **Red ("UNCONFIRMED" or "NO LIMIT"):** the game has no limit yet, or has one that hasn't been checked yet.
- The graph shows pace **deviations**: while frame times stay within the tolerance (±13 % of the target, shown in the label: *"target 8.3 ms ±1.1"*), the line is straight; only what goes outside is drawn. The same rule applies with and without a limit, so the difference you see is real. **Stutters** appear in amber.
- **With frame generation**, the graph draws the images **on screen** (what you see), and the label says so (*"on screen 13.9 ms ±1.8"*). If the game turns frame generation on and off by itself, the app warns about it on *Games*.
- At the bottom left, the pace **swing** (±ms). The first session without a limit of each game is kept as a reference, and with the limit confirmed you'll see the comparison: *"swing ±0.1 ms · before ±0.4"*.
- With the limit confirmed, a **blue band** shows that tolerance around the target.
- With an active limit, the **1 % low** discounts the limiter's own delays (frames that come out slightly late even though the game had them ready): it shows what the game causes. Each session summary in *Log* shows both values. If the line sits above the target, the game isn't quite reaching the limit: a slightly lower limit gives the limiter room to flatten it.
- It refreshes up to 60 times per second and scrolls continuously, with a small delay that adapts on its own (PresentMon delivers data in batches; with frame generation the delay can reach about 2 seconds).

It's a separate, transparent window: it doesn't touch the game. Since 0.44 the GPU draws it, at almost no CPU cost; if it didn't work on your PC, the app falls back to the classic overlay on its own (and with `overlay=clasico` in `equipo.txt`, inside `%LOCALAPPDATA%\FlatPace`, the classic one is always used).

## 10. Warnings and undo

- **Amber warnings** (on *Games* and *Now*): the game isn't holding its limit, it runs clearly better than when the limit was decided (for example after lowering settings), frame generation is the opposite of when the limit was applied, or, with a **manual** limit, **it could hold a higher one** (*"It has plenty of headroom…"*): the app doesn't change manual limits, so it only suggests it. Automatic limits rise on their own (section 4).
- **The log** also shows the estimated capacity with the limit in place (*"sin límite ~X fps"*), the session's **phases** (changes of area or settings) and, in the summary, the **game's memory**: what it holds according to all your sessions.
- **Undo one game:** the *Undo* button in its row. It goes back exactly to how it was; if "Apply limits" is on, it's recalculated next session.
- **Undo everything:** *Undo all changes* (the app switches to measuring only) or double-click `Deshacer cambios.bat`.

## 11. Other features

- **Choose game…:** for windowed games that aren't detected on their own, type the `.exe` name.
- **Adjusting the display refresh rate (monitors without VRR):** if your monitor offers several refresh rates (for example 100, 120 and 144 Hz) and another one clearly allows more steady FPS, the app switches to it when that game opens and restores it when the game closes. It's a temporary change that doesn't modify your Windows settings.

## 12. Comparing limits with consistency

To find which limit gives the steadiest pace in a game, compare sessions of a few minutes in the same area:
1. Play with a limit and note, from the session summary in *Log*, the line *"consistencia: X % de cambios < 2 ms"* (the log is in Spanish).
2. On *Games*, click *Undo* for that game, change the criterion (for example the priority) and repeat.
3. The limit with the higher percentage, and fewer stutters, is the steadiest.

## 13. Frequently asked questions

**The app says "restart the game".** The driver applies limits when the game opens. Close it and open it again; the app checks it on its own. Sometimes changing a graphics option that restarts rendering (for example the DLSS mode) is enough.

**A game runs worse than before.** On *Games*, click *Undo* in its row and restart the game. Send the log if you'd like it investigated.

**Can an anti-cheat ban me?** FlatPace never injects into games: it only changes the game's profile in the NVIDIA driver, just like the NVIDIA Control Panel.

**Does it use a lot of resources?** It shouldn't be noticeable in your games. FlatPace and PresentMon run at low priority and in Windows 11 efficiency mode, so the game always comes first and, on CPUs with efficiency cores, FlatPace uses those. In testing on a laptop, the two together used about 10 % of one core, almost all of it on efficiency cores. Each session summary in the *Log* shows the real cost on your PC.

**Why isn't the limit equal to my FPS?** The app picks the pace the game holds in its demanding moments, not in the quiet ones, so the pace stays constant.

**Where are the logs?** Next to the program, as `registro-date-time.txt`, and in the *Log* section.
