import csv
import os
import time

import serial


# ============================================================
# CONFIGURATION
# ============================================================

PORT = "/dev/ttyUSB0"
BAUD = 115200

SAMPLE_RATE = 100

WINDOW_SAMPLES = 100

WINDOW_DURATION = WINDOW_SAMPLES / SAMPLE_RATE

WINDOWS_PER_CLASS = 20

OUTPUT_DIR = "data"


# ============================================================
# DATASET CLASSES
# ============================================================

CLASSES = [
    {
        "name": "normal",
        "instruction": (
            "NORMAL ROAD:\n"
            "Drive normally on an ordinary road.\n"
            "Do not intentionally create bumps or potholes.\n"
            "Let the sensor capture the natural road vibration."
        ),
    },

    {
        "name": "speed_hump",
        "instruction": (
            "SPEED HUMP:\n"
            "Drive over a speed hump normally.\n"
            "Try to maintain approximately the same speed "
            "for each recording."
        ),
    },

    {
        "name": "pothole",
        "instruction": (
            "POTHOLE:\n"
            "Drive through a pothole normally, or safely "
            "simulate the pothole motion.\n"
            "Try to maintain approximately the same speed "
            "for each recording."
        ),
    },
]


# ============================================================
# DIRECTORY
# ============================================================

os.makedirs(
    OUTPUT_DIR,
    exist_ok=True
)


# ============================================================
# SERIAL CONNECTION
# ============================================================

print()
print("========================================")
print(" Connecting to ESP32")
print("========================================")
print()

print(f"Port : {PORT}")
print(f"Baud : {BAUD}")
print()

try:
    ser = serial.Serial(
        PORT,
        BAUD,
        timeout=1
    )

except serial.SerialException as e:
    print()
    print("ERROR: Could not open serial port.")
    print()
    print(e)
    print()
    print("Check:")
    print("  1. ESP32 is connected")
    print("  2. Correct port is /dev/ttyUSB0")
    print("  3. No other program is using the port")
    print()
    raise SystemExit(1)


time.sleep(2)

ser.reset_input_buffer()

print("Connected.")
print()


# ============================================================
# WAIT FOR ESP32 STREAM
# ============================================================

print(
    "Waiting for ESP32 sensor stream..."
)

print(
    "Looking for DATA_BEGIN..."
)

while True:

    line = (
        ser.readline()
        .decode(
            "utf-8",
            errors="ignore"
        )
        .strip()
    )

    if line == "DATA_BEGIN":
        break


print()
print(
    "ESP32 sensor stream detected."
)
print()


# ============================================================
# READ ONE VALID SENSOR SAMPLE
# ============================================================

def read_sample():

    while True:

        line = (
            ser.readline()
            .decode(
                "utf-8",
                errors="ignore"
            )
            .strip()
        )

        if not line:
            continue

        parts = line.split(",")

        if len(parts) != 4:
            continue

        try:

            timestamp = int(parts[0])

            x = float(parts[1])
            y = float(parts[2])
            z = float(parts[3])

            return (
                timestamp,
                x,
                y,
                z
            )

        except ValueError:

            continue


# ============================================================
# COUNTDOWN
# ============================================================

def countdown():

    print()

    for value in (3, 2, 1):

        print(
            f"   {value}..."
        )

        time.sleep(1)

    print()

    print(
        ">>> RECORDING NOW <<<"
    )


# ============================================================
# COLLECT ONE WINDOW
# ============================================================

def collect_window():

    samples = []

    while len(samples) < WINDOW_SAMPLES:

        samples.append(
            read_sample()
        )

    return samples


# ============================================================
# SAVE CLASS CSV
# ============================================================

def save_class_csv(
    class_name,
    rows
):

    filename = os.path.join(
        OUTPUT_DIR,
        f"{class_name}.csv"
    )

    with open(
        filename,
        "w",
        newline=""
    ) as file:

        writer = csv.writer(file)

        writer.writerow([
            "class",
            "window",
            "sample",
            "timestamp",
            "x",
            "y",
            "z",
        ])

        writer.writerows(rows)

    print()
    print(
        f"Saved: {filename}"
    )

    print(
        f"Rows : {len(rows)}"
    )


# ============================================================
# MAIN COLLECTION
# ============================================================

try:

    print()
    print("########################################")
    print("#      POTHOLE ML DATA COLLECTION      #")
    print("########################################")
    print()

    print(
        f"Sample rate     : {SAMPLE_RATE} Hz"
    )

    print(
        f"Samples/window  : {WINDOW_SAMPLES}"
    )

    print(
        f"Window duration : "
        f"{WINDOW_DURATION:.1f} seconds"
    )

    print(
        f"Windows/class   : {WINDOWS_PER_CLASS}"
    )

    print()


    for class_info in CLASSES:

        class_name = class_info["name"]

        instruction = class_info["instruction"]


        # ----------------------------------------------------
        # CLASS HEADER
        # ----------------------------------------------------

        print()
        print()
        print(
            "########################################"
        )

        print(
            f"# CLASS: {class_name.upper()}"
        )

        print(
            "########################################"
        )

        print()

        print(
            instruction
        )

        print()

        print(
            f"Windows to collect : "
            f"{WINDOWS_PER_CLASS}"
        )

        print(
            f"Samples/window     : "
            f"{WINDOW_SAMPLES}"
        )

        print(
            f"Duration/window    : "
            f"{WINDOW_DURATION:.1f} s"
        )

        print()


        input(
            "Press ENTER when ready..."
        )


        class_rows = []


        # ----------------------------------------------------
        # WINDOWS
        # ----------------------------------------------------

        for window in range(
            WINDOWS_PER_CLASS
        ):

            print()
            print(
                "----------------------------------------"
            )

            print(
                f"{class_name.upper()} "
                f"WINDOW {window + 1}/"
                f"{WINDOWS_PER_CLASS}"
            )

            print(
                "----------------------------------------"
            )

            print()

            print(
                instruction
            )

            print()


            countdown()


            # ----------------------------------------------
            # Capture
            # ----------------------------------------------

            samples = collect_window()


            # ----------------------------------------------
            # Store
            # ----------------------------------------------

            for sample_index, sample in enumerate(
                samples
            ):

                timestamp, x, y, z = sample

                class_rows.append([
                    class_name,
                    window,
                    sample_index,
                    timestamp,
                    x,
                    y,
                    z,
                ])


            print()

            print(
                f"Captured {len(samples)} samples."
            )

            print(
                f"Progress: "
                f"{window + 1}/{WINDOWS_PER_CLASS}"
            )


            # Small gap between windows

            time.sleep(1)


        # ----------------------------------------------------
        # SAVE CLASS
        # ----------------------------------------------------

        save_class_csv(
            class_name,
            class_rows
        )


        print()
        print(
            f"Finished class: {class_name}"
        )


finally:

    ser.close()


# ============================================================
# COMPLETE
# ============================================================

print()
print()
print("========================================")
print("       DATA COLLECTION COMPLETE")
print("========================================")
print()

print(
    f"Classes         : {len(CLASSES)}"
)

print(
    f"Windows / class : {WINDOWS_PER_CLASS}"
)

print(
    f"Total windows   : "
    f"{len(CLASSES) * WINDOWS_PER_CLASS}"
)

print()

print("Files:")

for class_info in CLASSES:

    print(
        f"  data/{class_info['name']}.csv"
    )

print()

print(
    "Next step:"
)

print(
    "    python train.py"
)

print()
print("========================================")

