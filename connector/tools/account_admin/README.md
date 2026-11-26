# account-admin

Visual editor for the strategy ↔ account graph that
`connector::StrategyAccountMap` consumes.

## Run

```sh
python3 tools/account_admin/server.py            # serves on 127.0.0.1:8765
python3 tools/account_admin/server.py --port 9000
```

Open `http://localhost:8765/` in a browser. No third-party Python deps —
the server uses only stdlib (`http.server`). The page loads
`vis-network` from a CDN.

## What you get

- Bipartite graph: strategy boxes (orange) ←→ account ellipses
  (blue = paper, green = live)
- Side panel to add/remove strategies, accounts, and assignments
- Save button writes back to `accounts.json` (atomic file rename)

## State file

`accounts.json` lives next to `server.py` and looks like:

```json
{
  "strategies": [
    {"id": 1, "name": "PCTA"},
    {"id": 2, "name": "MARKET_MAKER"}
  ],
  "accounts": [
    {"id": 16, "label": "OANDA-PRIMARY", "venue": "OANDA",
     "capital_units": 100000, "live": false}
  ],
  "assignments": [
    {"strategy_id": 1, "account_id": 16}
  ]
}
```

Strategy ids match the `connector::StrategyId` enum
(1 = PCTA, 2 = MARKET_MAKER). Account ids must be ≠ 0
(0 is reserved for `ACCOUNT_INVALID` / `ACCOUNT_DEFAULT`).

## How the engine consumes it

GEL or any other engine reads `accounts.json` at startup, then:

```cpp
#include <connector/account_map.hpp>
#include <nlohmann/json.hpp>
#include <fstream>

connector::StrategyAccountMap load_account_map(const std::string& path) {
    nlohmann::json j;
    std::ifstream(path) >> j;

    connector::StrategyAccountMap m;
    for (const auto& a : j["accounts"]) {
        m.add_account({
            .id            = a["id"],
            .label         = a["label"],
            .venue         = a.value("venue", ""),
            .capital_units = a.value("capital_units", 0LL),
            .live          = a.value("live", false),
        });
    }
    for (const auto& asn : j["assignments"]) {
        m.assign(static_cast<connector::StrategyId>(asn["strategy_id"]),
                 asn["account_id"]);
    }
    return m;
}
```

The `connector::StrategyAccountMap` lookups (`primary`, `accounts_of`,
`stamp_primary`) are then the canonical source for routing decisions
upstream of `gel::platform::AccountRouter`.
