"""
Synthetic road-session generator (for testing the pipeline end-to-end).

Physics: a linear quarter-car model per wheel (4 corners) driven over a
road profile = ISO 8608 random roughness + discrete features
(speed humps, potholes, expansion joints, manhole covers, rough patches).
Body heave, roll and pitch are combined into what a chassis-mounted
ADXL345 would measure, rotated by a random mounting angle, band-limited
to the sensor's 50 Hz bandwidth, quantised to 3.9 mg and sampled at 100 Hz.

The output has the SAME on-disk format as ml/collect.py, so every later
step (dataset, train) cannot tell the difference:

    data/<dir>/<label>/<session>.csv     seq,x,y,z      (g)
    data/<dir>/<label>/<session>.marks   one sample index per line

IMPORTANT: a model trained only on this data is a placeholder. Real
roads, cars and mountings differ - collect real data (docs/03-data-collection.md).

    python ml/simulate.py --out data/synthetic --sessions 12
"""

import argparse
import os

import numpy as np
from scipy import signal

G = 9.81
SIM_HZ = 1000
OUT_HZ = 100
LABELS = ["normal", "speed_hump", "pothole"]


# ============================================================
# ROAD PROFILES
# ============================================================

def iso8608_roughness(length_m, dx, gd_n0, rng):
    """Random road profile with PSD Gd(n) = Gd(n0) (n / 0.1)^-2."""
    n_pts = int(length_m / dx)
    freqs = np.fft.rfftfreq(n_pts, dx)
    freqs[0] = freqs[1]
    psd = gd_n0 * (freqs / 0.1) ** -2.0
    psd[(freqs < 0.01) | (freqs > 10.0)] = 0.0
    amp = np.sqrt(psd * freqs[1] * 2.0)
    phase = rng.uniform(0, 2 * np.pi, len(freqs))
    z = np.fft.irfft(amp * np.exp(1j * phase) * n_pts / 2.0, n_pts)
    return z - z[0]


def bump(x, x0, length, height):
    """Cosine-shaped hump (or dip if height < 0) starting at x0."""
    u = (x - x0) / length
    z = np.zeros_like(x)
    inside = (u >= 0) & (u <= 1)
    z[inside] = height * 0.5 * (1 - np.cos(2 * np.pi * u[inside]))
    return z


def pothole(x, x0, length, depth, edge, rng, tyre_radius=0.3):
    """Steep-edged depression; a tyre cannot reach the bottom of a short hole."""
    reach = tyre_radius - np.sqrt(max(tyre_radius ** 2 - (length / 2) ** 2, 0.0))
    depth = min(depth, reach)
    z = np.zeros_like(x)
    u = x - x0
    inside = (u >= 0) & (u <= length)
    ramp = np.minimum(np.minimum(u, length - u) / edge, 1.0)
    z[inside] = -depth * ramp[inside]
    z[inside] += rng.normal(0, depth * 0.08, inside.sum())
    return z


def contact_patch(z, dx, patch=0.12):
    k = max(int(patch / dx), 1)
    return np.convolve(z, np.ones(k) / k, mode="same")


# ============================================================
# VEHICLE
# ============================================================

def random_vehicle(rng):
    return {
        "ms": rng.uniform(220, 420),          # sprung mass per corner (kg)
        "mu": rng.uniform(30, 50),            # unsprung mass (kg)
        "ks": rng.uniform(14e3, 32e3),        # spring (N/m)
        "cs": rng.uniform(900, 2600),         # damper (N s/m)
        "kt": rng.uniform(150e3, 260e3),      # tyre (N/m)
        "wheelbase": rng.uniform(2.2, 2.8),
        "track": rng.uniform(1.3, 1.6),
        "sensor_x": rng.uniform(0.3, 0.7),    # 0 = rear axle, 1 = front axle
        "sensor_h": rng.uniform(0.2, 0.6),    # height above roll/pitch axis
    }


def quarter_car(v):
    ms, mu, ks, cs, kt = v["ms"], v["mu"], v["ks"], v["cs"], v["kt"]
    a = np.array([
        [0, 1, 0, 0],
        [-ks / ms, -cs / ms, ks / ms, cs / ms],
        [0, 0, 0, 1],
        [ks / mu, cs / mu, -(ks + kt) / mu, -cs / mu],
    ])
    b = np.array([[0], [0], [0], [kt / mu]])
    c = np.array([a[1], [1, 0, 0, 0]])      # body accel, body displacement
    d = np.zeros((2, 1))
    return signal.cont2discrete((a, b, c, d), 1.0 / SIM_HZ, method="bilinear")


def corner_response(sys_d, zr):
    ad, bd, cd, dd, _ = sys_d
    _, y, _ = signal.dlsim((ad, bd, cd, dd, 1.0 / SIM_HZ), zr)
    return y[:, 0], y[:, 1]


def random_rotation(rng, max_tilt_deg):
    axis = rng.normal(size=3)
    axis /= np.linalg.norm(axis)
    ang = np.radians(rng.uniform(0, max_tilt_deg))
    k = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    tilt = np.eye(3) + np.sin(ang) * k + (1 - np.cos(ang)) * k @ k
    yaw = rng.uniform(0, 2 * np.pi)
    rz = np.array([[np.cos(yaw), -np.sin(yaw), 0], [np.sin(yaw), np.cos(yaw), 0], [0, 0, 1]])
    return tilt @ rz


# ============================================================
# SEGMENT = a few seconds of driving around one road feature
# ============================================================

def make_segment(kind, veh, sys_d, rng, rough):
    speed = rng.uniform(12, 45) / 3.6
    if kind == "speed_hump":
        speed = rng.uniform(8, 30) / 3.6    # people slow down for humps

    duration = rng.uniform(4.0, 6.0)
    dx = 0.005
    t = np.arange(int(duration * SIM_HZ)) / SIM_HZ
    s_front = speed * t + veh["wheelbase"]
    length = s_front[-1] + 1.0
    n_pts = int(length / dx)
    x = np.arange(n_pts) * dx

    base_l = iso8608_roughness(length, dx, rough, rng)
    base_r = 0.7 * base_l + 0.3 * iso8608_roughness(length, dx, rough, rng)
    taper = np.clip(x / 1.0, 0, 1)
    base_l *= taper
    base_r *= taper

    x0 = veh["wheelbase"] + speed * rng.uniform(1.0, 2.0)
    feat_l = np.zeros_like(x)
    feat_r = np.zeros_like(x)

    if kind == "speed_hump":
        hl = rng.choice([rng.uniform(0.3, 0.6), rng.uniform(1.5, 4.0)])
        h = rng.uniform(0.05, 0.11)
        feat_l = feat_r = bump(x, x0, hl, h)
    elif kind == "pothole":
        pl = rng.uniform(0.25, 1.2)
        depth = rng.uniform(0.03, 0.12)
        edge = rng.uniform(0.02, 0.08)
        hole = pothole(x, x0, pl, depth, edge, rng)
        side = rng.random()
        if side < 0.4:
            feat_l = hole
        elif side < 0.8:
            feat_r = hole
        else:
            feat_l = feat_r = hole
    elif kind == "joint":
        step = rng.uniform(0.008, 0.03) * rng.choice([-1, 1])
        w = rng.uniform(0.02, 0.06)
        feat_l = feat_r = bump(x, x0, w, step)
    elif kind == "manhole":
        feat = bump(x, x0, rng.uniform(0.5, 1.0), rng.uniform(0.01, 0.035) * rng.choice([-1, 1]))
        if rng.random() < 0.5:
            feat_l = feat
        else:
            feat_r = feat
    elif kind == "rough":
        mask = (x > x0) & (x < x0 + rng.uniform(3, 8))
        extra = iso8608_roughness(length, dx, rough * rng.uniform(10, 40), rng)
        feat_l = extra * mask
        feat_r = (0.6 * extra + 0.4 * np.roll(extra, 37)) * mask

    prof_l = contact_patch(base_l + feat_l, dx)
    prof_r = contact_patch(base_r + feat_r, dx)

    def road(prof, s):
        return np.interp(s, x, prof)

    s_rear = s_front - veh["wheelbase"]

    acc = {}
    disp = {}
    for name, prof, s in [("fl", prof_l, s_front), ("fr", prof_r, s_front),
                          ("rl", prof_l, s_rear), ("rr", prof_r, s_rear)]:
        acc[name], disp[name] = corner_response(sys_d, road(prof, s))

    wx = veh["sensor_x"]
    heave_acc = wx * (acc["fl"] + acc["fr"]) / 2 + (1 - wx) * (acc["rl"] + acc["rr"]) / 2

    roll = ((disp["fl"] + disp["rl"]) - (disp["fr"] + disp["rr"])) / 2 / veh["track"]
    pitch = ((disp["fl"] + disp["fr"]) - (disp["rl"] + disp["rr"])) / 2 / veh["wheelbase"]
    roll_acc = np.gradient(np.gradient(roll, 1 / SIM_HZ), 1 / SIM_HZ)
    pitch_acc = np.gradient(np.gradient(pitch, 1 / SIM_HZ), 1 / SIM_HZ)

    # tyre hitting a pothole's far edge also pushes the car backwards
    edge_kick = 0.0
    if kind == "pothole":
        tyre_front = (road(prof_l, s_front) + road(prof_r, s_front)) / 2
        slope = np.gradient(tyre_front, 1 / SIM_HZ) / speed
        edge_kick = -np.clip(slope, 0, None) * speed ** 2 * 0.02

    drive = np.cumsum(rng.normal(0, 0.0005, len(t)))
    drive = np.clip(drive - drive.mean(), -0.15, 0.15) * G

    ax = veh["sensor_h"] * pitch_acc + G * pitch + drive + edge_kick
    ay = veh["sensor_h"] * roll_acc - G * roll
    az = heave_acc

    body = np.stack([ax / G, ay / G, 1.0 + az / G], axis=1)

    # front-wheel hit time of the feature (what a passenger would "mark")
    t_event = (x0 - veh["wheelbase"]) / speed

    return body, t_event


def to_sensor(body, rot, rng):
    acc = body @ rot.T
    k = SIM_HZ // OUT_HZ
    n = len(acc) // k
    acc = acc[: n * k].reshape(n, k, 3).mean(axis=1)    # ~50 Hz band limit
    acc += rng.normal(0, 0.008, acc.shape)
    acc = np.round(acc / 0.0039) * 0.0039
    return np.clip(acc, -8.0, 8.0)


# ============================================================
# SESSION
# ============================================================

def make_session(label, n_segments, rng):
    veh = random_vehicle(rng)
    sys_d = quarter_car(veh)
    rot = random_rotation(rng, max_tilt_deg=35)
    rough = rng.choice([16e-6, 32e-6, 64e-6, 128e-6])

    chunks = []
    marks = []
    offset = 0

    for _ in range(n_segments):
        if label == "normal":
            kind = rng.choice(["joint", "manhole", "rough", "flat"], p=[0.35, 0.25, 0.25, 0.15])
        else:
            kind = label if rng.random() < 0.8 else rng.choice(["joint", "manhole", "flat"])

        body, t_event = make_segment(kind, veh, sys_d, rng, rough)
        acc = to_sensor(body, rot, rng)

        if kind == label or (label == "normal" and kind != "flat"):
            marks.append(offset + int(t_event * OUT_HZ))

        chunks.append(acc)
        offset += len(acc)

    return np.concatenate(chunks), marks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="data/synthetic")
    ap.add_argument("--sessions", type=int, default=12, help="sessions per class")
    ap.add_argument("--segments", type=int, default=40, help="road features per session")
    ap.add_argument("--seed", type=int, default=1)
    args = ap.parse_args()

    rng = np.random.default_rng(args.seed)

    for label in LABELS:
        folder = os.path.join(args.out, label)
        os.makedirs(folder, exist_ok=True)

        for i in range(args.sessions):
            acc, marks = make_session(label, args.segments, rng)
            name = f"sim_{label}_{i:02d}"

            with open(os.path.join(folder, name + ".csv"), "w") as f:
                f.write("seq,x,y,z\n")
                for k, (x, y, z) in enumerate(acc):
                    f.write(f"{k},{x:.4f},{y:.4f},{z:.4f}\n")

            with open(os.path.join(folder, name + ".marks"), "w") as f:
                f.write("\n".join(str(m) for m in marks) + "\n")

            print(f"{folder}/{name}.csv  {len(acc) / OUT_HZ:6.1f} s  {len(marks)} marks")


if __name__ == "__main__":
    main()
