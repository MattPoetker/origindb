#!/usr/bin/env python3
"""Compare two instantdb_bench JSON reports.

Usage: bench_compare.py baseline.json current.json [--threshold 10]

Exits nonzero if any benchmark regressed by more than --threshold percent
(throughput drop or p99 latency increase), so it can gate CI.
"""
import argparse
import json
import sys


def load(path):
    with open(path) as f:
        doc = json.load(f)
    return {r["name"]: r for r in doc["results"]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("baseline")
    ap.add_argument("current")
    ap.add_argument("--threshold", type=float, default=10.0,
                    help="regression threshold in percent (default 10)")
    args = ap.parse_args()

    base, cur = load(args.baseline), load(args.current)
    regressions = []

    print(f"{'benchmark':<38} {'base ops/s':>12} {'cur ops/s':>12} {'Δ%':>8} "
          f"{'base p99':>10} {'cur p99':>10}")
    print("-" * 96)
    for name, b in base.items():
        c = cur.get(name)
        if not c:
            print(f"{name:<38} {'(missing in current)':>12}")
            continue
        d_tp = 100.0 * (c["ops_per_sec"] - b["ops_per_sec"]) / b["ops_per_sec"] \
            if b["ops_per_sec"] else 0.0
        marker = ""
        if d_tp < -args.threshold:
            marker = "  ⚠ throughput"
            regressions.append(name)
        elif b.get("p99_us", 0) > 0 and c.get("p99_us", 0) > 0:
            d_p99 = 100.0 * (c["p99_us"] - b["p99_us"]) / b["p99_us"]
            if d_p99 > args.threshold:
                marker = "  ⚠ p99"
                regressions.append(name)
        print(f"{name:<38} {b['ops_per_sec']:>12.0f} {c['ops_per_sec']:>12.0f} "
              f"{d_tp:>+7.1f}% {b.get('p99_us', 0):>9.1f}u {c.get('p99_us', 0):>9.1f}u{marker}")

    for name in cur:
        if name not in base:
            print(f"{name:<38} (new benchmark)")

    if regressions:
        print(f"\n{len(regressions)} regression(s) beyond {args.threshold}%: "
              + ", ".join(regressions))
        sys.exit(1)
    print("\nno regressions beyond threshold")


if __name__ == "__main__":
    main()
