#!/usr/bin/env python3
"""Plot TopoSteal benchmark results from bench_results.csv (3-mode: uniform/static/pmu)."""

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
    configs = defaultdict(lambda: {"uniform": [], "topostatic": [], "topopmu": [],
                                    "static_speedup": [], "pmu_speedup": [],
                                    "u_local": [], "u_remote": [],
                                    "t_local": [], "t_remote": [],
                                    "p_local": [], "p_remote": []})
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            c = row["config"]
            configs[c]["uniform"].append(float(row["uniform_s"]))
            configs[c]["topostatic"].append(float(row["topostatic_s"]))
            configs[c]["topopmu"].append(float(row["topopmu_s"]))
            configs[c]["static_speedup"].append(float(row["static_speedup"]))
            configs[c]["pmu_speedup"].append(float(row["pmu_speedup"]))
            configs[c]["u_local"].append(int(row["u_local_steals"]))
            configs[c]["u_remote"].append(int(row["u_remote_steals"]))
            configs[c]["t_local"].append(int(row["t_local_steals"]))
            configs[c]["t_remote"].append(int(row["t_remote_steals"]))
            configs[c]["p_local"].append(int(row["p_local_steals"]))
            configs[c]["p_remote"].append(int(row["p_remote_steals"]))
    return configs

def plot_speedup_bars(configs):
    """Bar chart: mean speedup per config — static vs PMU."""
    fig, ax = plt.subplots(figsize=(10, 5))
    names = list(configs.keys())
    labels = [n.replace("-", " ").title() for n in names]
    x = np.arange(len(names))
    width = 0.35

    static_means = [np.mean(configs[n]["static_speedup"]) for n in names]
    static_stds = [np.std(configs[n]["static_speedup"]) for n in names]
    pmu_means = [np.mean(configs[n]["pmu_speedup"]) for n in names]
    pmu_stds = [np.std(configs[n]["pmu_speedup"]) for n in names]

    bars1 = ax.bar(x - width/2, static_means, width, yerr=static_stds, capsize=5,
                   label="TopoStatic (1/d²)", color="#2196F3", edgecolor="black", linewidth=0.5)
    bars2 = ax.bar(x + width/2, pmu_means, width, yerr=pmu_stds, capsize=5,
                   label="Topo+PMU (dynamic)", color="#4CAF50", edgecolor="black", linewidth=0.5)

    ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=1, label="Baseline (1.0x)")
    ax.set_ylabel("Speedup over Uniform Stealing", fontsize=12)
    ax.set_title("TopoSteal Speedup: Static vs Dynamic PMU Feedback\n"
                 "2x Intel Xeon E5-2690v3, NUMA dist 10/21, 10 trials each", fontsize=13)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=20, ha="right", fontsize=10)
    ax.set_ylim(0.95, 1.25)

    for bar, mean in zip(bars1, static_means):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.005,
                f"{mean:.2f}x", ha="center", va="bottom", fontweight="bold", fontsize=9)
    for bar, mean in zip(bars2, pmu_means):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.005,
                f"{mean:.2f}x", ha="center", va="bottom", fontweight="bold", fontsize=9)

    ax.legend(fontsize=10)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "speedup_bars.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "speedup_bars.pdf"))
    plt.close(fig)
    print("  -> speedup_bars.png / .pdf")

def plot_steal_locality(configs):
    """Grouped bar chart: local steal % for uniform vs topostatic vs topo+pmu."""
    fig, ax = plt.subplots(figsize=(10, 5))
    names = list(configs.keys())
    labels = [n.replace("-", " ").title() for n in names]
    x = np.arange(len(names))
    width = 0.25

    u_pct, t_pct, p_pct = [], [], []
    for n in names:
        ul, ur = sum(configs[n]["u_local"]), sum(configs[n]["u_remote"])
        tl, tr = sum(configs[n]["t_local"]), sum(configs[n]["t_remote"])
        pl, pr = sum(configs[n]["p_local"]), sum(configs[n]["p_remote"])
        u_pct.append(100 * ul / (ul + ur) if (ul + ur) > 0 else 0)
        t_pct.append(100 * tl / (tl + tr) if (tl + tr) > 0 else 0)
        p_pct.append(100 * pl / (pl + pr) if (pl + pr) > 0 else 0)

    ax.bar(x - width, u_pct, width, label="Uniform", color="#EF5350", edgecolor="black", linewidth=0.5)
    ax.bar(x, t_pct, width, label="TopoStatic", color="#2196F3", edgecolor="black", linewidth=0.5)
    ax.bar(x + width, p_pct, width, label="Topo+PMU", color="#4CAF50", edgecolor="black", linewidth=0.5)

    ax.set_ylabel("Local (Same-Socket) Steals (%)", fontsize=12)
    ax.set_title("Steal Locality: Uniform vs TopoStatic vs Topo+PMU\n"
                 "Higher = more NUMA-local steals = less remote DRAM traffic", fontsize=13)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=20, ha="right", fontsize=10)
    ax.set_ylim(0, 100)
    ax.axhline(y=50, color="gray", linestyle=":", linewidth=1, alpha=0.5)

    for bars, pcts in [(ax.containers[0], u_pct), (ax.containers[1], t_pct), (ax.containers[2], p_pct)]:
        for bar, pct in zip(bars, pcts):
            ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 1,
                    f"{pct:.0f}%", ha="center", va="bottom", fontsize=8)

    ax.legend(fontsize=10)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "steal_locality.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "steal_locality.pdf"))
    plt.close(fig)
    print("  -> steal_locality.png / .pdf")

def plot_trial_timeseries(configs):
    """Line plot: per-trial times for the best config — 3 modes."""
    best = "heavy-imbalance-med"
    if best not in configs:
        best = list(configs.keys())[0]

    d = configs[best]
    trials = range(1, len(d["uniform"]) + 1)

    fig, ax = plt.subplots(figsize=(8, 4.5))
    ax.plot(trials, d["uniform"], "o-", color="#EF5350", label="Uniform", linewidth=2, markersize=6)
    ax.plot(trials, d["topostatic"], "s-", color="#2196F3", label="TopoStatic", linewidth=2, markersize=6)
    ax.plot(trials, d["topopmu"], "^-", color="#4CAF50", label="Topo+PMU", linewidth=2, markersize=6)

    u_mean = np.mean(d["uniform"])
    t_mean = np.mean(d["topostatic"])
    p_mean = np.mean(d["topopmu"])
    ax.axhline(y=u_mean, color="#EF5350", linestyle="--", alpha=0.4)
    ax.axhline(y=t_mean, color="#2196F3", linestyle="--", alpha=0.4)
    ax.axhline(y=p_mean, color="#4CAF50", linestyle="--", alpha=0.4)

    ax.fill_between(trials, d["topopmu"], d["uniform"], alpha=0.1, color="#4CAF50")
    ax.set_xlabel("Trial", fontsize=12)
    ax.set_ylabel("Execution Time (s)", fontsize=12)
    ax.set_title(f"Per-Trial Execution Times — {best.replace('-', ' ').title()}\n"
                 f"Static speedup: {u_mean/t_mean:.2f}x | PMU speedup: {u_mean/p_mean:.2f}x", fontsize=13)
    ax.legend(fontsize=10)
    ax.set_xticks(list(trials))
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "trial_timeseries.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "trial_timeseries.pdf"))
    plt.close(fig)
    print("  -> trial_timeseries.png / .pdf")

def plot_execution_comparison(configs):
    """Side-by-side bar chart: mean execution time per config — 3 modes."""
    fig, ax = plt.subplots(figsize=(10, 5))
    names = list(configs.keys())
    labels = [n.replace("-", " ").title() for n in names]
    x = np.arange(len(names))
    width = 0.25

    u_means = [np.mean(configs[n]["uniform"]) for n in names]
    t_means = [np.mean(configs[n]["topostatic"]) for n in names]
    p_means = [np.mean(configs[n]["topopmu"]) for n in names]
    u_stds = [np.std(configs[n]["uniform"]) for n in names]
    t_stds = [np.std(configs[n]["topostatic"]) for n in names]
    p_stds = [np.std(configs[n]["topopmu"]) for n in names]

    ax.bar(x - width, u_means, width, yerr=u_stds, capsize=4,
           label="Uniform", color="#EF5350", edgecolor="black", linewidth=0.5)
    ax.bar(x, t_means, width, yerr=t_stds, capsize=4,
           label="TopoStatic", color="#2196F3", edgecolor="black", linewidth=0.5)
    ax.bar(x + width, p_means, width, yerr=p_stds, capsize=4,
           label="Topo+PMU", color="#4CAF50", edgecolor="black", linewidth=0.5)

    ax.set_ylabel("Mean Execution Time (s)", fontsize=12)
    ax.set_title("Execution Time: Uniform vs TopoStatic vs Topo+PMU", fontsize=13)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=20, ha="right", fontsize=10)
    ax.legend(fontsize=10)
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
