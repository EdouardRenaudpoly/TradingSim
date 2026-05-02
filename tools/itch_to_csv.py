#!/usr/bin/env python3
"""
Convert NASDAQ ITCH v2 text format to TradingSim CSV format.

Input: fixed-width ASCII ITCH v2 (e.g. S010303-v2.txt from emi.nasdaq.com/ITCH)
Output: timestamp,symbol,price,quantity,side,trader_id

A-message layout (42 chars, 0-indexed):
  [0:8]  timestamp   nanoseconds from midnight
  [8]    type        'A' = add order
  [9:18] order_ref   unique order ID (used as trader_id)
  [18]   side        'B' = BUY, 'S' = SELL
  [19:25] shares     quantity
  [25:33] stock      symbol, space-padded on the right
  [33:41] price      integer, divide by 10000 for dollars
  [41]   attribution 'Y'/'N'

Usage:
  python3 tools/itch_to_csv.py S010303-v2.txt data/itch.csv
  python3 tools/itch_to_csv.py S010303-v2.txt data/itch.csv --max 100000
  python3 tools/itch_to_csv.py S010303-v2.txt data/itch.csv --symbols AAPL MSFT
"""
import argparse


def convert(input_path: str, output_path: str,
            max_orders: int | None = None,
            symbol_filter: set | None = None,
            min_price: float = 0.01,
            max_price: float = 499.99) -> int:
    count = 0
    with open(input_path, "r") as fin, open(output_path, "w") as fout:
        fout.write("timestamp,symbol,price,quantity,side,trader_id\n")
        for line in fin:
            if len(line) < 42 or line[8] != "A":
                continue

            stock = line[25:33].rstrip()
            if symbol_filter and stock not in symbol_filter:
                continue

            ts         = line[0:8].lstrip("0") or "0"
            order_ref  = line[9:18].strip()
            side       = "BUY" if line[18] == "B" else "SELL"
            shares_str = line[19:25].strip()
            price_str  = line[33:41].strip()

            if not shares_str or not price_str:
                continue

            price = int(price_str) / 10000.0
            if not (min_price <= price <= max_price):
                continue

            fout.write(f"{ts},{stock},{price:.4f},{shares_str},{side},{order_ref}\n")
            count += 1

            if max_orders and count >= max_orders:
                break

    return count


def main():
    p = argparse.ArgumentParser(description="NASDAQ ITCH v2 → TradingSim CSV")
    p.add_argument("input",           help="ITCH v2 text file (e.g. S010303-v2.txt)")
    p.add_argument("output",          help="Output CSV path")
    p.add_argument("--max",       type=int,   default=None,  help="Max orders to extract")
    p.add_argument("--symbols",   nargs="+",  default=None,  help="Only include these symbols")
    p.add_argument("--min-price", type=float, default=0.01,  help="Min price filter (default 0.01)")
    p.add_argument("--max-price", type=float, default=499.99,help="Max price filter (default 499.99)")
    args = p.parse_args()

    symbol_filter = set(args.symbols) if args.symbols else None
    n = convert(args.input, args.output, args.max, symbol_filter, args.min_price, args.max_price)
    print(f"Extracted {n:,} orders → {args.output}")


if __name__ == "__main__":
    main()
