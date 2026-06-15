#!/usr/bin/env python3
"""Plot SpMV benchmark results from spmv_results.csv (audikw_1)."""

import csv
import os
import sys
from collections import defaultdict

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("Install matplotlib and numpy: pip install matplotlib numpy")
    sys.exit(1)

CSV_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "spmv_results.csv")
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "figures")
os.makedirs(OUT_DIR, exist_ok=True)

def load_data(path):
    modes = defaultdict(lambda: {"time": [], "gflops": [], "local": [], "remote": []})
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            m = row["mode"]
            modes[m]["time"].append(float(row["time_s"]))
            modes[m]["gflops"].append(float(row["gflops"]))
            modes[m]["local"].append(int(row["local_steals"]))
            modes[m]["remote"].append(int(row["remote_steals"]))
    return modes

def plot_gflops(modes):
    """Bar chart: GFLOP/s per mode."""
    fig, ax = plt.subplots(figsize=(8, 5))
    mode_names = ["uniform", "topostatic", "topopmu", "openmp"]
    labels = ["Uniform", "TopoStatic", "Topo+PMU", "OpenMP\n(static,close)"]
    colors = ["#EF5350", "#2196F3", "#4CAF50", "#FF9800"]

    means = []
    stds = []
    for m in mode_names:
        if m in modes:
            means.append(np.mean(modes[m]["gflops"]))
            stds.append(np.std(modes[m]["gflops"]))
        else:
            means.append(0)
            stds.append(0)

    x = np.arange(len(mode_names))
    bars = ax.bar(x, means, yerr=stds, capsize=5, color=colors,
                  edgecolor="black", linewidth=0.5, width=0.6)

    for bar, mean in zip(bars, means):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.05,
                f"{mean:.2f}", ha="center", va="bottom", fontweight="bold", fontsize=11)

    ax.set_ylabel("GFLOP/s", fontsize=12)
    ax.set_title("SpMV Performance: audikw_1 (943K rows, 77M nnz)\n"
                 "2x Xeon E5-2690v3, 24 cores, 20 outer reps, 5 trials",
                 fontsize=13)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=11)
    ax.set_ylim(0, max(means) * 1.2)
    ax.axhline(y=means[3], color="#FF9800", linestyle="--", alpha=0.4, linewidth=1)

    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "spmv_gflops.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "spmv_gflops.pdf"))
    plt.close(fig)
    print("  -> spmv_gflops.png / .pdf")

def plot_steal_locality(modes):
    """Bar chart: local steal % per mode."""
    fig, ax = plt.subplots(figsize=(6, 5))
    mode_names = ["uniform", "topostatic", "topopmu"]
    labels = ["Uniform", "TopoStatic", "Topo+PMU"]
    colors = ["#EF5350", "#2196F3", "#4CAF50"]

    pcts = []
    for m in mode_names:
        l = sum(modes[m]["local"])
        r = sum(modes[m]["remote"])
        pcts.append(100 * l / (l + r) if (l + r) > 0 else 0)

    x = np.arange(len(mode_names))
    bars = ax.bar(x, pcts, color=colors, edgecolor="black", linewidth=0.5, width=0.5)

    for bar, pct in zip(bars, pcts):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 1,
                f"{pct:.0f}%", ha="center", va="bottom", fontweight="bold", fontsize=12)

    ax.set_ylabel("Same-Socket Steals (%)", fontsize=12)
    ax.set_title("SpMV Steal Locality (audikw_1)", fontsize=13)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=11)
    ax.set_ylim(0, 100)
    ax.axhline(y=50, color="gray", linestyle=":", linewidth=1, alpha=0.5)

    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "spmv_steal_locality.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "spmv_steal_locality.pdf"))
    plt.close(fig)
    print("  -> spmv_steal_locality.png / .pdf")

def plot_speedup(modes):
    """Bar chart: speedup over uniform."""
    fig, ax = plt.subplots(figsize=(7, 5))
    u_mean = np.mean(modes["uniform"]["time"])
    mode_names = ["topostatic", "topopmu", "openmp"]
    labels = ["TopoStatic", "Topo+PMU", "OpenMP\n(static,close)"]
    colors = ["#2196F3", "#4CAF50", "#FF9800"]

    speedups = [u_mean / np.mean(modes[m]["time"]) for m in mode_names]

    x = np.arange(len(mode_names))
    bars = ax.bar(x, speedups, color=colors, edgecolor="black", linewidth=0.5, width=0.5)

    for bar, s in zip(bars, speedups):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.005,
                f"{s:.2f}x", ha="center", va="bottom", fontweight="bold", fontsize=12)

    ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=1)
    ax.set_ylabel("Speedup over Uniform Stealing", fontsize=12)
    ax.set_title("SpMV Speedup: audikw_1 on NUMA System\n"
                 "TopoSteal beats OpenMP schedule(static) + OMP_PROC_BIND=close",
                 fontsize=12)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=11)
    ax.set_ylim(0.95, max(speedups) * 1.08)

    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "spmv_speedup.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "spmv_speedup.pdf"))
    plt.close(fig)
    print("  -> spmv_speedup.png / .pdf")

if __name__ == "__main__":
    print(f"Loading {CSV_PATH}")
    modes = load_data(CSV_PATH)
    print(f"Found modes: {list(modes.keys())}\n")
    print("Generating SpMV figures:")
    plot_gflops(modes)
    plot_steal_locality(modes)
    plot_speedup(modes)
    print(f"\nAll figures saved to {OUT_DIR}/")
