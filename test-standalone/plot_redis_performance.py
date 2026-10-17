#!/usr/bin/env python3
"""Paper Fig. 17: standalone insert/query throughput of rax vs rax + stail fast path.

usage: plot_redis_performance.py <results.csv> <out.pdf>
The CSV is the one written by the standalone sweep:
    N,K,L,tree_type,insert_ns,query_ns[,...]   (tree_type RAX or QuART; rows per (K,tree) are averaged)
Figure style follows the paper's other figures (Libertine via LaTeX, no frame, legend above the panels).
"""
import csv, sys
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

plt.rcParams["font.family"] = "serif"
plt.rcParams["font.serif"] = ["Linux Libertine O", "Linux Libertine", "Libertine"]
plt.rcParams["text.usetex"] = True
plt.rcParams["text.latex.preamble"] = r"\usepackage{libertine}"
plt.rcParams["font.size"] = 11

COLORS = {"RAX": "#55a868", "QuART": "#6a0dad"}
LABELS = {"RAX": r"\emph{rax}", "QuART": r"QuART-\emph{rax}"}

def load(path):
    acc = defaultdict(list)  # (K, tree) -> [(insert_ns, query_ns, N)]
    with open(path) as f:
        for row in csv.DictReader(f):
            if not row.get("insert_ns") or not row["insert_ns"].strip().isdigit():
                continue
            acc[(int(row["K"]), row["tree_type"])].append(
                (int(row["insert_ns"]), int(row["query_ns"]), int(row["N"])))
    out = {}
    for key, runs in acc.items():
        n = runs[0][2]
        ins = np.mean([r[0] for r in runs]); qry = np.mean([r[1] for r in runs])
        out[key] = (n / ins * 1e3, n / qry * 1e3)  # M ops/s = N / ns * 1e9 / 1e6
    return out

def main(csv_path, out_path):
    data = load(csv_path)
    ks = sorted({k for k, _ in data})
    x = np.arange(len(ks)); w = 0.36
    fig, axes = plt.subplots(1, 2, figsize=(6.95, 1.85))
    for ax, idx, title in ((axes[0], 0, "(a) Inserts"), (axes[1], 1, "(b) Queries")):
        for off, tree in ((-w / 2, "RAX"), (w / 2, "QuART")):
            ys = [data[(k, tree)][idx] for k in ks]
            ax.bar(x + off, ys, w, color=COLORS[tree], label=LABELS[tree], edgecolor="none")
        ax.set_xticks(x); ax.set_xticklabels([str(k) for k in ks])
        ax.set_xlabel(r"$K$ (\%)"); ax.set_title(title)
        ax.set_ylabel("Throughput (M ops/sec)")
        ax.spines["top"].set_visible(False); ax.spines["right"].set_visible(False)
        ax.yaxis.grid(True, linestyle=":", color="lightgray"); ax.set_axisbelow(True)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=2, frameon=False, bbox_to_anchor=(0.5, 1.02))
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(out_path)
    for k in ks:
        r, q = data[(k, "RAX")], data[(k, "QuART")]
        print(f"K={k:3d}: insert rax {r[0]:.2f} stail {q[0]:.2f} M/s ({q[0]/r[0]:.2f}x); query rax {r[1]:.2f} stail {q[1]:.2f} M/s ({q[1]/r[1]:.2f}x)")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
