#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Areeb Sherjil
# plot_latency_compare.py — N HdrHistogram per-bucket CSVs (value_us,count) as ONE
# figure of side-by-side panels sharing a common x-axis, so distributions are compared
# by eye without flipping between images. Same binning/stats as plot_latency_hist.py.
#
# Usage:
#   plot_latency_compare.py a.hist.csv "Label A" b.hist.csv "Label B" \
#       [--out f.png] [--suptitle "..."] [--xlabel "RTT (us)"] [--bins N] [--logy]
import argparse, math, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator


def fmt_count(n):
    """Sample count -> compact label, rounded UP: 15.6M->16M, 47.3B->48B."""
    if n >= 1e9:
        return f"{math.ceil(n / 1e9)}B"
    if n >= 1e6:
        return f"{math.ceil(n / 1e6)}M"
    if n >= 1e3:
        return f"{math.ceil(n / 1e3)}K"
    return str(int(n))


def nice_tick(span, target=10):
    if span <= 0:
        return 1.0
    raw = span / target
    mag = 10.0 ** math.floor(math.log10(raw))
    for m in (1, 2, 2.5, 5, 10):
        if m * mag >= raw:
            return m * mag
    return 10 * mag


def load(path):
    d = np.genfromtxt(path, delimiter=",", names=True)
    v = np.atleast_1d(d["value_us"]).astype(float)
    c = np.atleast_1d(d["count"]).astype(float)
    o = np.argsort(v)
    return v[o], c[o]


ap = argparse.ArgumentParser()
ap.add_argument("pairs", nargs="+", help="file.hist.csv 'Label' [file.hist.csv 'Label' ...]")
ap.add_argument("--out", default="latency_compare.png")
ap.add_argument("--suptitle", default=None)
ap.add_argument("--xlabel", default="RTT (us)")
ap.add_argument("--bins", type=int, default=220)
ap.add_argument("--logy", action="store_true")
a = ap.parse_args()

if len(a.pairs) % 2:
    ap.error("pairs must be <file> <label> ...")
files = a.pairs[0::2]
labels = a.pairs[1::2]

data = [load(f) for f in files]

# ONE shared x-range across every panel — that is the whole point: the eye compares
# positions, not just shapes. Colour per panel so the legend/labels stay distinct.
lo = min(v[0] for v, _ in data)
hi = max(v[-1] for v, _ in data)
xmajor = nice_tick(hi - lo)
colors = ["#2c7fb8", "#d95f02", "#1b9e77", "#7570b3"]

fig, axes = plt.subplots(1, len(data), figsize=(7.0 * len(data), 5.4), sharex=True)
axes = np.atleast_1d(axes)

for ax, (v, c), label, col in zip(axes, data, labels, colors):
    N = c.sum()
    cum = np.cumsum(c)

    def pct(p, v=v, cum=cum, N=N):
        return v[min(int(np.searchsorted(cum, p / 100.0 * N)), len(v) - 1)]

    mn, mx = v[0], v[-1]
    med, p99, p999, p9999, p99999 = pct(50), pct(99), pct(99.9), pct(99.99), pct(99.999)

    edges = np.linspace(lo, hi, a.bins + 1)
    idx = np.clip(np.searchsorted(edges, v, side="right") - 1, 0, a.bins - 1)
    hist = np.zeros(a.bins)
    np.add.at(hist, idx, c)
    centers = 0.5 * (edges[:-1] + edges[1:])

    ax.bar(centers, hist, width=edges[1] - edges[0], color=col, edgecolor="none")
    if a.logy:
        ax.set_yscale("log")
    ax.set_title(f"{label}   —   median {med:.3f} us", fontsize=12, color=col)
    ax.set_xlabel(a.xlabel, fontsize=11)
    ax.set_ylabel(f"Frequency ({fmt_count(N)} samples)" + (" — log" if a.logy else ""),
                  fontsize=11)
    ax.set_xlim(lo, hi)
    ax.xaxis.set_major_locator(MultipleLocator(xmajor))
    ax.xaxis.set_minor_locator(MultipleLocator(xmajor / 5.0))
    ax.tick_params(axis="x", labelsize=8)
    ax.grid(axis="x", which="major", alpha=0.25)
    ax.grid(axis="x", which="minor", alpha=0.10)

    box = "\n".join([
        f"Min      {mn:7.3f}",
        f"Median   {med:7.3f}",
        f"P99      {p99:7.3f}",
        f"P99.9    {p999:7.3f}",
        f"P99.99   {p9999:7.3f}",
        f"P99.999  {p99999:7.3f}",
        f"Max      {mx:7.3f}",
    ])
    ax.text(0.99, 0.975, box, transform=ax.transAxes, ha="right", va="top",
            fontsize=7, family="monospace",
            bbox=dict(boxstyle="round,pad=0.3", fc="white", ec="0.6", alpha=0.85))

    print(f"{label}: N={int(N):,} min={mn:.3f} med={med:.3f} p99.999={p99999:.3f} max={mx:.3f}")

if a.suptitle:
    fig.suptitle(a.suptitle, fontsize=14)
plt.tight_layout()
plt.savefig(a.out, dpi=150, bbox_inches="tight")
print("wrote", a.out)
