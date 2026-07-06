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

No administrator rights are needed. To launch it automatically at login, put a
shortcut to the exe in your Startup folder (`Win+R` -> `shell:startup`).

## Configuration file

Every tunable lives in **`pendulum.conf`**, read at startup. On first run the app
writes a fully-commented default file next to the exe; edit it and relaunch. Any
key you omit keeps its built-in default. You can also pass a config path as the
first command-line argument.

The file covers geometry (arm lengths, radii), masses, gravity `G`, friction,
timing (substeps, fps cap, forced refresh rate), the four bob/rod colours, the
snap-capture behaviour, and every snap-target angle (in degrees). Example:

```
L1 = 20
L2 = 40
G  = 2200          # higher = snappier swing
FRICTION = 0.35
COLOR_PIVOT = 230,40,40
NWSE_THETA1 = -135  # diagonal-resize pose, degrees
```

## Snap cursor poses

| Cursor type | OCR id | Pose |
|-------------|--------|------|
| Hand / "pressable" | `OCR_HAND` | triangle |
| Text I-beam | `OCR_IBEAM` | vertical line (fold) |
| Vertical resize | `OCR_SIZENS` | vertical line |
| Horizontal resize | `OCR_SIZEWE` | horizontal line |
| Diagonal resize `\` | `OCR_SIZENWSE` | NW-SE line |
| Diagonal resize `/` | `OCR_SIZENESW` | NE-SW line |

The two diagonal poses use the same logic as the H/V ones: rod 1 points one way
and rod 2 the opposite way (180 deg apart), so the pivot and both bobs form a
straight line along the resize axis. NW-SE uses -135 deg / 45 deg; NE-SW uses
135 deg / -45 deg.

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

Either way you get `pendulum_cursor.exe` (plus a `pendulum.conf` copied beside
it by the CMake build).

## Build the .exe without a local compiler (GitHub Actions)

You don't need a C++ toolchain on your machine. A workflow at
`.github/workflows/build.yml` builds `pendulum_cursor.exe` on a Windows runner
and validates the physics against `test/reference.py` on every push.

1. Create a repo and push this project (the workflow lives at the **repo root**,
   with the C++ code under `cpp/`):

   ```
   cd DoublePendulum
   git init
   git add .
   git commit -m "Double-pendulum cursor (C++ port)"
   git branch -M main
   git remote add origin https://github.com/<you>/<repo>.git
   git push -u origin main
   ```

2. Open the repo's **Actions** tab. When the "Build Windows EXE" run is green,
   open it and download `pendulum_cursor.exe` from the **Artifacts** section.

3. Optional -- push a version tag (`git tag v1.0 && git push --tags`) and the
   workflow also attaches the exe to a GitHub Release.

Downloaded artifacts are zipped by GitHub; unzip to get the `.exe`. Drop the
`pendulum.conf` from this folder beside it, or let the app create one on first
run.

## Layout

| File | Responsibility |
|------|----------------|
| `include/Config.h` | Runtime `Settings` struct (all tunables) + global `cfg::g`. |
| `include/ConfigFile.h`, `src/ConfigFile.cpp` | Parse `pendulum.conf`; holds the `cfg::g` definition. Platform independent. |
| `include/Physics.h`, `src/Physics.cpp` | Double-pendulum RK4 dynamics, wells, homing springs. Platform independent. |
| `include/SnapMode.h`, `src/SnapMode.cpp` | Per-cursor-type snap pose state machine. Platform independent. |
| `include/Renderer.h`, `src/Renderer.cpp` | Anti-aliased rasteriser into a reused BGRA buffer. Platform independent. |
| `include/CursorController.h`, `src/CursorController.cpp` | Win32: one reused DIB section + mask, cursor install, refresh detection. |
| `include/AppWindow.h`, `src/AppWindow.cpp` | Win32: hidden window, tray icon, quit hotkey (background operation). |
| `src/main.cpp` | Config load, mouse sampling, frame pacing, message-pumped main loop. |
| `pendulum.conf` | Default, fully-commented configuration. |
| `test/` | Portable validation harness + Python reference + numeric comparator. |

Physics, snap logic, config parsing and rendering carry no Windows headers, so
they are compiled and validated on any platform in CI.

## What changed from the Python version

- **No per-frame allocations.** One DIB section and one mask are created at
  startup and reused; each frame is a `memcpy` + `CreateIconIndirect` +
  `SetSystemCursor`. Renderer buffers are sized once and reused.
- **Hand-written rasteriser** replaces PIL + NumPy, drawing directly in BGRA
  (the DIB's native byte order) with light anti-aliasing.
- **No `std::function` in the hot loop.** Snap force modifiers are a small
  inline tagged struct, keeping RK4 tight.
- **Accurate frame pacing** via `timeBeginPeriod(1)`.
- **Background operation**, a **runtime config file**, and two **diagonal-resize
  snap poses** added in this revision.

Physics constants and behaviour match `run.py` exactly at default settings; only
the implementation was optimised.
