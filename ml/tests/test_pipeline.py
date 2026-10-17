"""
Host tests for the shared C code and the training pipeline.

    python -m pytest ml/tests -q
"""

import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import road_event as re  # noqa: E402
import simulate  # noqa: E402


def nmea(body):
    cs = 0
    for c in body:
        cs ^= ord(c)
    return f"${body}*{cs:02X}"


# ------------------------------------------------------------ NMEA

def test_nmea_rmc_and_gga():
    code, fix = re.parse_nmea(nmea("GPRMC,101530.00,A,1258.2960,N,07909.5280,E,10.80,90.0,171026,,,A"))
    assert code == 1
    assert fix["valid"] == 1
    assert abs(fix["lat"] - (12 + 58.296 / 60)) < 1e-6
    assert abs(fix["lon"] - (79 + 9.528 / 60)) < 1e-6
    assert abs(fix["speed_kmh"] - 10.8 * 1.852) < 1e-3

    code, fix = re.parse_nmea(nmea("GPGGA,101530.00,1258.2960,N,07909.5280,E,1,08,1.10,912.0,M,-86.0,M,,"))
    assert code == 2
    assert fix["sats"] == 8 and abs(fix["hdop"] - 1.1) < 1e-6


def test_nmea_southern_western_and_no_fix():
    _, fix = re.parse_nmea(nmea("GNRMC,000000.00,A,3352.0000,S,15112.0000,W,0.0,0.0,010126,,,A"))
    assert fix["lat"] < 0 and fix["lon"] < 0

    _, fix = re.parse_nmea(nmea("GPRMC,000001.00,V,,,,,,,010126,,,N"))
    assert fix["valid"] == 0

    _, fix = re.parse_nmea(nmea("GPGGA,000002.00,,,,,0,00,99.99,,,,,,"))
    assert fix["valid"] == 0


def test_nmea_bad_checksum():
    line = nmea("GPRMC,101530.00,A,1258.2960,N,07909.5280,E,10.80,90.0,171026,,,A")
    broken = line[:-2] + ("00" if line[-2:] != "00" else "11")
    code, _ = re.parse_nmea(broken)
    assert code == -1


# ------------------------------------------------------------ detector

def flat(n, g=(0.1, 0.05, 0.87)):
    rng = np.random.default_rng(0)
    return np.tile(g, (n, 1)) + rng.normal(0, 0.005, (n, 3))


def test_quiet_road_never_triggers():
    assert re.detect_events(flat(3000)) == []


def test_single_dip_then_spike_is_captured_and_aligned():
    a = flat(1000)
    g = a[:200].mean(axis=0)
    u = g / np.linalg.norm(g)
    a[500:505] -= 0.5 * u      # drop into the hole
    a[505:510] += 1.2 * u      # hit the far edge

    events = re.detect_events(a)
    assert len(events) == 1

    ev = events[0]
    assert ev["trigger"] == 500
    assert ev["window"].shape == (re.WIN, 3)
    # trigger sample sits at index PRE of the window
    assert np.allclose(ev["window"][re.PRE], a[500], atol=1e-5)

    f = dict(zip(re.FEATURE_NAMES, re.features(ev["window"], ev["g"])))
    assert f["first_peak"] < 0            # dip comes first
    assert f["t_max_minus_t_min"] > 0
    assert f["v_max"] > 1.0 and f["v_min"] < -0.4


def test_features_independent_of_mounting_angle():
    rng = np.random.default_rng(3)
    a = flat(1000, g=(0, 0, 1.0))
    a[500:508, 2] += np.hanning(8) * 0.9
    rot = simulate.random_rotation(rng, 60)

    f1 = re.features(*_first(a))
    f2 = re.features(*_first(a @ rot.T))
    assert np.allclose(f1, f2, atol=0.02)


def _first(a):
    ev = re.detect_events(a)[0]
    return ev["window"], ev["g"]


def test_simulated_classes_have_expected_shape():
    rng = np.random.default_rng(7)
    sign = {}
    for label in ("pothole", "speed_hump"):
        acc, _ = simulate.make_session(label, 12, rng)
        fp = [re.features(e["window"], e["g"])[re.FEATURE_NAMES.index("first_peak")]
              for e in re.detect_events(acc)]
        sign[label] = np.median(fp)
    assert sign["pothole"] < 0 < sign["speed_hump"]
