#!/usr/bin/env python3
"""
data_downloader.py
──────────────────
Downloads intraday futures data and converts it to the CSV format
expected by ema21_flip_backtest.cpp

Requirements:  pip install yfinance pandas

Usage:
    python data_downloader.py --symbol MES --timeframe 5s
    python data_downloader.py --all        # download all combos

Output files are written to ./data/ as:
    {SYMBOL}_{TIMEFRAME}.csv   (e.g., MES_5sec.csv)

CSV format:
    timestamp,open,high,low,close,volume
    (timestamp = Unix milliseconds)

NOTE: Yahoo Finance free API only provides ~60 days of 1m data.
For full historical data consider:
  • Interactive Brokers (TWS API)
  • Tradovate / Rithmic historical data
  • NinjaTrader tick/bar export
  • TradeStation data export
"""

import argparse
import os
import sys
from datetime import datetime, timedelta

try:
    import yfinance as yf
    import pandas as pd
except ImportError:
    print("Install requirements:  pip install yfinance pandas")
    sys.exit(1)

# ─── Yahoo Finance ticker mapping ───────────────────────────────
TICKER_MAP = {
    "MES":  "MES=F",
    "MNQ":  "MNQ=F",
    "ES":   "ES=F",
    "NQ":   "NQ=F",
    "MGC":  "MGC=F",
}

# ─── yfinance interval mapping ──────────────────────────────────
YF_INTERVAL = {
    "5s":  "1m",   # yfinance min is 1m; for sub-minute you need a broker
    "15s": "1m",
    "30s": "1m",
    "1m":  "1m",
    "5m":  "5m",
}

TF_SUFFIX = {
    "5s":  "5sec",
    "15s": "15sec",
    "30s": "30sec",
    "1m":  "1min",
    "5m":  "5min",
}

def download_and_save(symbol: str, timeframe: str, out_dir: str = "./data"):
    """Download OHLCV data and save as backtester-ready CSV."""
    ticker = TICKER_MAP.get(symbol.upper())
    if not ticker:
        print(f"[WARN] Unknown symbol: {symbol}")
        return

    yf_interval = YF_INTERVAL.get(timeframe, "1m")
    suffix      = TF_SUFFIX.get(timeframe, timeframe)

    print(f"[DOWN] {symbol} @ {timeframe}  (yf interval={yf_interval})")

    try:
        df = yf.download(
            ticker,
            period="60d",
            interval=yf_interval,
            auto_adjust=True,
            progress=False,
        )
    except Exception as e:
        print(f"[ERR]  Download failed: {e}")
        return

    if df.empty:
        print(f"[WARN] No data returned for {ticker}")
        return

    # Flatten multi-level columns if present
    if isinstance(df.columns, pd.MultiIndex):
        df.columns = df.columns.get_level_values(0)

    df = df[["Open", "High", "Low", "Close", "Volume"]].dropna()
    df.index = pd.to_datetime(df.index)

    # Resample to requested timeframe (1m data → 5s/15s/30s via aggregation)
    resample_map = {"5s": "5s", "15s": "15s", "30s": "30s"}
    if timeframe in resample_map and yf_interval == "1m":
        print(f"  [NOTE] yfinance min granularity is 1m; "
              f"cannot resample DOWN to {timeframe}.\n"
              f"         Saving as 1m data instead.")

    # Convert index to Unix milliseconds
    df["timestamp"] = df.index.view("int64") // 1_000_000  # ns → ms

    out = df[["timestamp", "Open", "High", "Low", "Close", "Volume"]].copy()
    out.columns = ["timestamp", "open", "high", "low", "close", "volume"]

    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, f"{symbol}_{suffix}.csv")
    out.to_csv(out_path, index=False)
    print(f"[SAVE] {out_path}  ({len(out)} bars)")


def main():
    parser = argparse.ArgumentParser(description="Download futures data for backtester")
    parser.add_argument("--symbol",    default="MES",   help="MES, MNQ, ES, NQ, MGC")
    parser.add_argument("--timeframe", default="1m",    help="5s, 15s, 30s, 1m, 5m")
    parser.add_argument("--outdir",    default="./data", help="Output directory")
    parser.add_argument("--all",       action="store_true", help="Download all symbol×TF combos")
    args = parser.parse_args()

    symbols     = list(TICKER_MAP.keys())
    timeframes  = ["1m", "5m"]   # yfinance practical options

    if args.all:
        for sym in symbols:
            for tf in timeframes:
                download_and_save(sym, tf, args.outdir)
    else:
        download_and_save(args.symbol, args.timeframe, args.outdir)

    print("\n[DONE]")
    print(f"Data saved to: {os.path.abspath(args.outdir)}")
    print("\nNext steps:")
    print("  g++ -O2 -std=c++17 -o backtester ema21_flip_backtest.cpp")
    print("  ./backtester ./data")


if __name__ == "__main__":
    main()
