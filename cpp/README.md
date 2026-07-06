# Flying Double-Pendulum Cursor (C++)

A C++ port of the original `run.py`. It replaces the Windows system cursor with
a live double-pendulum simulation whose pivot follows your real cursor. Moving
the mouse accelerates the pivot, which is physically equivalent to tilting
gravity, so flicking the mouse flings the pendulum around and holding still lets
it settle. Certain built-in cursor types (hand / text / resize) fall into their
own fixed "snap" poses while shown.

The port exists to cut CPU load: the Python version rebuilt a PIL image and
allocated fresh GDI objects (`CreateDIBSection`, `CreateBitmap`, matching
`DeleteObject`s) every single frame. This version does all per-frame work in a
handful of reused buffers.

## Build

Requires Windows and either the Visual Studio Build Tools (`cl`) or MinGW-w64
(`g++`).

Quickest:

```
build.bat
```

Or with CMake:

```
cmake -B build -S .
cmake --build build --config Release
```

Either way you get `pendulum_cursor.exe`.

## Run

Double-click the exe or run it from a console. Press **Ctrl+C** in the console
to stop; your normal cursor scheme is restored automatically. If the process is
killed forcefully and the cursor stays stuck, restore it with:

```
rundll32.exe user32.dll,UpdatePerUserSystemParameters
```

No administrator rights are needed.

## Layout

| File | Responsibility |
|------|----------------|
| `include/Config.h` | All tunable constants (geometry, forces, colours, timing). |
| `include/Physics.h`, `src/Physics.cpp` | Double-pendulum RK4 dynamics, gravity wells, homing springs. Platform independent. |
| `include/SnapMode.h`, `src/SnapMode.cpp` | Per-cursor-type snap pose state machine. Platform independent. |
| `include/Renderer.h`, `src/Renderer.cpp` | Anti-aliased rasteriser into a reused BGRA buffer. Platform independent. |
| `include/CursorController.h`, `src/CursorController.cpp` | The only Win32 piece: one reused DIB section + mask, cursor install, refresh detection. |
| `src/main.cpp` | Mouse sampling, frame pacing, Ctrl+C / close handling, main loop. |

Physics, snap logic and rendering carry no Windows headers, so they can be
compiled and unit tested on any platform (see `test/`).

## What changed from the Python version

- **No per-frame allocations.** One DIB section and one mask are created at
  startup and reused; each frame is a `memcpy` + `CreateIconIndirect` +
  `SetSystemCursor`. The pixel and accumulation buffers in the renderer are
  sized once and reused.
- **Hand-written rasteriser** replaces PIL + NumPy. It draws directly in BGRA
  (the DIB's native byte order), so there is no RGBA→BGRA conversion pass, and
  it adds light anti-aliasing the aliased PIL primitives lacked.
- **No `std::function` in the hot loop.** The snap force modifiers that were
  Python lambdas are a small tagged struct evaluated inline, keeping RK4 tight.
- **Accurate frame pacing** via `timeBeginPeriod(1)` so the loop actually hits
  the monitor refresh rate instead of being quantised to the ~15 ms timer tick.

Physics constants and behaviour match `run.py` exactly; only the implementation
was optimised.

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

2. Open the repo's **Actions** tab. The "Build Windows EXE" run appears
   automatically; when it's green, open it and download `pendulum_cursor.exe`
   from the **Artifacts** section at the bottom.

3. Optional -- to get a permanent download link, push a version tag and the
   workflow also attaches the exe to a GitHub Release:

   ```
   git tag v1.0
   git push --tags
   ```

You can also trigger a build by hand from the Actions tab (the workflow enables
`workflow_dispatch`). Downloaded artifacts are zipped by GitHub; unzip to get
the `.exe`.
