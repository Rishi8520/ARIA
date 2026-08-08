import struct, csv, sys

# Usage: python convert_capture.py <infile.bin> <outfile.csv> [duration_seconds]
#
# duration_seconds = wall-clock length of the capture (default 60).
# This is used to compute the per-sample interval for the timestamp column,
# since the firmware loop period (DRDY wait + fixed vTaskDelay) isn't a
# perfectly clean constant -- deriving it from known total duration / sample
# count is more accurate than hardcoding 200ms.

infile = sys.argv[1]
outfile = sys.argv[2]
duration_seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0

with open(infile, "rb") as f:
    data = f.read()

num_samples = len(data) // 8
samples = struct.unpack(f"<{num_samples}d", data)

# Trim trailing zeros (unwritten buffer tail).
# NOTE: this assumes your real signal never legitimately equals exactly 0.0.
# If it can, this will silently eat real samples -- worth double-checking
# against g_capture_total_count from the firmware instead of trusting this
# trim blindly.
trimmed = list(samples)
while trimmed and trimmed[-1] == 0.0:
    trimmed.pop()

n = len(trimmed)
if n < 2:
    print("Not enough samples after trimming to compute an interval.")
    sys.exit(1)

interval_ms = (duration_seconds * 1000.0) / n

with open(outfile, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(["timestamp", "voltage"])  # Edge Impulse expects a header
    for i, v in enumerate(trimmed):
        timestamp_ms = round(i * interval_ms, 3)
        writer.writerow([timestamp_ms, v])

print(f"Trimmed {num_samples - n} trailing zero samples, kept {n}")
print(f"Computed interval: {interval_ms:.2f} ms/sample (~{1000.0/interval_ms:.2f} Hz)")
print(f"Wrote {outfile} with timestamp + voltage columns")
