#!/usr/bin/env python3
"""
NanoMatch – Synthetic Order Generator
======================================
Generates a multi-million row CSV of realistic limit order book events.

Design:
  - Mid-price follows a random walk (GBM)
  - Orders are placed around mid ± spread with realistic size distribution
  - Cancellation rate ~30%  (realistic for HFT books)
  - Market orders ~5%
  - Edge cases: crossed markets, zero qty (filtered), duplicate IDs, large orders

Output CSV columns:
  timestamp_ns, order_id, symbol, side, type, price, qty, tif
"""

import random
import math
import argparse
import csv
import sys
import time
from pathlib import Path


# ── Parameters ────────────────────────────────────────────────────────────────
PRICE_SCALE  = 10_000      # 4 decimal places (same as C++ engine)
DEFAULT_ROWS = 1_000_000
SYMBOLS      = ["AAPL", "GOOG", "MSFT", "AMZN", "NVDA"]


def gbm_price_series(S0: float, mu: float, sigma: float, dt: float, n: int) -> list:
    """Geometric Brownian Motion price path."""
    prices = [S0]
    for _ in range(n - 1):
        dW = random.gauss(0, math.sqrt(dt))
        S = prices[-1] * math.exp((mu - 0.5 * sigma**2) * dt + sigma * dW)
        prices.append(max(S, 1.0))  # floor at $1
    return prices


def to_ticks(price_usd: float) -> int:
    """Convert float USD to integer ticks."""
    return int(round(price_usd * PRICE_SCALE))


def generate(
    output_path: str,
    n_rows: int = DEFAULT_ROWS,
    symbols: list = None,
    seed: int = 42,
    verbose: bool = True
):
    random.seed(seed)
    symbols = symbols or SYMBOLS

    # Pre-generate price paths (one per symbol)
    price_paths = {}
    for sym in symbols:
        S0    = random.uniform(50.0, 500.0)
        sigma = random.uniform(0.01, 0.04)   # 1-4% daily vol
        price_paths[sym] = gbm_price_series(S0, 0.0, sigma, 1/252/6.5/390, n_rows)

    out_path = Path(output_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    t0 = time.time()
    order_id   = 1
    ts_ns      = 1_000_000_000  # start 1 second into the trading day
    active_ids = {sym: [] for sym in symbols}  # track resting order ids per symbol

    # Probability config
    P_CANCEL  = 0.28
    P_MARKET  = 0.05
    P_MODIFY  = 0.05

    with open(output_path, "w", newline="", buffering=1 << 20) as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp_ns", "order_id", "symbol", "side",
                         "type", "price", "qty", "tif"])

        row_idx = 0
        while row_idx < n_rows:
            sym_idx = row_idx % len(symbols)
            sym     = symbols[sym_idx]
            mid_px  = price_paths[sym][row_idx]

            # ── Tick size & spread ─────────────────────────────────────────
            tick      = max(0.01, mid_px * 0.0001)   # ~1bps tick
            half_sprd = tick * random.randint(1, 3)

            r = random.random()

            if r < P_CANCEL and active_ids[sym]:
                # Cancel an existing order
                victim = random.choice(active_ids[sym])
                active_ids[sym].remove(victim)
                writer.writerow([ts_ns, victim, sym, "B", "C", 0, 0, "GTC"])

            elif r < P_CANCEL + P_MARKET:
                # Market order
                side   = random.choice(["B", "S"])
                qty    = random.randint(50, 2000)
                writer.writerow([ts_ns, order_id, sym, side, "M", 0, qty, "IOC"])
                order_id += 1

            elif r < P_CANCEL + P_MARKET + P_MODIFY and active_ids[sym]:
                # Modify: cancel + new order (handled by engine as modify)
                victim = random.choice(active_ids[sym])
                active_ids[sym].remove(victim)
                writer.writerow([ts_ns, victim, sym, "B", "C", 0, 0, "GTC"])
                ts_ns += random.randint(100, 10_000)
                # New order at adjusted price
                side  = random.choice(["B", "S"])
                dpx   = half_sprd if side == "B" else -half_sprd
                price = to_ticks(max(0.01, mid_px + dpx + random.gauss(0, tick)))
                qty   = random.randint(10, 1000)
                tif   = random.choice(["GTC", "GTC", "GTC", "IOC"])  # 75% GTC
                writer.writerow([ts_ns, order_id, sym, side, "L", price, qty, tif])
                active_ids[sym].append(order_id)
                order_id += 1

            else:
                # Limit order
                side = random.choice(["B", "S"])

                # Aggressive orders (20% of limits) – cross the spread
                if random.random() < 0.20:
                    dpx = -half_sprd if side == "B" else half_sprd
                else:
                    dpx = half_sprd if side == "B" else -half_sprd

                price = to_ticks(max(0.01, mid_px + dpx + random.gauss(0, tick * 0.5)))
                # Realistic size distribution: log-normal, heavy tail
                mu_qty  = 4.5  # ln(90) ≈ 4.5
                sig_qty = 1.2
                qty     = max(1, int(random.lognormvariate(mu_qty, sig_qty)))
                qty     = min(qty, 100_000)  # cap at 100k shares

                tif = random.choice(["GTC", "GTC", "GTC", "IOC", "FOK"])

                writer.writerow([ts_ns, order_id, sym, side, "L", price, qty, tif])
                if tif == "GTC":
                    active_ids[sym].append(order_id)
                order_id += 1

            # ── Edge cases: inject periodically ───────────────────────────
            if row_idx > 0 and row_idx % 50_000 == 0:
                # Large block order (stressed matching)
                writer.writerow([ts_ns, order_id, sym, "B", "M", 0, 100_000, "IOC"])
                order_id += 1
                row_idx  += 1

            ts_ns  += random.randint(100, 100_000)  # 100ns – 100μs between events
            row_idx += 1

            if verbose and row_idx % 100_000 == 0:
                elapsed = time.time() - t0
                rate    = row_idx / elapsed
                print(f"\r  Generated {row_idx:,} / {n_rows:,} rows  "
                      f"({rate:,.0f} rows/sec)", end="", flush=True)

    if verbose:
        elapsed = time.time() - t0
        size_mb = out_path.stat().st_size / 1e6
        print(f"\n\n✓ Generated {n_rows:,} rows → {output_path} "
              f"({size_mb:.1f} MB) in {elapsed:.1f}s")


# ─────────────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="NanoMatch Synthetic Order Generator")
    parser.add_argument("-o", "--output",  default="data/orders.csv",
                        help="Output CSV path (default: data/orders.csv)")
    parser.add_argument("-n", "--rows",    type=int, default=DEFAULT_ROWS,
                        help=f"Number of rows (default: {DEFAULT_ROWS:,})")
    parser.add_argument("-s", "--symbols", nargs="+", default=SYMBOLS,
                        help="Symbol list")
    parser.add_argument("--seed",          type=int, default=42)
    parser.add_argument("-q", "--quiet",   action="store_true")
    args = parser.parse_args()

    generate(
        output_path=args.output,
        n_rows=args.rows,
        symbols=args.symbols,
        seed=args.seed,
        verbose=not args.quiet
    )
