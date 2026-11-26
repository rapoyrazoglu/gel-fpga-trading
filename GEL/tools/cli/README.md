# gel — command-line frontend for the GEL execution engine

Two ways to drive GEL:

1. **One-shot mode** — `gel <noun> <verb> [flags]` (Linux/git/kubectl style)
2. **Interactive session** — `gel connect` drops you into a REPL where
   the `gel` prefix is implied: just `accounts list`, `assign add …`,
   `ipc send-order …`, `exit`.

One Python script, no third-party dependencies, ~30 working commands.

This is the **local-mode** CLI. A sibling tool `gelctl` (next iteration)
will wrap the same commands in `ssh` for remote (AWS) operation.

## Install

```sh
# from the GEL repo root
ln -s "$PWD/tools/cli/gel" /usr/local/bin/gel        # or anywhere on $PATH
# or just:
alias gel="$PWD/tools/cli/gel"
```

Verify:
```sh
gel doctor
```

## Quick tour

```sh
gel help                        # full command tree, grouped by topic
gel status                      # daemon + config + binaries summary
gel version                     # version + git rev
gel doctor                      # binaries built? configs present? python module loadable?
```

## Interactive session

```sh
gel connect                     # auto-starts the daemon, drops into REPL
gel connect --no-start-daemon   # REPL without auto-spawning the daemon
gel disconnect                  # stops the daemon (also `disconnect` from inside)
```

Inside the REPL:

```
$ gel connect
gel — interactive session
commands work without the 'gel' prefix:  accounts list / assign add ...
type 'help' for the command tree, 'status' for daemon state,
     'exit' (or ctrl-d) to leave
gel(pid 20128) > status
gel(pid 20128) > accounts list
gel(pid 20128) > ipc send-order --strategy 1 --symbol 1 --side BUY --qty 100 --account 16
gel(pid 20128) > !ls /tmp/gel-*.shm                # ! escapes to the shell
gel(pid 20128) > exit
```

REPL specifics:

- Prompt shows daemon state: `gel(pid N)` (running, green) vs `gel(off)` (dim).
- `--json`/`--quiet`/`--no-color` flags inside the REPL apply to that
  command only — the session's defaults snap back afterwards.
- Tab-completes the top-level commands; arrow keys and `^R` work via
  `readline`. History persists at `~/.gel_history`.
- `!<cmd>` runs `<cmd>` in a sub-shell. `clear` clears the screen.
- `<cmd> --help` works on any command; argparse's `SystemExit` is caught
  so the REPL stays alive.

## Configuration commands

State lives in three JSON files:

| File | Purpose |
|---|---|
| `connector/tools/account_admin/accounts.json` | strategies, accounts, assignments — also written by the admin web UI |
| `GEL/config/symbols.json` | ticker registry (ticker, venue, tick size, lot) |
| `GEL/config/risk.json` | per-strategy risk limits |

```sh
# accounts
gel accounts list
gel accounts add    --id 17 --label "OANDA-PROP" --venue OANDA --capital 250000 --live
gel accounts show   --id 17
gel accounts remove --id 17

# strategies (PCTA=1, MARKET_MAKER=2 are pre-seeded)
gel strategies list
gel strategies add    --id 3 --name SCALPER
gel strategies remove --id 3

# strategy ↔ account assignment (N:M)
gel assign add    --strategy 1 --account 17        # PCTA on OANDA-PROP
gel assign add    --strategy 1 --account 16        # PCTA also on OANDA-PAPER (1:N)
gel assign add    --strategy 2 --account 16        # MM shares OANDA-PAPER (N:1)
gel assign list
gel assign show   --strategy 1                     # which accounts is PCTA on?
gel assign show   --account  16                    # which strategies on this account?
gel assign remove --strategy 1 --account 16

# symbols
gel symbols add    --ticker EURUSD --venue OANDA --tick-size 1 --lot 1
gel symbols add    --ticker AAPL   --venue NASDAQ --tick-size 100
gel symbols list
gel symbols remove --ticker AAPL

# risk limits per strategy
gel risk show
gel risk show --strategy 1
gel risk set  --strategy 1 --max-position 50000 --max-order-qty 5000 \
              --fat-finger 0.10 --rate 1000

# whole config bundle
gel config show           # human-readable summary
gel config validate       # schema check
gel config path           # print file paths
gel config edit           # open the account-admin web UI
```

## Build

```sh
gel build                                    # configure + build all (Release)
gel build --target gel_integrated_demo       # one target
gel build --target connector_parity_test
gel build --debug                            # Debug build
gel build clean                              # rm -rf build/
```

## Run / verify

```sh
gel demo               # ./build/gel_integrated_demo  (PCTA + MM end-to-end)
gel parity             # connector_parity_test  (CPU vs HLS-shim, 100K orders)
gel shm-test           # connector_shm_roundtrip_test  (cross-process round-trip)
```

## Cross-process IPC

The local fill-echo daemon (`gel_daemon`) acts like a tiny stand-in
engine — every order it receives over SHM gets a synthetic FILLED
ExecReport in return. Useful for end-to-end strategy testing without
a real broker.

```sh
# run a daemon in the background
gel daemon start
gel daemon status
gel daemon stop

# explicitly create the SHM ring files (without starting a daemon)
gel ipc create --orders /tmp/orders.shm --reports /tmp/reports.shm

# inspect a ring's pending count
gel ipc status

# live-tail incoming ExecReports (ctrl-c to stop, or --count N)
gel ipc tap --count 100

# send a single order (waits up to 2s for the fill by default)
gel ipc send-order --strategy 1 --symbol 1 --side BUY --qty 100 \
                   --account 16
gel ipc send-order --strategy 2 --symbol 1 --side SELL --qty 250 \
                   --type LIMIT --price 1.0750 --account 16 --wait 5

# cancel an open order
gel ipc cancel  --strategy 1 --coid 7001 --account 16

# replace an open order (atomic price/qty change)
gel ipc replace --strategy 1 --coid 7001 --new-coid 7002 \
                --symbol 1 --side BUY --qty 200 --price 1.0760 \
                --account 16
```

End-to-end smoke test in one terminal:

```sh
gel daemon start
gel ipc send-order --strategy 1 --symbol 1 --side BUY --qty 100 --account 16
gel ipc send-order --strategy 2 --symbol 1 --side SELL --qty 250 --account 16
gel daemon stop
```

## Tools

```sh
gel admin              # opens the account-admin web UI (http://127.0.0.1:8765)
gel python             # Python REPL with `connector` pre-imported as `c`
gel doctor             # health check
gel backtest           # (stub — not implemented yet)
```

## Global flags

| Flag | Effect |
|---|---|
| `--json`     | machine-readable JSON output (for `jq`, scripts, etc.) |
| `--quiet`    | suppress non-essential prints |
| `--no-color` | disable ANSI colors (also via `NO_COLOR=1`) |

JSON example:
```sh
gel --json accounts list | jq '.[] | select(.live == "LIVE")'
gel --json daemon status | jq '.running'
```

## Environment variables

| Var | Default | Effect |
|---|---|---|
| `GEL_BUILD`     | `<GEL>/build`                                             | build dir |
| `GEL_CONFIG`    | `<GEL>/config`                                            | config dir |
| `GEL_ACCOUNTS`  | `<connector>/tools/account_admin/accounts.json`           | accounts file |
| `GEL_SYMBOLS`   | `<GEL>/config/symbols.json`                               | symbols file |
| `GEL_RISK`      | `<GEL>/config/risk.json`                                  | risk file |
| `GEL_PIDFILE`   | `$XDG_RUNTIME_DIR/gel-daemon.pid`, else `~/.gel/`         | daemon pidfile |
| `GEL_LOGFILE`   | `$XDG_RUNTIME_DIR/gel-daemon.log`, else `~/.gel/`         | daemon log |
| `GEL_HISTORY`   | `~/.gel_history`                                          | REPL history file |
| `NO_COLOR`      | (unset)                                                   | disable colors |

## Exit codes

| Code | Meaning |
|------|---|
| 0    | success |
| 1    | error |
| 2    | usage error (bad flags) |
| 64   | not found (missing binary, missing record, missing config) |
| 65   | not implemented yet (e.g. `gel backtest`) |
| 130  | interrupted by ctrl-c |

## Implementation notes

- Single Python 3.7+ file, ~900 lines. No third-party deps; uses
  `connector` Python module for IPC commands.
- Subcommands defined via `argparse`. Shell-completion can be wired up
  via `argcomplete` if needed later.
- All state is persisted to JSON files; commits are atomic
  (write `.tmp`, rename).
- The daemon is managed via a PID file; `start` is idempotent-blocking
  ("already running"), `stop` waits ≤2.5s for graceful exit then
  SIGKILLs.
- IPC commands import the pybind11 module; if it isn't built yet, the
  CLI suggests `gel build --target _connector_native`.

## What's coming next

- **`gelctl`**: same command surface, but each invocation wraps `ssh`
  to a host alias (e.g. `gelctl --host gel-prod accounts list`). Auth
  via standard `~/.ssh/config` + `.pem`. Static EC2 IP, no embedded
  credentials.
- **`gel telemetry`**: live PnL, latency histograms, per-cell fills.
  Needs a control endpoint on the daemon.
- **`gel backtest`**: historical replay across strategies. Needs a
  backtest engine in C++.
