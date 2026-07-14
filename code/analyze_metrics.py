#!/usr/bin/env python3
"""plot_metrics.py - Python/matplotlib equivalent of analyze_metrics.m

Reads metrics_log.txt (the exact CSV produced by the C collector) and renders
the three figures the assignment asks for, then prints a numerical summary.

Usage:
    python3 plot_metrics.py [metrics_log.txt] [--save]

    --save   write PNGs next to the log instead of opening windows
             (useful on a headless Raspberry Pi).

Dependencies: numpy, matplotlib  (pip install numpy matplotlib)

CSV columns:
    Seconds, Nanoseconds, Commit_Count, Identity_Count, Account_Count,
    Info_Count, Buffer_Occupancy_Pct, CPU_Pct
"""
import sys
import argparse
import numpy as np

import matplotlib
# Pick a non-interactive backend automatically when saving / headless.
if "--save" in sys.argv:
    matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main() -> None:
    ap = argparse.ArgumentParser(description="Plot metrics_30062026.txt")
    ap.add_argument("logfile", nargs="?", default="metrics_30062026.txt")
    ap.add_argument("--save", action="store_true",
                    help="save PNGs instead of showing windows")
    args = ap.parse_args()

    data = np.genfromtxt(args.logfile, delimiter=",", skip_header=1)
    if data.ndim == 1:               # a single data row
        data = data.reshape(1, -1)
    if data.size == 0:
        sys.exit(f"No data rows in {args.logfile}")

    secs   = data[:, 0]
    nsecs  = data[:, 1]
    commit = data[:, 2]
    ident  = data[:, 3]
    acct   = data[:, 4]
    info   = data[:, 5]
    buffpc = data[:, 6]
    cpu    = data[:, 7]

    tabs = secs + nsecs * 1e-9
    t = tabs - tabs[0]
    rate = commit + ident + acct + info

    # ---- (1) Jitter ----------------------------------------------------
    dt = np.diff(tabs)
    jitter_ms = (dt - 1.0) * 1000.0
    tj = t[1:]

    fig1, ax1 = plt.subplots(figsize=(10, 4))
    ax1.plot(tj, jitter_ms, linewidth=0.9)
    ax1.axhline(0, color="k", linestyle="--", linewidth=0.8)
    ax1.set_xlabel("Time (s)")
    ax1.set_ylabel("Jitter (ms)")
    ax1.set_title("Periodic logger jitter (deviation from the ideal 1.000 s)")
    ax1.grid(True)
    fig1.tight_layout()

    # ---- (2) Load & buffer (dual axis) ---------------------------------
    fig2, axL = plt.subplots(figsize=(10, 4))
    axR = axL.twinx()
    l1, = axL.plot(t, rate, color="tab:blue", linewidth=0.9, label="Rate (Hz)")
    l2, = axR.plot(t, buffpc, color="tab:red", linewidth=0.9, label="Buffer (%)")
    axL.set_xlabel("Time (s)")
    axL.set_ylabel("Incoming messages (Hz)", color="tab:blue")
    axR.set_ylabel("Buffer occupancy (%)", color="tab:red")
    axR.set_ylim(0, 100)
    axL.set_title("Network load (Hz) vs. circular-buffer occupancy (%)")
    axL.grid(True)
    axL.legend(handles=[l1, l2], loc="best")
    fig2.tight_layout()

    # ---- (3) CPU correlation -------------------------------------------
    idle = 100.0 - cpu
    fig3, (axA, axB) = plt.subplots(1, 2, figsize=(12, 4.5))

    axA.scatter(rate, cpu, s=12)
    if rate.size >= 2 and np.ptp(rate) > 0:
        p = np.polyfit(rate, cpu, 1)
        xx = np.linspace(rate.min(), rate.max(), 100)
        axA.plot(xx, np.polyval(p, xx), "r-", linewidth=1.2)
        r = np.corrcoef(rate, cpu)[0, 1]
        axA.set_title(f"CPU busy vs. rate  (r = {r:.3f})")
    else:
        axA.set_title("CPU busy vs. rate")
    axA.set_xlabel("Incoming rate (Hz)")
    axA.set_ylabel("CPU busy (%)")
    axA.grid(True)

    axBr = axB.twinx()
    axB.plot(t, rate, color="tab:blue", linewidth=0.9)
    axBr.plot(t, idle, color="tab:green", linewidth=0.9)
    axB.set_xlabel("Time (s)")
    axB.set_ylabel("Rate (Hz)", color="tab:blue")
    axBr.set_ylabel("CPU idle (%)", color="tab:green")
    axBr.set_ylim(0, 100)
    axB.set_title("Rate (Hz) and CPU idle (%) over time")
    axB.grid(True)
    fig3.tight_layout()

    # ---- Numerical summary ---------------------------------------------
    span_s = tabs[-1] - tabs[0]
    n_lines = tabs.size
    expected = round(span_s) + 1
    lost = max(expected - n_lines, 0)
    mean_err = float(np.mean(dt - 1.0)) if dt.size else 0.0
    drift_24h = mean_err * 86400.0

    print("\n===== metrics_log.txt summary =====")
    print(f"Log lines (seconds)      : {n_lines}")
    print(f"Observed span            : {span_s:.3f} s (~{span_s/3600:.2f} h)")
    print(f"Lost/missing seconds     : {lost}")
    print("--- Jitter (vs ideal 1 s) ---")
    if jitter_ms.size:
        print(f"  mean                   : {jitter_ms.mean():+.3f} ms")
        print(f"  std                    : {jitter_ms.std():.3f} ms")
        print(f"  min / max              : {jitter_ms.min():+.3f} / {jitter_ms.max():+.3f} ms")
        print(f"  mean |jitter|          : {np.abs(jitter_ms).mean():.3f} ms")
    print("--- Drift ---")
    print(f"  mean timing error      : {mean_err:+.3e} s/line")
    print(f"  extrapolated over 24 h : {drift_24h:+.3f} s")
    print("--- Throughput ---")
    print(f"  mean rate              : {rate.mean():.1f} Hz")
    print(f"  peak rate              : {int(rate.max())} Hz")
    print(f"  total messages         : {int(rate.sum())}")
    print("--- Buffer / CPU ---")
    print(f"  peak buffer occupancy  : {buffpc.max():.2f} %")
    print(f"  mean / peak CPU busy   : {cpu.mean():.2f} / {cpu.max():.2f} %")
    print("===================================")

    if args.save:
        base = args.logfile.rsplit(".", 1)[0]
        fig1.savefig(f"{base}_jitter.png", dpi=130)
        fig2.savefig(f"{base}_load_buffer.png", dpi=130)
        fig3.savefig(f"{base}_cpu.png", dpi=130)
        print(f"\nSaved: {base}_jitter.png, {base}_load_buffer.png, {base}_cpu.png")
    else:
        plt.show()


if __name__ == "__main__":
    main()
