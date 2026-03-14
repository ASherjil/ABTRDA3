#!/usr/bin/env python3
"""
Plot latency histogram from ABTRDA3 test output.

Usage:
    python3 plot_latencies.py latencies_packet_mmap.txt
    python3 plot_latencies.py latencies_packet_mmap.txt --max-us 200   # clip X axis

Output PNG is saved in the same directory as the input file.
"""

import argparse
import os
import numpy as np

def main():
    parser = argparse.ArgumentParser(description="Plot ABTRDA3 latency histogram")
    parser.add_argument("file", help="Path to latencies.txt (one value per line, nanoseconds)")
    parser.add_argument("--max-us", type=float, default=None,
                        help="Clip X axis at this value (microseconds)")
    parser.add_argument("--bins", type=int, default=500, help="Number of histogram bins")
    args = parser.parse_args()

    # Non-interactive backend (no X11 needed on remote machines)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # Output PNG in same directory as input file
    input_dir = os.path.dirname(os.path.abspath(args.file))
    base_name = os.path.splitext(os.path.basename(args.file))[0]
    save_path = os.path.join(input_dir, f"{base_name}.png")

    # Load data
    print(f"Loading {args.file}...")
    raw_ns = np.loadtxt(args.file, dtype=np.uint64)
    lat_us = raw_ns / 1000.0
    n = len(lat_us)
    print(f"Loaded {n:,} samples")

    # Statistics
    p50  = np.percentile(lat_us, 50)
    p90  = np.percentile(lat_us, 90)
    p99  = np.percentile(lat_us, 99)
    p999 = np.percentile(lat_us, 99.9)
    p100 = np.max(lat_us)
    minimum = np.min(lat_us)
    above_p999 = int(np.sum(lat_us > p999))

    print(f"\n{'='*40}")
    print(f"  Samples:  {n:,}")
    print(f"  Min:      {minimum:.2f} us")
    print(f"  Median:   {p50:.2f} us")
    print(f"  P90:      {p90:.2f} us")
    print(f"  P99:      {p99:.2f} us")
    print(f"  P99.9:    {p999:.2f} us")
    print(f"  P100:     {p100:.2f} us")
    print(f"  > P99.9:  {above_p999:,} samples")
    print(f"{'='*40}\n")

    # Determine plot range
    max_x = args.max_us if args.max_us else min(p100, p99 * 3)
    plot_data = lat_us[lat_us <= max_x]

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 10), gridspec_kw={"height_ratios": [3, 1]})

    # --- Top: Main histogram ---
    ax1.hist(plot_data, bins=args.bins, color="#2196F3", edgecolor="none", alpha=0.85)
    ax1.set_xlabel("RTT Latency (us)", fontsize=12)
    ax1.set_ylabel("Count", fontsize=12)
    ax1.set_title(f"ABTRDA3 Latency Distribution  ({n:,} samples)", fontsize=14, fontweight="bold")

    # Percentile lines
    for val, label, color, ls in [
        (p50,  f"P50  {p50:.1f}us",  "#4CAF50", "-"),
        (p90,  f"P90  {p90:.1f}us",  "#FF9800", "-"),
        (p99,  f"P99  {p99:.1f}us",  "#F44336", "-"),
        (p999, f"P99.9 {p999:.1f}us", "#9C27B0", "--"),
    ]:
        if val <= max_x:
            ax1.axvline(val, color=color, linestyle=ls, linewidth=1.5, label=label)

    # Stats box
    stats_text = (
        f"Min:    {minimum:.2f} us\n"
        f"Median: {p50:.2f} us\n"
        f"P99:    {p99:.2f} us\n"
        f"P99.9:  {p999:.2f} us\n"
        f"P100:   {p100:.2f} us\n"
        f"> P99.9: {above_p999:,} samples"
    )

    ax1.text(0.98, 0.95, stats_text, transform=ax1.transAxes,
             fontsize=10, fontfamily="monospace", verticalalignment="top",
             horizontalalignment="right",
             bbox=dict(boxstyle="round,pad=0.5", facecolor="wheat", alpha=0.8))

    ax1.legend(loc="upper left", fontsize=10)
    ax1.grid(axis="y", alpha=0.3)

    # --- Bottom: Full range log-scale histogram (shows tail) ---
    ax2.hist(lat_us, bins=1000, color="#F44336", edgecolor="none", alpha=0.7)
    ax2.set_yscale("log")
    ax2.set_xlabel("RTT Latency (us)", fontsize=12)
    ax2.set_ylabel("Count (log)", fontsize=12)
    ax2.set_title("Full Range (log scale) — tail outliers visible", fontsize=11)
    ax2.axvline(p100, color="black", linestyle="--", linewidth=1, label=f"P100 {p100:.1f}us")
    ax2.axvline(p99, color="#F44336", linestyle="-", linewidth=1, label=f"P99 {p99:.1f}us")
    ax2.legend(loc="upper right", fontsize=9)
    ax2.grid(axis="both", alpha=0.3)

    plt.tight_layout()
    plt.savefig(save_path, dpi=150, bbox_inches="tight")
    print(f"Saved to {save_path}")

if __name__ == "__main__":
    main()
