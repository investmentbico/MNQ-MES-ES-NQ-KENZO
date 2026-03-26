#!/usr/bin/env python3
"""
tv_converter.py
───────────────
Convert TradingView exported CSV files to the format expected by
ema21_flip_backtest.cpp

TradingView export format:
  time,open,high,low,close,Volume,Plot,BUY,SELL
  (time = Unix seconds)

Backtester format:
  timestamp,open,high,low,close,volume
  (timestamp = Unix milliseconds)

Usage:
  python tv_converter.py --input CME_MINI_MES1___5.csv --output data/MES_5min.csv
  python tv_converter.py --dir /path/to/downloads          # batch convert all CSVs

Timeframe is auto-detected from timestamp spacing and applied to the output filename
when using --dir mode. You can also specify --symbol and --timeframe manually.

Timeframe detection table:
  5s spacing   → 5sec
  15s          → 15sec
  30s          → 30sec
  60s          → 1min
  120s         → 2min
  300s         → 5min
  720s         → 12min
  900s         → 15min
"""

import argparse
import csv
import os
import sys


TF_MAP = {
    5:   "5sec",
    15:  "15sec",
    30:  "30sec",
    60:  "1min",
    120: "2min",
    300: "5min",
    720: "12min",
    900: "15min",
}


def detect_interval(rows):
    """Return bar interval in seconds from first few rows."""
    ts = [int(float(r["time"])) for r in rows[:5]]
    diffs = [ts[i+1] - ts[i] for i in range(len(ts)-1)]
    return min(diffs)


def convert(src_path, dst_path):
    rows = []
    with open(src_path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                ts = int(float(row["time"])) * 1000
                o  = float(row["open"])
                h  = float(row["high"])
                l  = float(row["low"])
                c  = float(row["close"])
                v  = float(row["Volume"])
                if h >= l and c > 0:
                    rows.append([ts, o, h, l, c, v])
            except Exception:
                pass

    if not rows:
        print(f"[WARN]  No valid rows in {src_path}")
        return False

    os.makedirs(os.path.dirname(dst_path) or ".", exist_ok=True)
    with open(dst_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["timestamp", "open", "high", "low", "close", "volume"])
        w.writerows(rows)

    import datetime
    first = datetime.datetime.fromtimestamp(
        rows[0][0] // 1000, datetime.timezone.utc).strftime("%b %d %Y")
    last  = datetime.datetime.fromtimestamp(
        rows[-1][0] // 1000, datetime.timezone.utc).strftime("%b %d %Y")
    print(f"[OK]    {dst_path}  ({len(rows):,} bars  {first} → {last})")
    return True


def batch_convert(src_dir, dst_dir, symbol="MES"):
    """Convert all CSVs in src_dir, auto-detecting timeframe."""
    converted = 0
    for fname in sorted(os.listdir(src_dir)):
        if not fname.endswith(".csv"):
            continue
        src_path = os.path.join(src_dir, fname)
        try:
            with open(src_path, newline="") as f:
                rows = list(csv.DictReader(f))
            if not rows or "time" not in rows[0]:
                print(f"[SKIP]  {fname}  (not a TradingView export)")
                continue
            interval = detect_interval(rows)
            tf = TF_MAP.get(interval, f"{interval}s")
            dst_path = os.path.join(dst_dir, f"{symbol}_{tf}.csv")
            convert(src_path, dst_path)
            converted += 1
        except Exception as e:
            print(f"[ERR]   {fname}: {e}")
    print(f"\nConverted {converted} file(s) → {os.path.abspath(dst_dir)}/")


def main():
    parser = argparse.ArgumentParser(
        description="Convert TradingView CSV exports to backtester format")
    parser.add_argument("--input",     help="Single source CSV file")
    parser.add_argument("--output",    help="Single destination CSV file")
    parser.add_argument("--dir",       help="Batch: source directory of TV exports")
    parser.add_argument("--outdir",    default="./data", help="Batch: output directory (default: ./data)")
    parser.add_argument("--symbol",    default="MES",    help="Symbol name for output file (default: MES)")
    parser.add_argument("--timeframe", help="Force timeframe label (e.g. 5min). Auto-detected if omitted.")
    args = parser.parse_args()

    if args.dir:
        batch_convert(args.dir, args.outdir, args.symbol)
    elif args.input and args.output:
        convert(args.input, args.output)
    elif args.input:
        # auto-detect timeframe and build output path
        with open(args.input, newline="") as f:
            rows = list(csv.DictReader(f))
        interval = detect_interval(rows)
        tf = args.timeframe or TF_MAP.get(interval, f"{interval}s")
        dst = os.path.join(args.outdir, f"{args.symbol}_{tf}.csv")
        convert(args.input, dst)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
