"""
Python bindings to the firmware's own C code (components/road_event and
components/neo7m_gps/nmea.c).

The shared library is compiled on first use with the system C compiler,
so the features used for training are computed by exactly the same code
that runs on the ESP32 - there is no Python re-implementation that could
drift out of sync.
"""

import ctypes
import os
import shutil
import subprocess

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SOURCES = [
    os.path.join(ROOT, "ml", "native", "host_shim.c"),
    os.path.join(ROOT, "components", "road_event", "road_event.c"),
    os.path.join(ROOT, "components", "neo7m_gps", "nmea.c"),
]

INCLUDES = [
    os.path.join(ROOT, "components", "road_event", "include"),
    os.path.join(ROOT, "components", "neo7m_gps", "include"),
]

LIB_DIR = os.path.join(ROOT, "ml", "_native")
LIB_PATH = os.path.join(LIB_DIR, "libroad_event.so")

ADXL345_LSB_G = 0.0039


def _build():
    compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")

    if compiler is None:
        raise RuntimeError("A C compiler (gcc/cc) is needed to build the feature library")

    os.makedirs(LIB_DIR, exist_ok=True)

    cmd = [compiler, "-O2", "-std=c99", "-shared", "-fPIC", "-ffp-contract=off"]
    cmd += [f"-I{d}" for d in INCLUDES]
    cmd += SOURCES + ["-o", LIB_PATH, "-lm"]

    subprocess.run(cmd, check=True)


def _load():
    stale = not os.path.exists(LIB_PATH) or any(
        os.path.getmtime(s) > os.path.getmtime(LIB_PATH) for s in SOURCES
    )

    if stale:
        _build()

    lib = ctypes.CDLL(LIB_PATH)

    f32p = ctypes.POINTER(ctypes.c_float)

    lib.re_const.restype = ctypes.c_int
    lib.re_feature_name.restype = ctypes.c_char_p
    lib.re_new.restype = ctypes.c_void_p
    lib.re_free.argtypes = [ctypes.c_void_p]
    lib.re_push.argtypes = [ctypes.c_void_p, ctypes.c_float, ctypes.c_float, ctypes.c_float]
    lib.re_push.restype = ctypes.c_int
    lib.re_window.argtypes = [ctypes.c_void_p, f32p, f32p]
    lib.re_window.restype = ctypes.c_uint
    lib.re_features.argtypes = [f32p, f32p, f32p]
    lib.re_severity.argtypes = [f32p, f32p]
    lib.re_severity.restype = ctypes.c_float
    lib.nmea_host_parse.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_double)]
    lib.nmea_host_parse.restype = ctypes.c_int

    return lib


_lib = _load()

FS_HZ = _lib.re_const(0)
PRE = _lib.re_const(1)
POST = _lib.re_const(2)
WIN = PRE + POST
NUM_FEATURES = _lib.re_const(3)
TRIGGER_G = _lib.re_const(4) / 1000.0
FEATURE_NAMES = [_lib.re_feature_name(i).decode() for i in range(NUM_FEATURES)]

IDLE, TRIGGERED, WINDOW_READY = 0, 1, 2


def _ptr(a):
    return a.ctypes.data_as(ctypes.POINTER(ctypes.c_float))


class Detector:
    """Streaming event detector - identical to the firmware's."""

    def __init__(self):
        self._d = _lib.re_new()

    def __del__(self):
        if getattr(self, "_d", None):
            _lib.re_free(self._d)
            self._d = None

    def push(self, x, y, z):
        return _lib.re_push(self._d, x, y, z)

    def window(self):
        xyz = np.zeros((WIN, 3), dtype=np.float32)
        g = np.zeros(3, dtype=np.float32)
        seq = _lib.re_window(self._d, _ptr(xyz), _ptr(g))
        return xyz, g, seq


def detect_events(samples):
    """
    Run the detector over an (N, 3) array of accelerations in g.

    Returns a list of dicts: trigger (sample index), window (WIN, 3), g (3,).
    """
    det = Detector()
    events = []

    for x, y, z in np.asarray(samples, dtype=np.float32):
        if det.push(x, y, z) == WINDOW_READY:
            xyz, g, seq = det.window()
            events.append({"trigger": int(seq), "window": xyz, "g": g})

    return events


def features(window, g):
    xyz = np.ascontiguousarray(window, dtype=np.float32)
    gv = np.ascontiguousarray(g, dtype=np.float32)
    out = np.zeros(NUM_FEATURES, dtype=np.float32)
    _lib.re_features(_ptr(xyz), _ptr(gv), _ptr(out))
    return out


def severity(window, g):
    xyz = np.ascontiguousarray(window, dtype=np.float32)
    gv = np.ascontiguousarray(g, dtype=np.float32)
    return float(_lib.re_severity(_ptr(xyz), _ptr(gv)))


def parse_nmea(line):
    """Parse with the firmware's NMEA parser. Returns (code, dict)."""
    out = (ctypes.c_double * 7)()
    code = _lib.nmea_host_parse(line.encode(), out)
    keys = ["lat", "lon", "speed_kmh", "hdop", "sats", "quality", "valid"]
    return code, dict(zip(keys, list(out)))
