#!/usr/bin/env python3
"""Serve the WebGPU demo with the isolation headers required by threaded Wasm."""
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] / "build" / "webgpu-renderer-demo"
ISOLATION_HEADERS = True


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def end_headers(self):
        if ISOLATION_HEADERS:
            self.send_header("Cross-Origin-Opener-Policy", "same-origin")
            self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--without-isolation",
        action="store_true",
        help="simulate static hosting without COOP/COEP headers for a local smoke check",
    )
    arguments = parser.parse_args()
    if not (ROOT / "index.html").exists():
        raise SystemExit("Build first with: make webgpu-renderer-demo")
    port = 8789 if arguments.without_isolation else 8788
    ISOLATION_HEADERS = not arguments.without_isolation
    print(f"Open http://127.0.0.1:{port}")
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
