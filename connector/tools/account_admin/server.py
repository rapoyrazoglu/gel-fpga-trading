"""Account admin web server.

Tiny stdlib-only HTTP server that lets you visually manage the
strategy ↔ account mapping. The state is persisted to ``accounts.json``
in this directory; the engine reads the same file at startup to
populate ``connector::StrategyAccountMap``.

Run::

    python3 tools/account_admin/server.py            # default :8765
    python3 tools/account_admin/server.py --port 9000

Then open http://localhost:8765 in a browser.

No third-party dependencies — only Python 3 stdlib.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


HERE       = Path(__file__).resolve().parent
STATE_PATH = HERE / "accounts.json"
STATIC_DIR = HERE / "static"

# Default seed if accounts.json doesn't exist yet. Strategy IDs match
# the connector::StrategyId enum.
SEED_STATE: dict[str, Any] = {
    "strategies": [
        {"id": 1, "name": "PCTA"},
        {"id": 2, "name": "MARKET_MAKER"},
    ],
    "accounts": [
        {
            "id": 16,
            "label": "OANDA-PRIMARY",
            "venue": "OANDA",
            "capital_units": 100_000,
            "live": False,
        },
    ],
    "assignments": [
        {"strategy_id": 1, "account_id": 16},
    ],
}


_LOCK = threading.Lock()


def _load_state() -> dict[str, Any]:
    if not STATE_PATH.exists():
        STATE_PATH.write_text(json.dumps(SEED_STATE, indent=2))
        return json.loads(json.dumps(SEED_STATE))
    return json.loads(STATE_PATH.read_text())


def _save_state(state: dict[str, Any]) -> None:
    tmp = STATE_PATH.with_suffix(".json.tmp")
    tmp.write_text(json.dumps(state, indent=2))
    os.replace(tmp, STATE_PATH)


def _validate(state: dict[str, Any]) -> str | None:
    """Return an error message, or None if state is well-formed."""
    if not isinstance(state, dict):
        return "state must be a JSON object"
    for key in ("strategies", "accounts", "assignments"):
        if key not in state or not isinstance(state[key], list):
            return f"missing or non-list field: {key}"

    def is_int(x: Any) -> bool:
        return isinstance(x, int) and not isinstance(x, bool)

    s_ids: set[int] = set()
    for s in state["strategies"]:
        if not isinstance(s, dict) or "id" not in s or "name" not in s:
            return "strategy missing id/name"
        if not is_int(s["id"]) or not isinstance(s["name"], str):
            return "strategy id must be an integer and name a string"
        if s["id"] in s_ids:
            return f"duplicate strategy id: {s['id']}"
        s_ids.add(s["id"])

    a_ids: set[int] = set()
    for a in state["accounts"]:
        if not isinstance(a, dict) or "id" not in a or "label" not in a:
            return "account missing id/label"
        if not is_int(a["id"]) or not isinstance(a["label"], str):
            return "account id must be an integer and label a string"
        if not isinstance(a.get("venue", ""), str):
            return "account venue must be a string"
        if not isinstance(a.get("capital_units", 0), (int, float)) or isinstance(a.get("capital_units", 0), bool):
            return "account capital_units must be a number"
        if a["id"] == 0:
            return "account id 0 is reserved (ACCOUNT_INVALID)"
        if a["id"] in a_ids:
            return f"duplicate account id: {a['id']}"
        a_ids.add(a["id"])

    seen_pairs: set[tuple[int, int]] = set()
    for asn in state["assignments"]:
        if not isinstance(asn, dict):
            return "assignment must be an object"
        sid = asn.get("strategy_id")
        aid = asn.get("account_id")
        if sid is None or aid is None:
            return "assignment missing strategy_id/account_id"
        if sid not in s_ids:
            return f"assignment references unknown strategy {sid}"
        if aid not in a_ids:
            return f"assignment references unknown account {aid}"
        if (sid, aid) in seen_pairs:
            return f"duplicate assignment ({sid}, {aid})"
        seen_pairs.add((sid, aid))

    return None


class Handler(BaseHTTPRequestHandler):
    # Keep stdout quiet for normal traffic; only log errors.
    def log_message(self, fmt: str, *args: Any) -> None:
        if "200" not in (args[1] if len(args) > 1 else ""):
            super().log_message(fmt, *args)

    # -------- helpers ---------------------------------------------------
    def _send_json(self, code: int, body: Any) -> None:
        payload = json.dumps(body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type",   "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control",  "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def _send_file(self, path: Path, content_type: str) -> None:
        if not path.exists():
            self.send_error(404)
            return
        data = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type",   content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _read_body_json(self) -> Any:
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n) if n else b""
        return json.loads(raw or b"{}")

    # -------- routing ---------------------------------------------------
    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/" or self.path == "/index.html":
            self._send_file(STATIC_DIR / "index.html", "text/html; charset=utf-8")
            return
        if self.path == "/api/state":
            with _LOCK:
                self._send_json(200, _load_state())
            return
        self.send_error(404)

    def do_POST(self) -> None:  # noqa: N802
        if self.path == "/api/state":
            # Only the page this server serves may change state. A JSON
            # content type forces a CORS preflight (which we never answer), and
            # the Host check stops DNS rebinding onto the loopback port.
            host, port = self.server.server_address[:2]
            allowed = {f"{h}:{port}" for h in (host, "localhost", "127.0.0.1")}
            ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip()
            origin = self.headers.get("Origin")
            if (ctype != "application/json"
                    or self.headers.get("Host") not in allowed
                    or (origin is not None
                        and origin.split("://", 1)[-1] not in allowed)):
                self._send_json(403, {"error": "cross-origin request refused"})
                return
            try:
                body = self._read_body_json()
            except json.JSONDecodeError as e:
                self._send_json(400, {"error": f"invalid JSON: {e}"})
                return
            err = _validate(body)
            if err:
                self._send_json(400, {"error": err})
                return
            with _LOCK:
                _save_state(body)
            self._send_json(200, {"ok": True})
            return
        self.send_error(404)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()

    # Ensure seed state exists before serving so the UI has something
    # to render on first load.
    with _LOCK:
        _load_state()

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"account-admin: http://{args.host}:{args.port}/")
    print(f"               state file: {STATE_PATH}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("account-admin: shutting down.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
