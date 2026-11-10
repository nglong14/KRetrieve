#!/usr/bin/env python3
"""Plot latency percentiles and failure rates from <dir>/results/<label>-<qps>.csv.

Usage: scripts/plot.py [dir]   (default dir: bench; cloud runs use bench/cloud)
Writes <dir>/latency.png and <dir>/summary.csv.
"""

import csv
import glob
import math
import os
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

OK, OVERLOADED = 0, 3


def percentile(sorted_values, q):
    """Nearest rank, same as kretrieve::percentile."""
    if not sorted_values:
        return 0
    rank = min(max(math.ceil(q * len(sorted_values)), 1), len(sorted_values))
    return sorted_values[rank - 1]


def summarize(path):
    latencies, incomplete, overloaded, errors = [], 0, 0, 0
    with open(path) as f:
        for row in csv.DictReader(f):
            status = int(row["status"])
            if status == OK:
                # Ok replies only: shed (Overloaded) replies are fast and would hide the tail.
                latencies.append(int(row["latency_us"]) / 1000)
                incomplete += int(row["missing"]) > 0
            elif status == OVERLOADED:
                overloaded += 1
            else:
                errors += 1
    latencies.sort()
    n = len(latencies) + overloaded + errors
    return {
        "samples": n,
        "p50_ms": percentile(latencies, 0.50),
        "p95_ms": percentile(latencies, 0.95),
        "p99_ms": percentile(latencies, 0.99),
        "incomplete_pct": 100 * incomplete / n,
        "overloaded_pct": 100 * overloaded / n,
        "error_pct": 100 * errors / n,
    }


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "bench"
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    runs = defaultdict(dict)  # label -> qps -> summary
    for path in glob.glob(f"{root}/results/*.csv"):
        label, qps = os.path.basename(path)[: -len(".csv")].rsplit("-", 1)
        runs[label][int(qps)] = summarize(path)
    if not runs:
        raise SystemExit(f"no {root}/results/*.csv; run scripts/bench.sh or scripts/remote_bench.sh first")

    with open(f"{root}/summary.csv", "w", newline="") as f:
        fields = ["label", "qps", "samples", "p50_ms", "p95_ms", "p99_ms",
                  "incomplete_pct", "overloaded_pct", "error_pct"]
        writer = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        for label in sorted(runs):
            for qps in sorted(runs[label]):
                row = {k: round(v, 3) if isinstance(v, float) else v
                       for k, v in runs[label][qps].items()}
                writer.writerow({"label": label, "qps": qps, **row})

    fig, (lat, bad) = plt.subplots(1, 2, figsize=(12, 4.5))
    styles = {"p50_ms": ":", "p95_ms": "--", "p99_ms": "-"}
    for color, label in zip(plt.rcParams["axes.prop_cycle"].by_key()["color"], sorted(runs)):
        qps = sorted(runs[label])
        for key, style in styles.items():
            lat.plot(qps, [runs[label][q][key] for q in qps], style, color=color, marker="o",
                     markersize=3, label=f"{label} {key[:3]}")
        failed = [runs[label][q]["incomplete_pct"] + runs[label][q]["overloaded_pct"]
                  + runs[label][q]["error_pct"] for q in qps]
        bad.plot(qps, failed, "-", color=color, marker="o", markersize=3, label=label)
    lat.set(xscale="log", yscale="log", xlabel="offered load (queries/s)", ylabel="latency (ms)",
            title="Latency of Ok replies, from scheduled send time")
    bad.set(xscale="log", xlabel="offered load (queries/s)", ylabel="% of requests",
            title="Incomplete + overloaded + errors")
    for ax in (lat, bad):
        ax.grid(True, which="both", alpha=0.3)
        ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(f"{root}/latency.png", dpi=120)
    print(f"wrote {root}/latency.png and {root}/summary.csv")


if __name__ == "__main__":
    main()
