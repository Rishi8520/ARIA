import struct, csv, sys

# Usage: python convert_meta_log.py <infile.bin> <outfile.csv>
#
# g_meta_log is float[N][4]: confidence_avg, confidence_trend, signal_delta, latency_us
# Each row is 4 x 4 bytes = 16 bytes. No timestamp/interval needed here --
# unlike the raw ADC capture buffer, these rows are one-per-inference-event,
# not evenly spaced in time, so we just preserve row order.

infile = sys.argv[1]
outfile = sys.argv[2]

with open(infile, "rb") as f:
    data = f.read()

row_bytes = 16  # 4 floats
num_rows = len(data) // row_bytes
if len(data) % row_bytes != 0:
    print(f"Warning: file size {len(data)} not a multiple of {row_bytes} bytes, truncating trailing partial row")

rows = []
for i in range(num_rows):
    chunk = data[i * row_bytes:(i + 1) * row_bytes]
    conf_avg, conf_trend, signal_delta, latency_us = struct.unpack("<4f", chunk)
    # Skip all-zero rows (unwritten buffer tail, same idea as trimming trailing zeros)
    if conf_avg == 0.0 and conf_trend == 0.0 and signal_delta == 0.0 and latency_us == 0.0:
        continue
    rows.append((conf_avg, conf_trend, signal_delta, latency_us))

with open(outfile, "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(["confidence_avg", "confidence_trend", "signal_delta", "latency_us"])
    writer.writerows(rows)

print(f"Parsed {num_rows} raw rows, kept {len(rows)} non-zero rows")
print(f"Wrote {outfile}")