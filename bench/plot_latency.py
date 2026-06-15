#!/usr/bin/env python3
"""Plot TopoSteal tail latency results from the sweep output files."""

import os
import re
import sys

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("Install matplotlib and numpy: pip install matplotlib numpy")
    sys.exit(1)

OUT_DIR = os.path.join(os.path.dirname(__file__), "figures")
os.makedirs(OUT_DIR, exist_ok=True)

# Parse the "Cross-Socket vs Local Steal Latency Breakdown" from each file
def parse_breakdown(filepath):
    """Extract final breakdown section from a latency output file."""
    with open(filepath) as f:
        text = f.read()

    # Find the final breakdown section
    marker = "Cross-Socket vs Local Steal Latency Breakdown"
    idx = text.rfind(marker)
    if idx == -1:
        return None
    section = text[idx:]

    results = {}
    for mode in ["UNIFORM", "TOPOSTATIC", "TOPO+PMU"]:
        mode_idx = section.find(mode + ":")
        if mode_idx == -1:
            continue
        block = section[mode_idx:mode_idx+500]

        def extract_line(label):
            match = re.search(label + r'.*?p50=\s*([\d.]+)\s+p99=\s*([\d.]+)\s+p99\.9=\s*([\d.]+)\s+max=\s*([\d.]+)', block)
            if match:
                return [float(match.group(i)) for i in range(1, 5)]
            return [0, 0, 0, 0]

        steals_match = re.search(r'Steals:\s*(\d+)\s*local\s*/\s*(\d+)\s*remote', block)
        local_steals = int(steals_match.group(1)) if steals_match else 0
        remote_steals = int(steals_match.group(2)) if steals_match else 0

        results[mode] = {
            'all': extract_line('All tasks:'),
            'cross': extract_line('Cross-socket:'),
            'local': extract_line('Local steals:'),
            'local_steals': local_steals,
            'remote_steals': remote_steals,
        }
    return results

# Parse per-trial mean latencies
def parse_trials(filepath):
    """Extract per-trial mean latencies."""
    with open(filepath) as f:
        lines = f.readlines()

    uniform, topostatic, topopmu = [], [], []
    for line in lines:
        match = re.match(r'\s+Uniform\s+(\d+)', line)
        if match:
            uniform.append(int(match.group(1)))
        match = re.match(r'\s+TopoStatic\s+(\d+)', line)
        if match:
            topostatic.append(int(match.group(1)))
        match = re.match(r'\s+Topo\+PMU\s+(\d+)', line)
        if match:
            topopmu.append(int(match.group(1)))
    return uniform, topostatic, topopmu


def plot_cross_vs_local(configs):
    """Bar chart comparing cross-socket vs local steal latency at p50."""
    fig, axes = plt.subplots(1, 3, figsize=(14, 5))

    config_names = ["Config A\n(2400 tasks, 500K iters)", "Config B\n(1200 tasks, 2M iters)",
                    "Config C\n(4800 tasks, 100K iters)"]

    for ax, (name, data, label) in zip(axes, zip(config_names, configs, ["A", "B", "C"])):
        if data is None:
            continue
        modes = ["UNIFORM", "TOPOSTATIC", "TOPO+PMU"]
        x = np.arange(len(modes))
        width = 0.35

        cross_p50 = [data[m]['cross'][0] for m in modes]
        local_p50 = [data[m]['local'][0] for m in modes]

        bars1 = ax.bar(x - width/2, cross_p50, width, label="Cross-Socket", color="#EF5350", edgecolor="black", linewidth=0.5)
        bars2 = ax.bar(x + width/2, local_p50, width, label="Local", color="#66BB6A", edgecolor="black", linewidth=0.5)

        ax.set_ylabel("p50 Latency (us)")
        ax.set_title(label)
        ax.set_xticks(x)
        ax.set_xticklabels(["Uniform", "TopoStatic", "Topo+PMU"], fontsize=9)
        ax.legend(fontsize=9)

        # Add penalty annotation
        for i, (c, l) in enumerate(zip(cross_p50, local_p50)):
            if l > 0:
                penalty = (c - l) / l * 100
                ax.text(i, max(c, l) * 1.02, f"+{penalty:.0f}%", ha="center", fontsize=8, color="red")

    fig.suptitle("Cross-Socket vs Local Steal Latency (p50)\nCross-socket steals incur NUMA penalty", fontsize=13)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "latency_cross_vs_local.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "latency_cross_vs_local.pdf"))
    plt.close(fig)
    print("  -> latency_cross_vs_local.png / .pdf")


def plot_tail_reduction(configs):
    """Show p99.9 reduction across modes."""
    fig, ax = plt.subplots(figsize=(10, 5))

    config_labels = ["Config A\n500K iters", "Config B\n2M iters", "Config C\n100K iters"]
    x = np.arange(3)
    width = 0.25

    uniform_p999 = []
    static_p999 = []
    pmu_p999 = []

    for data in configs:
        if data is None:
            uniform_p999.append(0)
            static_p999.append(0)
            pmu_p999.append(0)
            continue
        uniform_p999.append(data["UNIFORM"]["all"][2])
        static_p999.append(data["TOPOSTATIC"]["all"][2])
        pmu_p999.append(data["TOPO+PMU"]["all"][2])

    ax.bar(x - width, uniform_p999, width, label="Uniform", color="#EF5350", edgecolor="black", linewidth=0.5)
    ax.bar(x, static_p999, width, label="TopoStatic", color="#2196F3", edgecolor="black", linewidth=0.5)
    ax.bar(x + width, pmu_p999, width, label="Topo+PMU", color="#4CAF50", edgecolor="black", linewidth=0.5)

    ax.set_ylabel("p99.9 Task Latency (us)", fontsize=12)
    ax.set_title("Tail Latency (p99.9) Across Configurations\nTopoSteal reduces worst-case latency exposure", fontsize=13)
    ax.set_xticks(x)
    ax.set_xticklabels(config_labels, fontsize=10)
    ax.legend(fontsize=10)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "latency_p999.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "latency_p999.pdf"))
    plt.close(fig)
    print("  -> latency_p999.png / .pdf")


def plot_steal_exposure(configs):
    """Pie/bar showing fraction of tasks exposed to cross-socket penalty."""
    fig, axes = plt.subplots(1, 3, figsize=(12, 4))
    config_labels = ["Config A (500K)", "Config B (2M)", "Config C (100K)"]

    for ax, data, label in zip(axes, configs, config_labels):
        if data is None:
            continue
        modes = ["Uniform", "TopoStatic", "Topo+PMU"]
        remote_pct = []
        for m in ["UNIFORM", "TOPOSTATIC", "TOPO+PMU"]:
            total = data[m]['local_steals'] + data[m]['remote_steals']
            remote_pct.append(100 * data[m]['remote_steals'] / total if total > 0 else 0)

        colors = ["#EF5350", "#2196F3", "#4CAF50"]
        bars = ax.bar(modes, remote_pct, color=colors, edgecolor="black", linewidth=0.5)
        ax.set_ylabel("Cross-Socket Steals (%)")
        ax.set_title(label)
        ax.set_ylim(0, 60)
        ax.axhline(y=50, color="gray", linestyle=":", alpha=0.5)

        for bar, pct in zip(bars, remote_pct):
            ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 1,
                    f"{pct:.0f}%", ha="center", fontsize=10, fontweight="bold")

    fig.suptitle("Cross-Socket Steal Exposure\nTopoSteal reduces NUMA penalty exposure from ~50% to ~14%", fontsize=12)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "latency_steal_exposure.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "latency_steal_exposure.pdf"))
    plt.close(fig)
    print("  -> latency_steal_exposure.png / .pdf")


def plot_hft_percentile_ladder(configs):
    """Percentile ladder plot — the HFT classic visualization."""
    fig, ax = plt.subplots(figsize=(9, 5))

    # Use Config C (HFT burst) for this
    data = configs[2]
    if data is None:
        return

    percentiles = ["p50", "p99", "p99.9", "max"]
    x_pos = [0, 1, 2, 3]

    for mode, color, marker, label in [
        ("UNIFORM", "#EF5350", "o", "Uniform"),
        ("TOPOSTATIC", "#2196F3", "s", "TopoStatic"),
        ("TOPO+PMU", "#4CAF50", "^", "Topo+PMU"),
    ]:
        vals = data[mode]['all']  # [p50, p99, p99.9, max]
        ax.plot(x_pos, vals, f"{marker}-", color=color, label=label, linewidth=2, markersize=8)

    ax.set_xticks(x_pos)
    ax.set_xticklabels(percentiles, fontsize=11)
    ax.set_ylabel("Task Latency (us)", fontsize=12)
    ax.set_xlabel("Percentile", fontsize=12)
    ax.set_title("Latency Percentile Ladder — HFT Burst Config\n"
                 "(4800 tasks, 100K iters, extreme imbalance)", fontsize=13)
    ax.legend(fontsize=11)
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT_DIR, "latency_percentile_ladder.png"), dpi=300)
    fig.savefig(os.path.join(OUT_DIR, "latency_percentile_ladder.pdf"))
    plt.close(fig)
    print("  -> latency_percentile_ladder.png / .pdf")


if __name__ == "__main__":
    base = os.path.dirname(os.path.dirname(__file__))
    files = [
        os.path.join(base, "latency_A.txt"),
        os.path.join(base, "latency_B.txt"),
        os.path.join(base, "latency_C.txt"),
    ]

    configs = []
    for f in files:
        if os.path.exists(f):
            configs.append(parse_breakdown(f))
            print(f"Parsed: {f}")
        else:
            configs.append(None)
            print(f"Missing: {f}")

    print("\nGenerating latency figures:")
    plot_cross_vs_local(configs)
    plot_tail_reduction(configs)
    plot_steal_exposure(configs)
    plot_hft_percentile_ladder(configs)
    print(f"\nAll figures saved to {OUT_DIR}/")
