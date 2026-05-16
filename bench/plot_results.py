#!/usr/bin/env python3
"""Plot TopoSteal benchmark results from bench_results.csv."""

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

CSV_PATH = os.path.join(os.path.dirname(__file__), "bench_results.csv")
OUT_DIR = os.path.join(os.path.dirname(__file__), "figures")
os.makedirs(OUT_DIR, exist_ok=True)

def load_data(path):
    configs = defaultdict(lambda: {"uniform": [], "toposteal": [], "speedup": [],
                                    "u_local": [], "u_remote": [],
                                    "t_local": [], "t_remote": []})
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            c = row["config"]
            configs[c]["uniform"].append(float(row["uniform_s"]))
            configs[c]["toposteal"].append(float(row["toposteal_s"]))
            configs[c]["speedup"].append(float(row["speedup"]))
            configs[c]["u_local"].append(int(row["u_local_steals"]))
            configs[c]["u_remote"].append(int(row["u_remote_steals"]))
            configs[c]["t_local"].append(int(row["t_local_steals"]))
            configs[c]["t_remote"].append(int(row["t_remote_steals"]))
    return configs

def plot_speedup_bars(configs):
    """Bar chart: mean speedup per config with error bars."""
    fig, ax = plt.subplots(figsize=(10, 5))
    names = list(configs.keys())
    labels = [n.replace("-", " ").title() for n in names]
    means = [np.mean(configs[n]["speedup"]) for n in names]
    stds = [np.std(configs[n]["speedup"]) for n in names]

    colors = ["#2196F3", "#4CAF50", "#FF9800", "#9C27B0", "#F44336"]
    bars = ax.bar(range(len(names)), means, yerr=stds, capsize=5,
                  color=colors[:len(names)], edgecolor="black", linewidth=0.5)

    ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=1, label="Baseline (1.0x)")
    ax.set_ylabel("Speedup (Uniform / TopoSteal)", fontsize=12)
    ax.set_title("TopoSteal Speedup Across Configurations\n"
                 "2x Intel Xeon E5-2690v3, NUMA dist 10/21, 10 trials each", fontsize=13)
    ax.set_xticks(range(len(names)))
    ax.set_xticklabels(labels, rotation=20, ha="right", fontsize=10)
    ax.set_ylim(0.95, 1.25)

    for bar, mean in zip(bars, means):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.005,
                f"{mean:.2f}x", ha="center", va="bottom", fontweight="bold", fontsize=11)

    ax.legend(fontsize=10)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "speedup_bars.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "speedup_bars.pdf"))
    plt.close(fig)
    print("  -> speedup_bars.png / .pdf")

def plot_steal_locality(configs):
    """Grouped bar chart: local steal % for uniform vs toposteal."""
    fig, ax = plt.subplots(figsize=(10, 5))
    names = list(configs.keys())
    labels = [n.replace("-", " ").title() for n in names]
    x = np.arange(len(names))
    width = 0.35

    u_pct = []
    t_pct = []
    for n in names:
        ul = sum(configs[n]["u_local"])
        ur = sum(configs[n]["u_remote"])
        tl = sum(configs[n]["t_local"])
        tr = sum(configs[n]["t_remote"])
        u_pct.append(100 * ul / (ul + ur) if (ul + ur) > 0 else 0)
        t_pct.append(100 * tl / (tl + tr) if (tl + tr) > 0 else 0)

    bars1 = ax.bar(x - width/2, u_pct, width, label="Uniform", color="#EF5350", edgecolor="black", linewidth=0.5)
    bars2 = ax.bar(x + width/2, t_pct, width, label="TopoSteal", color="#66BB6A", edgecolor="black", linewidth=0.5)

    ax.set_ylabel("Local (Same-Socket) Steals (%)", fontsize=12)
    ax.set_title("Steal Locality: Uniform vs TopoSteal\n"
                 "Higher = more NUMA-local steals = less remote DRAM traffic", fontsize=13)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=20, ha="right", fontsize=10)
    ax.set_ylim(0, 100)
    ax.axhline(y=50, color="gray", linestyle=":", linewidth=1, alpha=0.5)

    for bar, pct in zip(bars1, u_pct):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 1,
                f"{pct:.0f}%", ha="center", va="bottom", fontsize=9)
    for bar, pct in zip(bars2, t_pct):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 1,
                f"{pct:.0f}%", ha="center", va="bottom", fontsize=9)

    ax.legend(fontsize=11)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "steal_locality.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "steal_locality.pdf"))
    plt.close(fig)
    print("  -> steal_locality.png / .pdf")

def plot_trial_timeseries(configs):
    """Line plot: per-trial times for the best config."""
    best = "heavy-imbalance-med"
    if best not in configs:
        best = list(configs.keys())[0]

    d = configs[best]
    trials = range(1, len(d["uniform"]) + 1)

    fig, ax = plt.subplots(figsize=(8, 4.5))
    ax.plot(trials, d["uniform"], "o-", color="#EF5350", label="Uniform", linewidth=2, markersize=6)
    ax.plot(trials, d["toposteal"], "s-", color="#66BB6A", label="TopoSteal", linewidth=2, markersize=6)

    u_mean = np.mean(d["uniform"])
    t_mean = np.mean(d["toposteal"])
    ax.axhline(y=u_mean, color="#EF5350", linestyle="--", alpha=0.5)
    ax.axhline(y=t_mean, color="#66BB6A", linestyle="--", alpha=0.5)

    ax.fill_between(trials, d["toposteal"], d["uniform"], alpha=0.15, color="#4CAF50")
    ax.set_xlabel("Trial", fontsize=12)
    ax.set_ylabel("Execution Time (s)", fontsize=12)
    ax.set_title(f"Per-Trial Execution Times — {best.replace('-', ' ').title()}\n"
                 f"Mean speedup: {u_mean/t_mean:.2f}x", fontsize=13)
    ax.legend(fontsize=11)
    ax.set_xticks(list(trials))
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "trial_timeseries.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "trial_timeseries.pdf"))
    plt.close(fig)
    print("  -> trial_timeseries.png / .pdf")

def plot_execution_comparison(configs):
    """Side-by-side bar chart: mean execution time per config."""
    fig, ax = plt.subplots(figsize=(10, 5))
    names = list(configs.keys())
    labels = [n.replace("-", " ").title() for n in names]
    x = np.arange(len(names))
    width = 0.35

    u_means = [np.mean(configs[n]["uniform"]) for n in names]
    t_means = [np.mean(configs[n]["toposteal"]) for n in names]
    u_stds = [np.std(configs[n]["uniform"]) for n in names]
    t_stds = [np.std(configs[n]["toposteal"]) for n in names]

    ax.bar(x - width/2, u_means, width, yerr=u_stds, capsize=4,
           label="Uniform", color="#EF5350", edgecolor="black", linewidth=0.5)
    ax.bar(x + width/2, t_means, width, yerr=t_stds, capsize=4,
           label="TopoSteal", color="#66BB6A", edgecolor="black", linewidth=0.5)

    ax.set_ylabel("Mean Execution Time (s)", fontsize=12)
    ax.set_title("Execution Time: Uniform vs TopoSteal", fontsize=13)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=20, ha="right", fontsize=10)
    ax.legend(fontsize=11)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "execution_comparison.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "execution_comparison.pdf"))
    plt.close(fig)
    print("  -> execution_comparison.png / .pdf")

if __name__ == "__main__":
    print(f"Loading {CSV_PATH}")
    configs = load_data(CSV_PATH)
    print(f"Found {len(configs)} configurations\n")
    print("Generating figures:")
    plot_speedup_bars(configs)
    plot_steal_locality(configs)
    plot_trial_timeseries(configs)
    plot_execution_comparison(configs)
    print(f"\nAll figures saved to {OUT_DIR}/")
