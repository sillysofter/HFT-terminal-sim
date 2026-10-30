# hft-sim

A high-frequency paper-trading simulator in C. It streams **live Binance
top-of-book data** and runs several independent trading profiles against it.
Each profile has its own starting cash, starting asset holdings, strategy and
parameters. An ncurses dashboard shows every profile's wins, losses, rejected
orders and P&L as they happen.

No real orders are sent. Fills are simulated against the live bid/ask.

## Build & run

```sh
make            # uses system libcurl/ncurses dev packages if installed
./hft-sim                       # dashboard, reads ./profiles.conf
./hft-sim my.conf               # another config
./hft-sim --headless 60         # no UI: run 60 s, print a summary
```

If `make` can't find curl/ncurses headers, either
`sudo apt install libcurl4-openssl-dev libncurses-dev` or run `make deps`,
which unpacks the headers into `third_party/` without root.

Needs a terminal of at least 110×28 (150×45 or larger is best).

### Keys

| key | action |
|---|---|
| ↑ / ↓ (j / k) | select profile |
| space | pause/resume new entries for the selected profile (exits still run) |
| a | pause/resume all profiles |
| f | flatten (close) the selected profile's open positions |
| n | create a new profile (asks for name, strategy, cash, holdings; saved to the config) |
| q | quit and print a session summary |

## Profiles

See the comments in `profiles.conf` for every key. Briefly:

```ini
[profile Alice]
strategy  = imbalance     # momentum | meanrev | imbalance
cash      = 10000         # starting USDT
hold.BTC  = 0.05          # starting holdings
hold.ETH  = 1.5
order_usd = 1000
fee_bps   = 10
```

* **Cash** pays for long entries (buy, then sell later).
* **Holdings** pay for short entries against inventory (sell, then buy back
  later). Spot only: there's no borrowing, so a profile with no BTC can't
  sell BTC and that order is shown as **REJECTED**.

## How results are scored

* Buys fill at the live **ask** and sells at the live **bid**. Fill size is
  capped at the quantity shown at that price, and `fee_bps` is charged on
  each side.
* Each entry→exit is one **round trip**. Exits happen on take-profit,
  stop-loss, signal reversal, timeout or manual flatten. A round trip is a
  **WIN** if its net P&L after fees is positive, otherwise a **LOSS**.
* **Strategy P&L** is realized plus open round-trip P&L: what the strategy
  itself made. **Δ Equity** also includes market moves on the holdings the
  profile started with.
* Every trade and reject is appended to `logs/<profile>.csv`.

## Limits

This is "HFT" in its logic, not in its speed. The feed is REST polling of
Binance's public `bookTicker` endpoint (default every 250 ms; the
round-trip from the UK is about 300 ms). Real HFT uses co-located servers,
exchange WebSocket/FIX feeds and microsecond latency. The dashboard's
"Decision" latency is only the in-process strategy time. With Binance's
standard 10 bp taker fee, most short-horizon round trips lose money; that is
a real result, not a bug. Set `fee_bps` lower to model maker or VIP fee tiers.
