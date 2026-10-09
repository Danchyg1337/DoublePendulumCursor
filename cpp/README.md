# Flying Double-Pendulum Cursor (C++)

A C++ port of the original `run.py`. It replaces the Windows system cursor with
a live double-pendulum simulation whose pivot follows your real cursor. Moving
the mouse accelerates the pivot, which is physically equivalent to tilting
gravity, so flicking the mouse flings the pendulum around and holding still lets
it settle. Certain built-in cursor types fall into their own fixed "snap" poses
while shown.

The port exists to cut CPU load: the Python version rebuilt a PIL image and
allocated fresh GDI objects every single frame. This version does all per-frame
work in a handful of reused buffers.

## Beat dancer (from BPMidentifier)

The cursor also carries a C++ port of the BPMidentifier project's classic
detector + GIF dancer. It listens to the **default output device** (WASAPI
loopback, follows device changes) and, every 0.25 s, estimates the tempo
(spectral-flux onsets + autocorrelation with a tempo prior), the beat phase,
the octave (snare "backbeat rule": DnB heard at 87 becomes 174) and the music
**intensity** (0 calm .. 1 intense).

While a beat is identified **and** the intensity is at/above `SHOW_INTENSITY`
(0.70, hidden again below `HIDE_INTENSITY` 0.62), a `.gifbpm` actor from the
`actors/` folder is drawn at the **bottom-right of the cursor**, with its
marked beat frames landing on the music's beats. The GIF closest in tempo that
hasn't played yet comes next; repeatable GIFs loop for at least 3 s and
change at the end of a loop. Otherwise nothing extra is drawn -- no BPM
number, no intensity colour.

Keep the `actors/` folder (the `.gifbpm` files made with GIF BPMer) next to the
exe; add or remove files there to change the line-up. Settings live in
`pendulum.conf`:

```
DANCER_ENABLED = 1     # 0 = pendulum only, no audio capture
ACTORS_DIR = actors    # relative = next to the exe
GIF_SIZE = 160         # px box the GIF is scaled into
GIF_OFFSET_X = 24      # GIF top-left relative to the pointer tip
GIF_OFFSET_Y = 24
SHOW_INTENSITY = 0.70
HIDE_INTENSITY = 0.62
BEAT_OFFSET_MS = 30    # shift beats later to match audio output latency
FAST_DROP = 1          # 1 = appear ~0.5 s after a drop (0 = original, ~2 s)
MAX_BPM_DIFF = 50      # only GIFs within this many BPM of the music play (0 = any)
BEAT_LOCK = 1          # lock tempo + phase once found (vocals/fills can't drag it)
MUSIC_GATE = 1         # speech-only audio (videos, voice chat) never triggers a GIF
VOCAL_ROBUST = 1       # vocals don't lower the intensity
```

How the beat is followed (`BEAT_LOCK = 1`): once three readings agree, the
tempo and beat phase are **locked** and the beat simply runs on. Readings that
fit the locked pulse (same tempo, double, half, or 3:2) never change the speed;
the phase is corrected only by the median of recent beat times, so a vocal or
fill can't drag the GIF off the beat. A different tempo is adopted only after
it holds for 3 s; the octave (e.g. 87 vs 174) flips only when the other octave
dominates for ~8 s. Drum & bass read at 116 (the 2/3 "breakbeat" level) is
corrected to 174: at the wrong level the hats fall on thirds of the beat.

GIF choice: only GIFs within `MAX_BPM_DIFF` of the music's tempo are used,
closest first, taking turns. If none is close enough, none is shown.

Music check (`MUSIC_GATE = 1`): speech has frequent irregular pauses and no
steady beat; music is continuous, or its gaps repeat with the beat. Speech-only
audio gets no beat and no GIF.

### Tray menu

Right-click the tray icon (or double-click it for Actors):

- **Actors...** -- every GIF in the actors folder as an animated tile with its
  BPM. Click a tile to disable / enable it (kept in `actors/disabled.txt`).
  *Add GIFs...* copies `.gifbpm` files into the folder; *Open folder* opens it.
  The playing GIF is outlined; tiles show how far their tempo is from the music
  (or "too far" beyond `MAX_BPM_DIFF`).
- **Refresh** -- reloads `pendulum.conf`, `cursors.conf` and the actors, as if
  the app had been restarted.
- **Exit**

The tray icon also appears when the app starts at logon before the taskbar is
ready (it retries, and re-adds itself if Explorer restarts).

The GIF is part of the cursor image, so it never steals clicks.

### Debug overlay

Set `DEBUG = 1` in `pendulum.conf` to always see, above-right of the cursor:

```
BPM 128            detected tempo (-- = no beat yet)
I 0.72 SHOW        intensity, coloured green..yellow..red; SHOW = GIF allowed
S 0.65 F 0.81*     score of the 2 s window, of the 0.5 s window;
                   * = the 0.5 s (FAST_DROP) score drove the value this update
P 0.03 MUSIC       share of pauses in the last 3 s; VOICE = speech (no GIF)
```

`DEBUG_LOG = 1` also writes those values 4 times a second to
`debug_log.csv` next to the exe (overwritten at each start).

## Runs in the background (no console)

`pendulum_cursor.exe` starts silently with **no console window** and adds a
**system-tray icon**. To stop it and restore your normal cursor:

- **Right-click the tray icon -> Exit**, or
- press the global hotkey **Ctrl+Alt+P**.

Logging off or shutting down also restores the cursor automatically. If the
process is ever killed forcefully and the cursor stays stuck, restore it with:

```
rundll32.exe user32.dll,UpdatePerUserSystemParameters
```

No administrator rights are needed. To start it automatically at logon, use
the installer described below -- do NOT drop the exe and .conf files into the
Startup folder (see the note in "Autostart at logon").

## Autostart at logon (recommended)

Do **not** put the exe and the `.conf` files in the Startup folder. Two things
go wrong there: (1) the Startup folder launches *every* file in it, so Windows
also tries to "open" `pendulum.conf` and `cursors.conf` (in Notepad or a file
picker); and (2) apps started from the Startup folder are deliberately delayed
and throttled by Windows' startup-impact manager, so they feel slow to appear.

Instead, double-click **`install.bat`** (no admin needed). It copies the exe and
configs to `%LOCALAPPDATA%\PendulumCursor` and registers a per-user "at log on"
scheduled task that starts promptly, at normal priority, and never touches the
.conf files. **`uninstall.bat`** removes the task, stops the app, and restores
your cursor. If you already added the files to your Startup folder, remove them
from there (`Win+R` -> `shell:startup`) so nothing launches twice.

The app also calls `SetPriorityClass(NORMAL_PRIORITY_CLASS)` on itself, since a
scheduled task otherwise defaults to below-normal priority.

## Two config files

Both are read at startup and created next to the exe on first run. Pass custom
paths as the first (pendulum.conf) and second (cursors.conf) command-line args.

### `pendulum.conf` -- physics & appearance

Geometry (arm lengths, radii), masses, gravity `G`, friction, timing (substeps,
fps cap, forced refresh rate), the four bob/rod colours, and the snap-capture
behaviour. Any key you omit keeps its default.

```
L1 = 20
G  = 2200          # higher = snappier swing
FRICTION = 0.35
COLOR_PIVOT = 230,40,40
```

### `cursors.conf` -- which cursor type snaps to which pose

Each line maps a cursor to a pair of target angles, loaded into an array at
startup. **You can add cursor types here without recompiling.** The left side is
a friendly name *or* a raw numeric OCR id; `:` works instead of `=`, and
parentheses around the angles are ignored:

```
hand    = -30, 90     # 'pressable' triangle
text    =   0, 180    # I-beam: vertical line
hresize = -90, 90     # horizontal resize: horizontal line
nwse    = -135, 45    # diagonal resize "\"
help    : (60, -60)   # give the help cursor a pose
32650   = 20, -20     # any cursor by raw OCR id (here: app-starting)
```

Angles are degrees from straight-down, positive = clockwise. For a straight line
along a resize axis, set `theta2 = theta1 + 180`. Known names: `hand`
(`pressable`), `text` (`ibeam`), `vresize` (`sizens`), `hresize` (`sizewe`),
`nwse` (`sizenwse`), `nesw` (`sizenesw`), `wait`, `cross`, `up`, `sizeall`,
`no`, `appstarting`, `help`. Later entries for the same cursor win.

## Build

Requires Windows and either the Visual Studio Build Tools (`cl`) or MinGW-w64
(`g++`). Quickest:

```
build.bat
```

Or with CMake:

```
cmake -B build -S .
cmake --build build --config Release
```

Either way you get `pendulum_cursor.exe`, with `pendulum.conf` and
`cursors.conf` copied beside it by the CMake build.

## Windows Defender / SmartScreen flag

Defender may flag the exe as `PUA:Win32/Puwaders.*!ml`. The `!ml` suffix means
it's a **machine-learning heuristic**, not a signature match, and "Puwaders" is
Microsoft's family for cursor/desktop-changing PUAs. The program does exactly
the behaviour that family is defined by -- it calls `SetSystemCursor` to replace
every system cursor globally, runs windowless, and registers a global hotkey --
so this is a **false positive** triggered by legitimate functionality, not by
anything malicious.

What this project already does to reduce the false positive: the exe ships with
a proper **version-info resource** (company, product, description, version) and
an **application icon** (`resources/app.rc`). Unsigned, metadata-less,
default-icon binaries score much higher with the ML model, so adding these is
the most effective no-cost mitigation.

If it's still flagged, your options, best first:

1. **Code-sign the exe** with an Authenticode certificate. This is the only
   thing that reliably clears heuristic flags; it does cost money.
2. **Submit a false-positive report** to Microsoft at
   <https://www.microsoft.com/en-us/wdsi/filesubmission> (choose "I disagree,
   this is clean"). They usually clear ML detections within a day or two, and
   the fix propagates to all users via signature updates.
3. **Add a local exclusion** (Windows Security -> Virus & threat protection ->
   Manage settings -> Exclusions) for the exe on your own machine.

Building it yourself (e.g. via the GitHub Actions workflow) rather than
downloading a prebuilt exe also tends to avoid reputation-based flags.

## Build the .exe without a local compiler (GitHub Actions)

A workflow at `.github/workflows/build.yml` builds `pendulum_cursor.exe` on a
Windows runner and validates the physics against `test/reference.py` on every
push.

1. Push this project to a repo (workflow at the **repo root**, C++ under `cpp/`):

   ```
   cd DoublePendulum
   git init && git add . && git commit -m "Double-pendulum cursor"
   git branch -M main
   git remote add origin https://github.com/<you>/<repo>.git
   git push -u origin main
   ```

2. Open the repo's **Actions** tab; when the run is green, download
   `pendulum_cursor.exe` from the **Artifacts** section. Push a tag
   (`git tag v1.2 && git push --tags`) to also attach it to a Release.

Put the `pendulum.conf` and `cursors.conf` from this folder beside the exe, or
let it create them on first run.

## Layout

| File | Responsibility |
|------|----------------|
| `include/Config.h` | Runtime `Settings` struct (physics/appearance) + global `cfg::g`. |
| `include/ConfigFile.h`, `src/ConfigFile.cpp` | Parse `pendulum.conf`; holds the `cfg::g` definition. Portable. |
| `include/CursorPoses.h`, `src/CursorPoses.cpp` | Parse `cursors.conf` into an array; name/OCR-id table; defaults. Portable. |
| `include/Physics.h`, `src/Physics.cpp` | Double-pendulum RK4 dynamics, wells, homing springs. Portable. |
| `include/SnapMode.h`, `src/SnapMode.cpp` | Per-cursor snap-pose state machine. Portable. |
| `include/Renderer.h`, `src/Renderer.cpp` | Anti-aliased rasteriser into a reused BGRA buffer. Portable. |
| `include/CursorController.h`, `src/CursorController.cpp` | Win32: reused DIB + mask, cursor install, refresh detection. |
| `include/AppWindow.h`, `src/AppWindow.cpp` | Win32: hidden window, tray icon, quit hotkey. |
| `include/Resource.h`, `resources/app.rc`, `resources/app.ico` | Icon + version metadata. |
| `include/BeatDsp.h`, `src/BeatDsp.cpp` | Tempo estimator, intensity meter, snare/octave rule, stabiliser, beat clock (port of `bpm_detector.py` / `bpm_common.py`). Portable. |
| `include/BeatWorker.h`, `src/BeatWorker.cpp` | Audio ring buffer + the 4 Hz analysis loop (`ClassicWorker`). Portable. |
| `include/LoopbackCapture.h`, `src/LoopbackCapture.cpp` | Win32: WASAPI loopback of the default output device; analysis thread. |
| `include/GifDecoder.h`, `src/GifDecoder.cpp` | Self-contained animated GIF decoder + bilinear resize. Portable. |
| `include/Dancer.h`, `src/Dancer.cpp` | `.gifbpm` loader and the beat-synced playback engine (port of `gif_dancer.py`), disabled list. Portable. |
| `include/ActorsWindow.h`, `src/ActorsWindow.cpp` | Win32: the tray's Actors window (own thread) + its mailbox to the main loop. |
| `actors/` | The `.gifbpm` dancers. |
| `src/main.cpp` | Loads both config files, frame pacing, message-pumped main loop. |
| `install.bat`/`.ps1`, `uninstall.bat`/`.ps1` | Set up / remove the logon autostart task. |
| `pendulum.conf`, `cursors.conf` | Default, commented configuration. |
| `test/` | Portable validation harness + Python reference + numeric comparator. |

Physics, snap logic, both config parsers, the beat detector and the GIF
dancer carry no Windows headers, so they are compiled and validated on any
platform in CI (`test/test_beat.cpp` checks BPM, beat phase, GIF decoding and
on-beat playback with synthetic audio).

## What changed from the Python version

- **No per-frame allocations.** One DIB section and one mask are created at
  startup and reused; each frame is a `memcpy` + `CreateIconIndirect` +
  `SetSystemCursor`. Renderer buffers are sized once and reused.
- **Hand-written rasteriser** replaces PIL + NumPy, drawing directly in BGRA
  with light anti-aliasing.
- **No `std::function` in the hot loop.** Snap force modifiers are a small
  inline tagged struct, keeping RK4 tight.
- **Accurate frame pacing** via `timeBeginPeriod(1)`.
- **Background operation**, **two runtime config files**, **diagonal-resize
  poses**, and **file-driven cursor types** (add new ones without recompiling).

Physics constants and behaviour match `run.py` exactly at default settings; only
the implementation was optimised.
