#!/usr/bin/env python3
import argparse
import pandas as pd

def main():
    parser = argparse.ArgumentParser(
        description="Generate replay_data.h from neural_96.csv")
    parser.add_argument("csv_path", help="Path to neural_96.csv")
    parser.add_argument("output_path", help="Path to replay_data.h")
    parser.add_argument("--rows", type=int, default=256,
                        help="Number of rows to keep (default: 256)")
    args = parser.parse_args()

    df = pd.read_csv(args.csv_path)
    ch_cols = [f"ch{i}" for i in range(96)]

    missing = [c for c in ["sample", "t", *ch_cols] if c not in df.columns]
    if missing:
        raise SystemExit(f"Missing required columns: {missing}")

    subset = df.iloc[:args.rows].copy()

    ch_max = int(subset[ch_cols].max().max())
    ch_min = int(subset[ch_cols].min().min())
    if ch_min < 0 or ch_max > 255:
        raise SystemExit(
            f"Channel values must fit in uint8_t for this header. "
            f"Observed range: {ch_min}..{ch_max}")

    with open(args.output_path, "w", encoding="utf-8") as f:
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        f.write("#define REPLAY_CHANNELS    96u\n")
        f.write(f"#define REPLAY_FRAME_COUNT {len(subset)}u\n\n")
        f.write("typedef struct __attribute__((packed)) {\n")
        f.write("  uint32_t sample;\n")
        f.write("  float t;\n")
        f.write("  uint8_t channels[REPLAY_CHANNELS];\n")
        f.write("} replay_frame_t;\n\n")
        f.write("static const replay_frame_t g_replay_frames[REPLAY_FRAME_COUNT] = {\n")

        for _, row in subset.iterrows():
            channels = ", ".join(str(int(row[c])) for c in ch_cols)
            f.write("  {\n")
            f.write(f"    .sample = {int(row['sample'])}u,\n")
            f.write(f"    .t = {float(row['t']):.6f}f,\n")
            f.write(f"    .channels = {{{channels}}}\n")
            f.write("  },\n")

        f.write("};\n")

if __name__ == "__main__":
    main()
