"""
Record a labelled driving session from esp32_collector over USB serial.

    python ml/collect.py --label pothole          # Ctrl+C to stop
    python ml/collect.py --label speed_hump --port /dev/ttyUSB0
    python ml/collect.py --label normal --minutes 10

Recording is continuous (no countdowns, nothing to time by hand). Every
sample is saved, so you can re-run detection with other settings later.

While recording, a passenger presses ENTER (or the ESP32's BOOT button) each time the car goes over
the thing you are collecting (pothole / speed hump). Those marks tell
ml/dataset.py which detected events are real; unmarked events in
pothole/hump sessions are ignored. For "normal" sessions marks are not
needed - every event on a normal road is a "normal" example.

Output (data/raw/<label>/<YYYYmmdd_HHMMSS>.*):
    .csv       seq,x,y,z           every accelerometer sample (g, 100 Hz)
    .marks     sample index of every ENTER / BOOT-button press
    .gps.csv   seq,fix,lat,lon,speed_kmh,sats,hdop   (1 Hz)
"""

import argparse
import datetime
import os
import sys
import threading
import time

import serial

LABELS = ["normal", "speed_hump", "pothole"]
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--label", required=True, choices=LABELS)
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--minutes", type=float, default=0, help="stop automatically (0 = until Ctrl+C)")
    ap.add_argument("--out", default=os.path.join(ROOT, "data", "raw"))
    return ap.parse_args()


def main():
    args = parse_args()

    folder = os.path.join(args.out, args.label)
    os.makedirs(folder, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    base = os.path.join(folder, stamp)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"ERROR: could not open {args.port}: {e}")
        print("Is the ESP32 plugged in, flashed with esp32_collector, and no other monitor open?")
        raise SystemExit(1)

    print(f"Connected to {args.port}. Waiting for the sensor stream...")

    # the ESP32 resets when the port opens; wait for its banner
    deadline = time.time() + 15
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", errors="ignore").strip()
        if line == "DATA_BEGIN" or line.startswith("D,"):
            break
        if line.startswith("W,"):
            print("ESP32:", line[2:])
    else:
        print("No data from the ESP32. Is esp32_collector flashed? (docs/02-firmware.md)")
        raise SystemExit(1)

    marks = []
    last_seq = [0]
    stop = threading.Event()

    def add_mark(seq, source):
        marks.append(seq)
        print(f"  >>> MARK #{len(marks)} ({source}) at sample {seq}", flush=True)

    def keyboard():
        while not stop.is_set():
            try:
                line = sys.stdin.readline()
            except (EOFError, ValueError):
                return
            if stop.is_set() or line == "":     # EOF: stdin not a terminal
                return
            add_mark(last_seq[0], "ENTER")

    threading.Thread(target=keyboard, daemon=True).start()

    print()
    print(f"RECORDING '{args.label}' -> {os.path.relpath(base, ROOT)}.csv")
    if args.label != "normal":
        print(f"Press ENTER (or the ESP32's BOOT button) every time you go over a "
              f"{args.label.replace('_', ' ')}.")
    print("Ctrl+C to stop.\n")

    n_samples = 0
    n_events = 0
    lost = 0
    expected = None
    t0 = time.time()
    gps_line = "no GPS data yet"

    with open(base + ".csv", "w") as fd, open(base + ".gps.csv", "w") as fg:
        fd.write("seq,x,y,z\n")
        fg.write("seq,fix,lat,lon,speed_kmh,sats,hdop\n")

        try:
            while True:
                if args.minutes and time.time() - t0 > args.minutes * 60:
                    break

                line = ser.readline().decode("utf-8", errors="ignore").strip()

                if line.startswith("D,"):
                    parts = line.split(",")
                    if len(parts) != 5:
                        continue
                    try:
                        seq = int(parts[1])
                        float(parts[2]), float(parts[3]), float(parts[4])
                    except ValueError:
                        continue
                    if expected is not None and seq != expected:
                        lost += max(seq - expected, 0)
                    expected = seq + 1
                    last_seq[0] = seq
                    fd.write(f"{seq},{parts[2]},{parts[3]},{parts[4]}\n")
                    n_samples += 1

                    if n_samples % 500 == 0:
                        print(f"  {n_samples / 100:7.0f} s  samples={n_samples}  events={n_events}  "
                              f"marks={len(marks)}  lost={lost}  | {gps_line}", flush=True)

                elif line.startswith("G,"):
                    p = line.split(",")
                    if len(p) == 7 and p[1] != "nodata":
                        fg.write(f"{last_seq[0]},{p[1]},{p[2]},{p[3]},{p[4]},{p[5]},{p[6]}\n")
                        gps_line = (f"GPS {float(p[2]):.5f},{float(p[3]):.5f} {p[4]} km/h sats={p[5]}"
                                    if p[1] == "1" else f"GPS searching (sats={p[5]})")
                    else:
                        gps_line = "GPS: no NMEA data (check wiring)"

                elif line.startswith("E,"):
                    p = line.split(",")
                    n_events += 1
                    print(f"  event #{n_events} at sample {p[1]}: device model says {p[2]} ({p[3]})", flush=True)

                elif line.startswith("M,"):
                    try:
                        add_mark(int(line.split(",")[1]), "BOOT button")
                    except (IndexError, ValueError):
                        pass

                elif line.startswith("W,"):
                    print("  ESP32 warning:", line[2:])

        except KeyboardInterrupt:
            pass

    stop.set()
    ser.close()

    with open(base + ".marks", "w") as fm:
        fm.write("".join(f"{m}\n" for m in marks))

    print()
    print(f"Saved {n_samples} samples ({n_samples / 100:.0f} s), {len(marks)} marks, {lost} lost samples")
    print(f"  {os.path.relpath(base, ROOT)}.csv / .marks / .gps.csv")
    if args.label != "normal" and not marks:
        print("  No marks: every detected event in this session will be labelled", args.label)
        print("  (use ml/review.py to check them).")
    print("\nNext: more sessions, then  python ml/train.py")


if __name__ == "__main__":
    main()
