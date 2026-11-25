#!/usr/bin/env python3
"""Read-only Codex usage bridge. The Codex CLI remains owner of auth.json."""
import argparse
import base64
import json
import logging
import os
import random
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

URL = "https://chatgpt.com/backend-api/wham/usage"
LOG = logging.getLogger("codex-usage-relay")


def jwt_exp(token):
    try:
        part = token.split(".")[1]
        return int(json.loads(base64.urlsafe_b64decode(part + "=" * (-len(part) % 4)))["exp"])
    except (IndexError, ValueError, KeyError):
        return None


def read_auth(path):
    # Atomic replacement by the CLI is safe to read. Never write its file.
    data = json.loads(path.read_text())
    tokens = data.get("tokens") or {}
    access = tokens.get("access_token")
    if not access:
        raise ValueError("Codex ChatGPT access token missing")
    exp = jwt_exp(access)
    if exp is not None and exp <= time.time() + 60:
        raise ValueError("Codex ChatGPT access token expired; refresh with codex login")
    return access, tokens.get("account_id")


def window_payload(body, now=None):
    now = time.time() if now is None else now
    root = body.get("rate_limit") or {}
    windows = {}
    for name in ("primary_window", "secondary_window"):
        item = root.get(name)
        if not isinstance(item, dict):
            continue
        duration = item.get("limit_window_seconds")
        used = item.get("used_percent")
        if not isinstance(duration, int) or not isinstance(used, (int, float)):
            continue
        kind = "session" if 14400 <= duration <= 21600 else "week" if 518400 <= duration <= 691200 else None
        if kind is None:
            continue
        reset = item.get("reset_at")
        if not isinstance(reset, (int, float)):
            after = item.get("reset_after_seconds")
            reset = now + after if isinstance(after, (int, float)) else None
        mins = max(0, int((reset - now + 30) // 60)) if reset is not None else -1
        windows[kind] = (max(0, round(used)), mins)
    if not windows:
        raise ValueError("usage answer lacks an account session/week window")
    s, sr = windows.get("session", (0, -1))
    w, wr = windows.get("week", (0, -1))
    limited = bool(root.get("limit_reached")) or root.get("allowed") is False
    result = {"s": s, "sr": sr, "w": w, "wr": wr,
              "st": "limited" if limited else "allowed",
              "acct": "pro" if "session" in windows else "week", "pv": "codex", "ok": True}
    credits = body.get("rate_limit_reset_credits") or {}
    count = credits.get("available_count")
    if isinstance(count, int) and count >= 0:
        result["rc"] = min(count, 999)
    return result


class Cache:
    def __init__(self, auth_path, interval):
        self.auth_path, self.interval = auth_path, interval
        self.lock = threading.Lock()
        self.payload = None
        self.updated = 0
        self.failure = "starting"

    def poll_forever(self):
        backoff = self.interval
        while True:
            wait = self.interval
            try:
                access, account = read_auth(self.auth_path)
                headers = {"Authorization": "Bearer " + access, "Accept": "application/json"}
                if account:
                    headers["ChatGPT-Account-Id"] = account
                req = urllib.request.Request(URL, headers=headers)
                with urllib.request.urlopen(req, timeout=30) as response:
                    payload = window_payload(json.load(response))
                with self.lock:
                    self.payload, self.updated, self.failure = payload, time.time(), ""
                backoff = self.interval
            except urllib.error.HTTPError as exc:
                retry = exc.headers.get("Retry-After", "") if exc.code == 429 else ""
                if exc.code == 429:
                    backoff = min(max(backoff * 2, int(retry) if retry.isdigit() else 0), 3600)
                    wait = backoff
                with self.lock:
                    self.failure = f"upstream HTTP {exc.code}"
                LOG.warning(self.failure)
            except (OSError, ValueError, urllib.error.URLError) as exc:
                with self.lock:
                    self.failure = str(exc)
                LOG.warning("poll failed: %s", exc)
            time.sleep(max(1, wait + random.uniform(-min(10, wait * .1), min(10, wait * .1))))


def serve(cache, host, port):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != "/usage":
                self.send_error(404)
                return
            with cache.lock:
                payload, updated = cache.payload, cache.updated
            if payload is None or time.time() - updated > 600:
                self.send_error(503, "No fresh Codex usage")
                return
            body = json.dumps(payload, separators=(",", ":")).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    ThreadingHTTPServer((host, port), Handler).serve_forever()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--auth", type=Path, default=Path.home() / ".codex/auth.json")
    parser.add_argument("--host", default="127.0.0.1", help="Bind to the VM's Tailscale IPv4")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--interval", type=int, default=120)
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO)
    cache = Cache(args.auth, args.interval)
    threading.Thread(target=cache.poll_forever, daemon=True).start()
    serve(cache, args.host, args.port)


if __name__ == "__main__":
    main()
