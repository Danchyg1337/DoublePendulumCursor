"""Faithful copy of the pure-Python physics from run.py (no Windows / numpy /
PIL), used only to generate reference numbers to validate the C++ port.
Prints labeled values to stdout as `key,value` lines."""
import math

L1, L2 = 20.0, 40.0
M1, M2 = 1.0, 1.0
G = 2200.0
FRICTION = 0.35
SUBSTEPS = 6

SNAP_STIFFNESS = 900.0
SNAP_DAMPING = 2.0 * math.sqrt(SNAP_STIFFNESS)
WELL_RADIUS = 0.4
WELL_STIFFNESS = 122225.0
WELL_DAMPING = 2.0 * math.sqrt(WELL_STIFFNESS)
SETTLE_ANGLE_TOL = 0.05
SETTLE_VEL_TOL = 0.3
SNAP_GRACE_PERIOD = 0.5


def _accel(theta1, theta2, w1, w2, gx, gy):
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


def _angle_diff(target, current):
    return (target - current + math.pi) % (2 * math.pi) - math.pi


def _homing_accel(target, theta, w):
    d = _angle_diff(target, theta)
    if w > 0 and d < 0:
        d += 2.0 * math.pi
    elif w < 0 and d > 0:
        d -= 2.0 * math.pi
    return SNAP_STIFFNESS * d - SNAP_DAMPING * w


def _well_accel(target, theta, w):
    d = _angle_diff(target, theta)
    if abs(d) > WELL_RADIUS:
        return 0.0
    return WELL_STIFFNESS * d - WELL_DAMPING * w


def _settled(target, theta, w):
    return abs(_angle_diff(target, theta)) < SETTLE_ANGLE_TOL and abs(w) < SETTLE_VEL_TOL


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
    s0 = (theta1, theta2, w1, w2)
    k1 = _deriv(s0, gx, gy, extra1, extra2, override1, override2)
    k2 = _deriv(tuple(s0[i] + 0.5 * dt * k1[i] for i in range(4)), gx, gy, extra1, extra2, override1, override2)
    k3 = _deriv(tuple(s0[i] + 0.5 * dt * k2[i] for i in range(4)), gx, gy, extra1, extra2, override1, override2)
    k4 = _deriv(tuple(s0[i] + dt * k3[i] for i in range(4)), gx, gy, extra1, extra2, override1, override2)
    theta1, theta2, w1, w2 = (
        s0[i] + (dt / 6.0) * (k1[i] + 2 * k2[i] + 2 * k3[i] + k4[i]) for i in range(4)
    )
    if FRICTION > 0:
        damp = math.exp(-FRICTION * dt)
        w1 *= damp
        w2 *= damp
    return theta1, theta2, w1, w2


class SnapMode:
    def __init__(self, t1, t2):
        self.t1, self.t2 = t1, t2
        self.timer = 0.0
        self.was_active = False
        self.bob1_locked = False
        self.bob2_locked = False
        self.bob1_lock_time = None


def run_snap_mode(mode, theta1, theta2, w1, w2, frame_dt, sub_dt):
    if not mode.was_active:
        mode.timer = 0.0
        mode.bob1_locked = False
        mode.bob2_locked = False
        mode.bob1_lock_time = None
    else:
        mode.timer += frame_dt
    mode.was_active = True
    t1, t2 = mode.t1, mode.t2
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
            theta1, w1 = t1, 0.0
            bob2_timer = mode.timer - mode.bob1_lock_time
            assist = bob2_timer >= SNAP_GRACE_PERIOD
            if assist:
                ov2 = lambda th, w: _homing_accel(t2, th, w)
                theta1, theta2, w1, w2 = step(theta1, theta2, w1, w2, 0.0, G, sub_dt,
                                              override1=lambda th, w: 0.0, override2=ov2)
            else:
                well2 = lambda th, w: _well_accel(t2, th, w)
                theta1, theta2, w1, w2 = step(theta1, theta2, w1, w2, 0.0, G, sub_dt,
                                              override1=lambda th, w: 0.0, extra2=well2)
            if _settled(t2, theta2, w2):
                theta2, w2 = t2, 0.0
                mode.bob2_locked = True
        else:
            theta1, w1 = t1, 0.0
            theta2, w2 = t2, 0.0
    return theta1, theta2, w1, w2


def emit(key, *vals):
    print(key + "," + ",".join(repr(v) for v in vals))


# --- Scenario 1: single free-flight step ---
s = step(math.pi / 2, math.pi / 2, 0.0, 0.0, 150.0, 2000.0, 0.001)
emit("free_1step", *s)

# --- Scenario 2: one frame = 6 substeps ---
t1, t2, w1, w2 = math.pi / 2, math.pi / 2, 0.0, 0.0
for _ in range(6):
    t1, t2, w1, w2 = step(t1, t2, w1, w2, 150.0, 2000.0, 0.001)
emit("free_frame", t1, t2, w1, w2)

# --- Scenario 3: well modifier for 5 steps ---
t1, t2, w1, w2 = 0.1, math.pi, 0.0, 0.0
for _ in range(5):
    t1, t2, w1, w2 = step(t1, t2, w1, w2, 0.0, G, 0.001,
                          extra1=lambda th, w: _well_accel(0.0, th, w))
emit("well_step", t1, t2, w1, w2)

# --- Scenario 4: homing modifier for 5 steps ---
t1, t2, w1, w2 = 1.0, 0.5, 2.0, -1.0
for _ in range(5):
    t1, t2, w1, w2 = step(t1, t2, w1, w2, 0.0, G, 0.001,
                          override1=lambda th, w: _homing_accel(0.0, th, w))
emit("homing_step", t1, t2, w1, w2)

# --- Scenario 5: full snap 'text' mode over 400 frames ---
mode = SnapMode(0.0, math.pi)
t1, t2, w1, w2 = math.pi / 2, math.pi / 2, 0.0, 0.0
frame_dt = 1.0 / 144.0
sub_dt = frame_dt / 6.0
bob1_frame = -1
bob2_frame = -1
for f in range(400):
    t1, t2, w1, w2 = run_snap_mode(mode, t1, t2, w1, w2, frame_dt, sub_dt)
    if bob1_frame < 0 and mode.bob1_locked:
        bob1_frame = f
    if bob2_frame < 0 and mode.bob2_locked:
        bob2_frame = f
emit("snap_lockframes", bob1_frame, bob2_frame)
emit("snap_final", t1, t2, w1, w2)

# --- Scenario 6: longer free chaotic run, sanity/finite ---
t1, t2, w1, w2 = math.pi / 2, math.pi / 2, 0.0, 0.0
sub_dt = (1.0 / 240.0) / 6.0
for _ in range(240 * 6):
    t1, t2, w1, w2 = step(t1, t2, w1, w2, -300.0, 2100.0, sub_dt)
emit("free_long", t1, t2, w1, w2)

# --- Scenario 7: NW-SE diagonal snap mode (new) ---
mode = SnapMode(math.radians(-135), math.radians(45))
t1, t2, w1, w2 = math.pi / 2, math.pi / 2, 0.0, 0.0
frame_dt = 1.0 / 144.0
sub_dt = frame_dt / 6.0
b1 = b2 = -1
for f in range(400):
    t1, t2, w1, w2 = run_snap_mode(mode, t1, t2, w1, w2, frame_dt, sub_dt)
    if b1 < 0 and mode.bob1_locked:
        b1 = f
    if b2 < 0 and mode.bob2_locked:
        b2 = f
emit("nwse_lockframes", b1, b2)
emit("nwse_final", t1, t2, w1, w2)

# --- Expected results of the config-file override test in test_physics.cpp ---
emit("config_L1", 30.0)
emit("config_G", 1000.0)
print("config_pivot,10,20,30")
emit("config_nwse1", math.radians(-100))
