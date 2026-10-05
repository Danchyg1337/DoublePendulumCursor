"""
Flying Double-Pendulum Cursor for Windows
-------------------------------------------
Replaces your system cursor with a live double-pendulum simulation.
The pendulum's pivot follows your real cursor position. Moving the
mouse accelerates the pivot, which is physically equivalent to
tilting/changing gravity on the pendulum -- so flicking the mouse
flings it around, and holding still lets it settle and swing gently.

Requirements:
    pip install pillow numpy pywin32

Run:
    python double_pendulum_cursor.py

Stop:
    Press Ctrl+C in this console window. Your normal cursor scheme
    will be restored automatically.

Notes:
- OCR_NORMAL (the arrow) always runs the live, mouse-driven, chaotic
  physics.
- Other cursor types can be given their own live "snap" behavior instead --
  see SNAP_MODES. Two are defined by default:
    * OCR_HAND ("pressable"): bob1 and bob2 fall into a triangle formation
      with the pivot.
    * OCR_IBEAM ("text", e.g. hovering text in an IDE or browser): bob1
      (blue) falls in directly below the pivot (red); bob2 (green) falls
      in back on top of the pivot, above bob1.
  For each mode: bob1 keeps swinging under its own power, with a gentle
  local "hole" added around its target -- purely additive to the normal
  gravity physics, not a replacement. A fast pass-through barely notices
  it; a slow pass (which naturally happens near the turning points of any
  swing) gets trapped by the hole's own damping and settles there for
  real, no teleporting. Only if that hasn't happened within
  SNAP_GRACE_PERIOD does a strong forced spring take over and guarantee it
  arrives. Once bob1 settles, bob2 gets the identical treatment (hole
  first, forced spring as fallback) with its own grace period timed from
  when bob1 finished. Moving off that cursor type at any point immediately
  drops any pull and returns to normal free-swinging physics from wherever
  the bobs currently are -- no popping or resetting. Add more modes by
  appending another SnapMode(name, ocr_id, theta1_target, theta2_target)
  to SNAP_MODES.
- Only ONE cursor slot is actually re-rendered and installed per frame
  (whichever one Windows is currently showing), so this stays exactly as
  cheap as the plain single-cursor version no matter how many modes exist.
- No admin rights should be required for SetSystemCursor in a normal
  user session.
- If the script crashes or is killed forcefully (not via Ctrl+C), your
  cursor may stay stuck as the pendulum. Just log off/on, or run:
    rundll32.exe user32.dll,UpdatePerUserSystemParameters
  or reboot, to restore it. The try/finally below handles normal exits.
"""

import ctypes
from ctypes import wintypes
import atexit
import time
import math
import numpy as np
from PIL import Image, ImageDraw

# ---------------- Tunable parameters ----------------
PIVOT_RADIUS = 5         # the red dot -- this IS the true click point
BOB_RADIUS = 3           # the blue/green pendulum bobs
ROD_WIDTH = 2
L1, L2 = 20, 40          # pendulum arm lengths, in pixels
M1, M2 = 1.0, 1.0        # relative bob masses
G = 2200.0               # base "gravity" strength (px/s^2) -- higher = snappier swing
FRICTION = 0.35          # energy loss per second (1/s). ~0 = frictionless & fully chaotic,
                         # higher = settles faster like it's swinging through syrup.
                         # This is applied as a true exponential decay so it no longer
                         # depends on FPS or substep count (that was the "rope" bug).

# The pivot sits at the CENTER of the canvas so the pendulum has L1+L2 pixels
# of room to swing in every direction without ever leaving the bitmap.
_MARGIN = max(PIVOT_RADIUS, BOB_RADIUS) + 4
_REACH = L1 + L2 + _MARGIN
CANVAS = _REACH * 2              # cursor bitmap size (square)
HOTSPOT = (_REACH, _REACH)       # pixel in the bitmap that acts as the real pointer tip

SUBSTEPS = 6             # physics substeps per rendered frame (stability)
MONITOR_HZ = None        # None = auto-detect from Windows; or set an int (e.g. 144) to force it
FPS_CAP = 240            # safety ceiling regardless of detected/forced refresh rate
ACCEL_SMOOTHING = 0.25   # 0..1: higher = more responsive/jittery, lower = smoother/laggier
MAX_ACCEL = 9000.0       # clamp pivot acceleration so a mouse teleport doesn't send it flying

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32

OCR_NORMAL = 32512
SPI_SETCURSORS = 0x0057

# Standard system cursor IDs (same numeric values as the classic IDC_*
# constants). OCR_NORMAL always gets the live, physics-driven pendulum.
# Any other id can be given a custom STATIC pose instead (see
# render_static_triangle / STATIC_CURSORS below) -- static poses are
# rendered once at startup, so they add effectively zero ongoing CPU cost,
# unlike constantly re-compositing a live frame onto them would.
OCR_IBEAM = 32513         # text fields
OCR_WAIT = 32514          # busy hourglass/spinner
OCR_CROSS = 32515
OCR_UP = 32516
OCR_SIZENWSE = 32642
OCR_SIZENESW = 32643
OCR_SIZEWE = 32644
OCR_SIZENS = 32645
OCR_SIZEALL = 32646
OCR_NO = 32648
OCR_HAND = 32649          # links / clickable elements ("pressable")
OCR_APPSTARTING = 32650
OCR_HELP = 32651


def restore_cursor():
    """Reload the user's normal cursor scheme. Safe to call more than once."""
    user32.SystemParametersInfoW(SPI_SETCURSORS, 0, None, 0)


# atexit covers normal Python-level exits (Ctrl+C, uncaught exceptions,
# sys.exit). It does NOT cover the console window being closed with the X
# button or the process being killed -- for those, Windows sends a console
# control event instead, which we catch below with SetConsoleCtrlHandler.
atexit.register(restore_cursor)

CTRL_C_EVENT = 0
CTRL_BREAK_EVENT = 1
CTRL_CLOSE_EVENT = 2
CTRL_LOGOFF_EVENT = 5
CTRL_SHUTDOWN_EVENT = 6

_HANDLER_ROUTINE = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.DWORD)


@_HANDLER_ROUTINE
def _console_ctrl_handler(ctrl_type):
    restore_cursor()
    if ctrl_type in (CTRL_CLOSE_EVENT, CTRL_LOGOFF_EVENT, CTRL_SHUTDOWN_EVENT):
        # We've cleaned up; tell Windows this event is handled so it doesn't
        # also pop up a "this program didn't close properly" dialog.
        return True
    return False  # Ctrl+C / Ctrl+Break: let Python's normal handling continue too


# Keep a reference to the callback alive for the life of the process --
# if it gets garbage collected, Windows calling into it will crash the app.
_ctrl_handler_ref = _console_ctrl_handler
kernel32.SetConsoleCtrlHandler(_ctrl_handler_ref, True)


# ---------------- Win32 plumbing ----------------
class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class CURSORINFO(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("hCursor", ctypes.c_void_p),
        ("ptScreenPos", POINT),
    ]


class BITMAPV5HEADER(ctypes.Structure):
    _fields_ = [
        ("bV5Size", wintypes.DWORD),
        ("bV5Width", ctypes.c_long),
        ("bV5Height", ctypes.c_long),
        ("bV5Planes", wintypes.WORD),
        ("bV5BitCount", wintypes.WORD),
        ("bV5Compression", wintypes.DWORD),
        ("bV5SizeImage", wintypes.DWORD),
        ("bV5XPelsPerMeter", ctypes.c_long),
        ("bV5YPelsPerMeter", ctypes.c_long),
        ("bV5ClrUsed", wintypes.DWORD),
        ("bV5ClrImportant", wintypes.DWORD),
        ("bV5RedMask", wintypes.DWORD),
        ("bV5GreenMask", wintypes.DWORD),
        ("bV5BlueMask", wintypes.DWORD),
        ("bV5AlphaMask", wintypes.DWORD),
        ("bV5CSType", wintypes.DWORD),
        ("bV5Endpoints", ctypes.c_byte * 36),
        ("bV5GammaRed", wintypes.DWORD),
        ("bV5GammaGreen", wintypes.DWORD),
        ("bV5GammaBlue", wintypes.DWORD),
        ("bV5Intent", wintypes.DWORD),
        ("bV5ProfileData", wintypes.DWORD),
        ("bV5ProfileSize", wintypes.DWORD),
        ("bV5Reserved", wintypes.DWORD),
    ]


class ICONINFO(ctypes.Structure):
    _fields_ = [
        ("fIcon", wintypes.BOOL),
        ("xHotspot", wintypes.DWORD),
        ("yHotspot", wintypes.DWORD),
        ("hbmMask", wintypes.HBITMAP),
        ("hbmColor", wintypes.HBITMAP),
    ]


class DEVMODE(ctypes.Structure):
    _fields_ = [
        ("dmDeviceName", ctypes.c_wchar * 32),
        ("dmSpecVersion", wintypes.WORD),
        ("dmDriverVersion", wintypes.WORD),
        ("dmSize", wintypes.WORD),
        ("dmDriverExtra", wintypes.WORD),
        ("dmFields", wintypes.DWORD),
        ("dmPositionX", ctypes.c_long),
        ("dmPositionY", ctypes.c_long),
        ("dmDisplayOrientation", wintypes.DWORD),
        ("dmDisplayFixedOutput", wintypes.DWORD),
        ("dmColor", ctypes.c_short),
        ("dmDuplex", ctypes.c_short),
        ("dmYResolution", ctypes.c_short),
        ("dmTTOption", ctypes.c_short),
        ("dmCollate", ctypes.c_short),
        ("dmFormName", ctypes.c_wchar * 32),
        ("dmLogPixels", wintypes.WORD),
        ("dmBitsPerPel", wintypes.DWORD),
        ("dmPelsWidth", wintypes.DWORD),
        ("dmPelsHeight", wintypes.DWORD),
        ("dmDisplayFlags", wintypes.DWORD),
        ("dmDisplayFrequency", wintypes.DWORD),
        ("dmICMMethod", wintypes.DWORD),
        ("dmICMIntent", wintypes.DWORD),
        ("dmMediaType", wintypes.DWORD),
        ("dmDitherType", wintypes.DWORD),
        ("dmReserved1", wintypes.DWORD),
        ("dmReserved2", wintypes.DWORD),
        ("dmPanningWidth", wintypes.DWORD),
        ("dmPanningHeight", wintypes.DWORD),
    ]


ENUM_CURRENT_SETTINGS = -1


def detect_refresh_rate(default=60):
    """Ask Windows for the primary monitor's current refresh rate in Hz."""
    dm = DEVMODE()
    dm.dmSize = ctypes.sizeof(DEVMODE)
    ok = user32.EnumDisplaySettingsW(None, ENUM_CURRENT_SETTINGS, ctypes.byref(dm))
    hz = dm.dmDisplayFrequency
    if not ok or hz <= 1:  # 0/1 means "hardware default", i.e. unknown
        return default
    return hz


def pil_to_hcursor(img: Image.Image, hotspot):
    """Convert an RGBA PIL image into a live Win32 HCURSOR."""
    w, h = img.size
    bmi = BITMAPV5HEADER()
    bmi.bV5Size = ctypes.sizeof(BITMAPV5HEADER)
    bmi.bV5Width = w
    bmi.bV5Height = -h  # negative = top-down DIB
    bmi.bV5Planes = 1
    bmi.bV5BitCount = 32
    bmi.bV5Compression = 3  # BI_BITFIELDS
    bmi.bV5RedMask = 0x00FF0000
    bmi.bV5GreenMask = 0x0000FF00
    bmi.bV5BlueMask = 0x000000FF
    bmi.bV5AlphaMask = 0xFF000000

    hdc = user32.GetDC(None)
    ptr_bits = ctypes.c_void_p()
    hbm_color = gdi32.CreateDIBSection(
        hdc, ctypes.byref(bmi), 0, ctypes.byref(ptr_bits), None, 0
    )
    user32.ReleaseDC(None, hdc)

    # RGBA -> BGRA (what the DIB expects), vectorized with numpy for speed
    arr = np.asarray(img)
    bgra = np.empty_like(arr)
    bgra[..., 0] = arr[..., 2]
    bgra[..., 1] = arr[..., 1]
    bgra[..., 2] = arr[..., 0]
    bgra[..., 3] = arr[..., 3]
    data = bgra.tobytes()
    ctypes.memmove(ptr_bits, data, len(data))

    hbm_mask = gdi32.CreateBitmap(w, h, 1, 1, None)

    ii = ICONINFO()
    ii.fIcon = False
    ii.xHotspot = hotspot[0]
    ii.yHotspot = hotspot[1]
    ii.hbmMask = hbm_mask
    ii.hbmColor = hbm_color

    hcursor = user32.CreateIconIndirect(ctypes.byref(ii))

    gdi32.DeleteObject(hbm_mask)
    gdi32.DeleteObject(hbm_color)
    return hcursor


def get_cursor_pos():
    pt = POINT()
    user32.GetCursorPos(ctypes.byref(pt))
    return pt.x, pt.y


# Must set these explicitly -- ctypes defaults to 32-bit return values,
# which would silently truncate 64-bit cursor handles and break the
# identity comparisons in active_snap_mode() below.
user32.LoadCursorW.restype = ctypes.c_void_p
user32.LoadCursorW.argtypes = [wintypes.HINSTANCE, ctypes.c_void_p]
user32.GetCursorInfo.restype = wintypes.BOOL
user32.GetCursorInfo.argtypes = [ctypes.POINTER(CURSORINFO)]


def _get_active_cursor_handle():
    ci = CURSORINFO()
    ci.cbSize = ctypes.sizeof(CURSORINFO)
    if not user32.GetCursorInfo(ctypes.byref(ci)):
        return None
    return ci.hCursor


class SnapMode:
    """
    A named custom cursor 'pose' triggered whenever Windows wants to show a
    particular built-in cursor type (identified by its OCR_* id). A cursor
    slot's HANDLE is a stable identifier -- SetSystemCursor only ever swaps
    its *bitmap contents*, never the handle -- so we grab it once at
    startup, then every frame just check whether the currently active
    cursor (whatever the foreground app most recently requested via
    SetCursor) *is* that slot, regardless of what image currently lives
    there. Each mode tracks its own grace timer and its own per-joint
    "has this bob been captured yet" state, both reset every time a fresh
    session starts (i.e. the cursor becomes this type again after not
    being it).
    """
    def __init__(self, name, ocr_id, theta1_target, theta2_target):
        self.name = name
        self.ocr_id = ocr_id
        self.theta1_target = theta1_target
        self.theta2_target = theta2_target
        self.slot_handle = user32.LoadCursorW(None, ctypes.c_void_p(ocr_id))
        self.timer = 0.0
        self.was_active = False
        self.bob1_locked = False
        self.bob2_locked = False
        self.bob1_lock_time = None  # session-relative time bob1 got captured

    def is_active(self, current_handle):
        return current_handle == self.slot_handle


# ---------------- Snap targets for each cursor mode ----------------
# IMPORTANT: these are FIXED absolute angles, deliberately NOT solved as a
# function of L1/L2. An earlier version tried to solve for the "perfect"
# angle to force an exact shape, and that formula only stayed valid across
# a narrow range of arm-length ratios -- it literally degenerated into a
# straight line at L2 == 2*L1 (and stayed broken beyond that), because
# forcing a specific x/y match while solving via asin/acos silently
# produces an invalid or nonsensical angle outside a narrow ratio window.
# Fixed angles avoid that class of bug entirely: the resulting shape's
# *proportions* depend on L1/L2 (as they must -- the rods really are those
# lengths), but the shape itself never degenerates, for any lengths.

# HAND / "pressable": fixed 60-degree bend at bob1. This guarantees pivot,
# bob1, and bob2 are never collinear (a valid triangle for ANY L1, L2) --
# it comes out exactly equilateral when L1 == L2, and a real (if not
# equilateral) triangle otherwise.
HAND_THETA1 = math.radians(-30)
HAND_THETA2 = math.radians(90)

# IBEAM / "text": bob1 (blue) hangs straight down from the pivot (red),
# and bob2 (green) folds back up along that SAME vertical line. Because
# green is always exactly L2 away from blue, and the pivot is always
# exactly L1 away from blue, green can only land exactly ON the pivot when
# L1 == L2 (that's a hard geometric limit, not a bug -- two circles of
# different radii around the same point don't intersect at that point).
# With unequal arm lengths, green still stays perfectly aligned above blue
# and in line with red, just offset by |L1-L2| pixels instead of an exact
# overlap.
TEXT_THETA1 = 0.0
TEXT_THETA2 = math.pi

# SIZEWE / horizontal resize: bob1 (blue) on the left, bob2 (green) on the
# right of the pivot, both at the same height. theta2 = 90 degrees means
# the second rod contributes zero vertical offset (cos(90) == 0) no matter
# what theta1 is, so blue and green always land at the same height; -30
# degrees for theta1 is the exact mirror point (with L1 == L2, blue and
# green land at x = -10 and +10 respectively -- perfectly symmetric).
HRESIZE_THETA1 = math.radians(-90)
HRESIZE_THETA2 = math.radians(90)

SNAP_STIFFNESS = 900.0                       # forced-homing spring (grace-period fallback only)
SNAP_DAMPING = 2.0 * math.sqrt(SNAP_STIFFNESS)  # critical damping -- no overshoot wobble

# The "hole": a gentle local spring+damper that only exists within
# WELL_RADIUS of the target, added ON TOP of the normal gravity physics
# (not replacing it). This is deliberately much weaker than the forced
# homing spring above -- a bob swinging through fast barely notices it and
# sails through untouched, but a bob that happens to enter slowly (which
# naturally happens near the turning points of any swing) gets damped and
# trapped, exactly like a ball rolling past a shallow dip in the ground.
WELL_RADIUS = 0.4        # rad (~23 degrees) -- capture zone around the target
WELL_STIFFNESS = 122225.0
WELL_DAMPING = 2.0 * math.sqrt(WELL_STIFFNESS)

# Once truly settled (both near the target AND nearly stationary), we snap
# the last tiny remainder exactly onto the target. Since it's already this
# close and this slow, that correction is imperceptible -- unlike forcing
# position at the moment of a crossing, which is what caused the "teleport"
# look before.
SETTLE_ANGLE_TOL = 0.05  # rad (~3 degrees)
SETTLE_VEL_TOL = 0.3     # rad/s

SNAP_GRACE_PERIOD = 0.5  # seconds: how long a bob gets to fall in on its own
                         # before the forced homing spring takes over


def _angle_diff(target, current):
    """Shortest signed angular distance, wrapped to [-pi, pi]."""
    return (target - current + math.pi) % (2 * math.pi) - math.pi


def _homing_accel(target, theta, w):
    """Strong critically-damped spring that FORCES theta toward target.
    Used only as the grace-period fallback, replacing the natural physics
    entirely for that joint.

    The shortest path to the target can point the opposite way from
    whichever direction the bob currently happens to be moving -- if we
    always pulled via the shortest path, engaging this spring could
    slam the brakes on existing momentum and yank it into a sudden
    reversal. Instead, if the bob is already moving, we bias the target
    direction to match: e.g. if it's currently increasing (w > 0) but the
    short way around is decreasing, we approach via the long way around
    (target + 2*pi) instead, so the spring accelerates smoothly WITH the
    current motion rather than fighting it.
    """
    d = _angle_diff(target, theta)
    if w > 0 and d < 0:
        d += 2.0 * math.pi
    elif w < 0 and d > 0:
        d -= 2.0 * math.pi
    return SNAP_STIFFNESS * d - SNAP_DAMPING * w


def _well_accel(target, theta, w):
    """Gentle local attractor -- the 'hole'. Zero outside WELL_RADIUS, so
    it never interferes with normal swinging; only pulls/damps once a bob
    is already close. Added ON TOP of the natural gravity acceleration,
    never replacing it, so a fast pass-through isn't artificially stopped."""
    d = _angle_diff(target, theta)
    if abs(d) > WELL_RADIUS:
        return 0.0
    return WELL_STIFFNESS * d - WELL_DAMPING * w


def _settled(target, theta, w):
    return abs(_angle_diff(target, theta)) < SETTLE_ANGLE_TOL and abs(w) < SETTLE_VEL_TOL


# Every entry here gets: its own live snap/release behavior, its own grace
# timer, and is checked in order each frame. Add more by appending another
# SnapMode(name, ocr_id, theta1_target, theta2_target) -- no other code
# needs to change.
SNAP_MODES = [
    SnapMode("pressable", OCR_HAND, HAND_THETA1, HAND_THETA2),
    SnapMode("text", OCR_IBEAM, TEXT_THETA1, TEXT_THETA2),
    SnapMode("vresize", OCR_SIZENS, TEXT_THETA1, TEXT_THETA2),
    SnapMode("hresize", OCR_SIZEWE, HRESIZE_THETA1, HRESIZE_THETA2),
]


def run_snap_mode(mode, theta1, theta2, w1, w2, frame_dt, sub_dt):
    """
    Advance the simulation one frame while `mode` is the currently active
    cursor.

    bob1 swings under its OWN power the whole time, with a gentle local
    well (a literal hole to fall into) added around its target -- if the
    natural chaotic swing happens to bring it in slowly enough, friction
    and the well's own damping trap it there for real, no teleporting.
    Only if it hasn't settled within SNAP_GRACE_PERIOD (bob1's own timer,
    counted from when this mode session started) does the strong forced
    spring take over and guarantee it gets there. Once bob1 is settled and
    pinned, bob2 gets the exact same treatment (well first, forced spring
    as fallback), timed from when bob1 finished.
    """
    if not mode.was_active:
        mode.timer = 0.0
        mode.bob1_locked = False
        mode.bob2_locked = False
        mode.bob1_lock_time = None
    else:
        mode.timer += frame_dt
    mode.was_active = True

    t1, t2 = mode.theta1_target, mode.theta2_target

    for _ in range(SUBSTEPS):
        if not mode.bob1_locked:
            assist = mode.timer >= SNAP_GRACE_PERIOD
            if assist:
                ov1 = lambda th, w: _homing_accel(t1, th, w)
                theta1, theta2, w1, w2 = step(theta1, theta2, w1, w2, 0.0, G, sub_dt, override1=ov1)
            else:
                well1 = lambda th, w: _well_accel(t1, th, w)
                theta1, theta2, w1, w2 = step(theta1, theta2, w1, w2, 0.0, G, sub_dt, extra1=well1)

            if _settled(t1, theta1, w1):
                theta1, w1 = t1, 0.0
                mode.bob1_locked = True
                mode.bob1_lock_time = mode.timer

        elif not mode.bob2_locked:
            theta1, w1 = t1, 0.0  # keep bob1 pinned once captured
            bob2_timer = mode.timer - mode.bob1_lock_time
            assist = bob2_timer >= SNAP_GRACE_PERIOD
            if assist:
                ov2 = lambda th, w: _homing_accel(t2, th, w)
                theta1, theta2, w1, w2 = step(
                    theta1, theta2, w1, w2, 0.0, G, sub_dt,
                    override1=lambda th, w: 0.0, override2=ov2
                )
            else:
                well2 = lambda th, w: _well_accel(t2, th, w)
                theta1, theta2, w1, w2 = step(
                    theta1, theta2, w1, w2, 0.0, G, sub_dt,
                    override1=lambda th, w: 0.0, extra2=well2
                )

            if _settled(t2, theta2, w2):
                theta2, w2 = t2, 0.0
                mode.bob2_locked = True

        else:
            theta1, w1 = t1, 0.0
            theta2, w2 = t2, 0.0

    return theta1, theta2, w1, w2


# ---------------- Physics ----------------
def _accel(theta1, theta2, w1, w2, gx, gy):
    """
    Angular accelerations of a double pendulum whose pivot is accelerating.
    An accelerating pivot is equivalent (in the pivot's own reference frame)
    to a uniform "gravity" vector (gx, gy) acting on both bobs -- so we just
    plug the effective gravity into the standard double-pendulum equations.
    """
    dth = theta1 - theta2
    c, s = math.cos(dth), math.sin(dth)

    a11 = (M1 + M2) * L1
    a12 = M2 * L2 * c
    b1 = (-M2 * L2 * w2 * w2 * s
          - (M1 + M2) * (gy * math.sin(theta1) - gx * math.cos(theta1)))

    a21 = M2 * L1 * c
    a22 = M2 * L2
    b2 = (M2 * L1 * w1 * w1 * s
          - M2 * (gy * math.sin(theta2) - gx * math.cos(theta2)))

    det = a11 * a22 - a12 * a21
    if abs(det) < 1e-9:
        det = 1e-9
    w1_dot = (b1 * a22 - a12 * b2) / det
    w2_dot = (a11 * b2 - b1 * a21) / det
    return w1_dot, w2_dot


def _deriv(state, gx, gy, extra1=None, extra2=None, override1=None, override2=None):
    theta1, theta2, w1, w2 = state
    w1_dot, w2_dot = _accel(theta1, theta2, w1, w2, gx, gy)
    if extra1 is not None:
        w1_dot += extra1(theta1, w1)
    if extra2 is not None:
        w2_dot += extra2(theta2, w2)
    if override1 is not None:
        w1_dot = override1(theta1, w1)
    if override2 is not None:
        w2_dot = override2(theta2, w2)
    return (w1, w2, w1_dot, w2_dot)


def step(theta1, theta2, w1, w2, gx, gy, dt, extra1=None, extra2=None, override1=None, override2=None):
    """
    One RK4 step. RK4 conserves energy far better than simple Euler methods,
    which matters a lot here: any artificial energy loss (or gain) from the
    integrator masks the pendulum's real chaotic behavior and makes it look
    like it's swinging through molasses instead of flipping unpredictably.

    extra1/extra2, if given, ADD a custom function(theta, w) -> accel on top
    of the naturally-computed acceleration -- this is the "gravity well"
    mechanism: a gentle local pull that coexists with normal physics rather
    than replacing it, so a fast pass-through isn't artificially stopped.

    override1/override2, if given, REPLACE the naturally-computed angular
    acceleration for that joint entirely -- this is the strong forced-homing
    fallback used once the grace period expires. The natural equations
    still run for whichever joint isn't overridden, so it reacts believably
    to the other joint being pulled around.
    """
    s0 = (theta1, theta2, w1, w2)
    k1 = _deriv(s0, gx, gy, extra1, extra2, override1, override2)
    k2 = _deriv(tuple(s0[i] + 0.5 * dt * k1[i] for i in range(4)), gx, gy, extra1, extra2, override1, override2)
    k3 = _deriv(tuple(s0[i] + 0.5 * dt * k2[i] for i in range(4)), gx, gy, extra1, extra2, override1, override2)
    k4 = _deriv(tuple(s0[i] + dt * k3[i] for i in range(4)), gx, gy, extra1, extra2, override1, override2)

    theta1, theta2, w1, w2 = (
        s0[i] + (dt / 6.0) * (k1[i] + 2 * k2[i] + 2 * k3[i] + k4[i])
        for i in range(4)
    )

    # True exponential decay -- independent of dt/fps/substep count, unlike a
    # flat per-substep multiplier (which is what caused the "rope" behavior).
    if FRICTION > 0:
        damp = math.exp(-FRICTION * dt)
        w1 *= damp
        w2 *= damp

    return theta1, theta2, w1, w2


def render_frame(theta1, theta2):
    img = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    px, py = HOTSPOT

    x1 = px + L1 * math.sin(theta1)
    y1 = py + L1 * math.cos(theta1)
    x2 = x1 + L2 * math.sin(theta2)
    y2 = y1 + L2 * math.cos(theta2)

    d.line([(px, py), (x1, y1)], fill=(20, 20, 20, 255), width=ROD_WIDTH)
    d.line([(x1, y1), (x2, y2)], fill=(20, 20, 20, 255), width=ROD_WIDTH)

    d.ellipse([px - PIVOT_RADIUS, py - PIVOT_RADIUS, px + PIVOT_RADIUS, py + PIVOT_RADIUS],
              fill=(230, 40, 40, 255))  # true tip
    d.ellipse([x1 - BOB_RADIUS, y1 - BOB_RADIUS, x1 + BOB_RADIUS, y1 + BOB_RADIUS],
              fill=(30, 90, 220, 255))
    d.ellipse([x2 - BOB_RADIUS, y2 - BOB_RADIUS, x2 + BOB_RADIUS, y2 + BOB_RADIUS],
              fill=(30, 160, 90, 255))
    return img


def main():
    theta1, theta2 = math.pi / 2, math.pi / 2  # start hanging out to the side
    w1 = w2 = 0.0

    prev_pos = get_cursor_pos()
    prev_vel = (0.0, 0.0)
    smoothed_accel = (0.0, 0.0)

    target_fps = min(MONITOR_HZ or detect_refresh_rate(), FPS_CAP)
    frame_dt = 1.0 / target_fps
    sub_dt = frame_dt / SUBSTEPS

    # Prime every cursor slot we manage once, up front, so none of them
    # ever show Windows' un-touched default, even for a single frame.
    initial_img = render_frame(theta1, theta2)
    user32.SetSystemCursor(pil_to_hcursor(initial_img, HOTSPOT), OCR_NORMAL)
    for mode in SNAP_MODES:
        user32.SetSystemCursor(pil_to_hcursor(initial_img, HOTSPOT), mode.ocr_id)

    print(f"Flying double-pendulum cursor running at {target_fps} fps. Press Ctrl+C to stop.")
    try:
        while True:
            t0 = time.perf_counter()

            pos = get_cursor_pos()
            vel = ((pos[0] - prev_pos[0]) / frame_dt,
                   (pos[1] - prev_pos[1]) / frame_dt)
            raw_accel = ((vel[0] - prev_vel[0]) / frame_dt,
                         (vel[1] - prev_vel[1]) / frame_dt)

            ax = max(-MAX_ACCEL, min(MAX_ACCEL, raw_accel[0]))
            ay = max(-MAX_ACCEL, min(MAX_ACCEL, raw_accel[1]))
            sax = smoothed_accel[0] + ACCEL_SMOOTHING * (ax - smoothed_accel[0])
            say = smoothed_accel[1] + ACCEL_SMOOTHING * (ay - smoothed_accel[1])
            smoothed_accel = (sax, say)

            current_handle = _get_active_cursor_handle()
            active_mode = next((m for m in SNAP_MODES if m.is_active(current_handle)), None)

            if active_mode is not None:
                for m in SNAP_MODES:
                    if m is not active_mode:
                        m.was_active = False  # reset other modes' grace timers

                theta1, theta2, w1, w2 = run_snap_mode(
                    active_mode, theta1, theta2, w1, w2, frame_dt, sub_dt
                )
                pendulum_img = render_frame(theta1, theta2)
                user32.SetSystemCursor(pil_to_hcursor(pendulum_img, HOTSPOT), active_mode.ocr_id)

            else:
                for m in SNAP_MODES:
                    m.was_active = False

                # Free flight -- exactly the original mouse-driven physics.
                # Releasing from a snapped/mid-snap pose just means we stop
                # applying the homing force; theta/w carry over untouched,
                # so the release is smooth with no pop or discontinuity.
                gx = -sax
                gy = G - say
                for _ in range(SUBSTEPS):
                    theta1, theta2, w1, w2 = step(theta1, theta2, w1, w2, gx, gy, sub_dt)

                pendulum_img = render_frame(theta1, theta2)
                user32.SetSystemCursor(pil_to_hcursor(pendulum_img, HOTSPOT), OCR_NORMAL)
            # SetSystemCursor takes ownership of the hcursor -- never destroy it yourself.

            prev_pos, prev_vel = pos, vel

            elapsed = time.perf_counter() - t0
            time.sleep(max(0.0, frame_dt - elapsed))
    finally:
        restore_cursor()
        print("Cursor restored.")


if __name__ == "__main__":
    main()