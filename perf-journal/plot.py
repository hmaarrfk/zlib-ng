#!/usr/bin/env python3
"""perf-journal/plot.py -- regenerate every plot from perf-journal/results/*.json.

Plots (all in perf-journal/plots/):
  timeline.png            geometric-mean speedup vs. baseline for each step, per family
  latest-vs-baseline.png  per-benchmark speedup of the newest step vs. baseline
  scaling.png             aggregate throughput vs. thread count, baseline vs. newest step
  step-<name>.png         per-benchmark speedup of one step vs. baseline and vs. previous step

Speedup = baseline median time / step median time (>1 is faster).
"""
import glob, json, math, os, re, sys
from collections import OrderedDict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.join(HERE, "results")
PLOTS = os.path.join(HERE, "plots")

# Validated categorical palette (fixed order, never cycled)
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e6e5e1"

FAMILIES = OrderedDict([
    ("deflate L1-3",   re.compile(r"^deflate_bench/(level|nocrc)/.*/[123]$|^deflate_corpora/.*level:[123](/\*)?$")),
    ("deflate L6-9",   re.compile(r"^deflate_bench/(level|nocrc)/.*/[689]$|^deflate_corpora/.*level:[689](/\*)?$")),
    ("deflate strategies", re.compile(r"^deflate_bench/(huffman|rle|fixed|filtered|sync_flush)/")),
    ("inflate",        re.compile(r"^inflate_bench/|^inflate_corpora/|^inflate_chunked/")),
    ("small buffers",  re.compile(r"^(compress|uncompress)_bench/")),
    ("checksums",      re.compile(r"^(crc32|adler32)")),
    ("internals",      re.compile(r"^(longest_match|insert_batch|compare256|slide_hash|chunkmemset)")),
])

def family_of(name):
    for fam, rx in FAMILIES.items():
        if rx.search(name):
            return fam
    return "other"

CORPUS_RX = re.compile(r"^(deflate_corpora/[^/]+)/[^/]+/(level:\d+)$|^(inflate_corpora/[^/]+)/[^/]+$")

def collapse_corpora(times):
    """Fold per-file corpora entries into one geometric-mean entry per (corpus, level),
    so the bar plots stay readable. The per-file numbers stay in the JSON."""
    groups = {}
    out = {}
    for name, t in times.items():
        m = CORPUS_RX.match(name)
        if m:
            key = (m.group(1) + "/" + m.group(2)) if m.group(1) else m.group(3)
            groups.setdefault(key, []).append(t)
        else:
            out[name] = t
    for key, ts in groups.items():
        out[key + "/*"] = gmean(ts)
    return out

def load():
    steps = []
    for path in sorted(glob.glob(os.path.join(RESULTS, "*.json"))):
        d = json.load(open(path))
        d["times"] = {b["name"].replace("_median", ""): b["real_time"] for b in d["benchmarks"]}
        d["times_collapsed"] = collapse_corpora(d["times"])
        steps.append(d)
    return steps

def gmean(xs):
    xs = [x for x in xs if x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float("nan")

def style(ax):
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=8)
    ax.yaxis.grid(True, color=GRID, linewidth=0.8)
    ax.xaxis.grid(False)
    ax.set_axisbelow(True)

def speedups(base, step, collapsed=False):
    key = "times_collapsed" if collapsed else "times"
    out = {}
    for name, t in step[key].items():
        if name in base[key] and t > 0:
            out[name] = base[key][name] / t
    return out

def plot_timeline(steps):
    base = steps[0]
    fams = list(FAMILIES.keys())
    fig, ax = plt.subplots(figsize=(max(7, 0.9 * len(steps) + 3), 4.2), dpi=150)
    xs = list(range(len(steps)))
    for i, fam in enumerate(fams):
        ys = []
        for st in steps:
            sp = speedups(base, st)
            ys.append(gmean([v for k, v in sp.items() if family_of(k) == fam]))
        if all(math.isnan(y) for y in ys):
            continue
        ax.plot(xs, ys, color=SERIES[i], linewidth=2, marker="o", markersize=5, label=fam)
        ax.annotate(fam, (xs[-1], ys[-1]), xytext=(4, 0), textcoords="offset points",
                    fontsize=7, color=INK2, va="center")
    ax.axhline(1.0, color=INK2, linewidth=0.8, linestyle="--")
    ax.set_xticks(xs)
    ax.set_xticklabels([s["step"] for s in steps], rotation=35, ha="right", fontsize=7)
    ax.set_ylabel("geometric-mean speedup vs. baseline", color=INK2, fontsize=8)
    ax.set_title("Speedup per journal step, by benchmark family", color=INK, fontsize=10, loc="left")
    ax.legend(fontsize=7, frameon=False, ncol=4, loc="upper left")
    style(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(PLOTS, "timeline.png"))
    plt.close(fig)

def plot_bars(base, step, prev, fname, title):
    sp = speedups(base, step, collapsed=True)
    spp = speedups(prev, step, collapsed=True) if prev is not None and prev is not base else None
    names = sorted(sp, key=lambda n: (list(FAMILIES).index(family_of(n)) if family_of(n) in FAMILIES else 99, n))
    if not names:
        return
    fig_h = max(4, 0.16 * len(names) + 1.5)
    fig, ax = plt.subplots(figsize=(9, fig_h), dpi=130)
    ys = list(range(len(names)))
    cols = [SERIES[list(FAMILIES).index(family_of(n)) % len(SERIES)] if family_of(n) in FAMILIES else INK2 for n in names]
    ax.barh(ys, [sp[n] for n in names], color=cols, height=0.7, label="vs. baseline")
    if spp:
        ax.scatter([spp.get(n, float("nan")) for n in names], ys, color=INK, s=8, zorder=3, label="vs. previous step")
    ax.axvline(1.0, color=INK2, linewidth=0.8, linestyle="--")
    ax.set_yticks(ys)
    ax.set_yticklabels(names, fontsize=6)
    ax.invert_yaxis()
    ax.set_xscale("log")
    ax.set_xlabel("speedup (log scale, >1 is faster)", color=INK2, fontsize=8)
    ax.set_title(title, color=INK, fontsize=10, loc="left")
    ax.xaxis.grid(True, color=GRID, linewidth=0.8)
    ax.yaxis.grid(False)
    ax.legend(fontsize=7, frameon=False, loc="lower right")
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.tick_params(colors=INK2)
    fig.tight_layout()
    fig.savefig(os.path.join(PLOTS, fname))
    plt.close(fig)

def plot_scaling(steps):
    base, last = steps[0], steps[-1]
    keys = sorted({(r["op"], r["level"]) for r in base["scaling"]}, key=lambda k: (k[0], k[1]))
    fig, axes = plt.subplots(1, len(keys), figsize=(3.2 * len(keys), 3.4), dpi=150, sharey=False)
    if len(keys) == 1:
        axes = [axes]
    for ax, (op, lvl) in zip(axes, keys):
        for st, col, lab in ((base, SERIES[0], base["step"]), (last, SERIES[1], last["step"])):
            rows = sorted([r for r in st["scaling"] if r["op"] == op and r["level"] == lvl], key=lambda r: r["threads"])
            ax.plot([r["threads"] for r in rows], [r["aggregate_MBps"] for r in rows], color=col, marker="o", linewidth=2, markersize=5, label=lab)
        ax.set_title(f"{op}" + (f" level {lvl}" if lvl >= 0 else ""), fontsize=9, color=INK, loc="left")
        ax.set_xlabel("threads (independent streams)", fontsize=7, color=INK2)
        ax.set_xscale("log", base=2)
        ax.set_ylabel("aggregate MB/s", fontsize=7, color=INK2)
        ax.set_ylim(bottom=0)
        style(ax)
    axes[0].legend(fontsize=7, frameon=False)
    fig.suptitle("Thread scaling on silesia/dickens: baseline vs. newest step", fontsize=10, color=INK, x=0.01, ha="left")
    fig.tight_layout()
    fig.savefig(os.path.join(PLOTS, "scaling.png"))
    plt.close(fig)

def main():
    os.makedirs(PLOTS, exist_ok=True)
    steps = load()
    if not steps:
        print("no results"); return
    base = steps[0]
    plot_timeline(steps)
    for i, st in enumerate(steps):
        prev = steps[i - 1] if i > 0 else None
        plot_bars(base, st, prev, f"step-{st['step']}.png", f"{st['step']} ({st['commit']}): speedup per benchmark")
    plot_bars(base, steps[-1], steps[-2] if len(steps) > 1 else None, "latest-vs-baseline.png",
              f"newest step {steps[-1]['step']} vs. baseline {base['step']}")
    plot_scaling(steps)
    # Summary table to stdout for the journal
    print(f"{'step':34s} " + " ".join(f"{f[:10]:>10s}" for f in FAMILIES))
    for st in steps:
        sp = speedups(base, st)
        row = [gmean([v for k, v in sp.items() if family_of(k) == f]) for f in FAMILIES]
        print(f"{st['step']:34s} " + " ".join(f"{r:10.3f}" for r in row))

if __name__ == "__main__":
    main()
