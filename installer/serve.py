# Local test server for the installer page. Adapted from nfsmw-nx-installer by StevensND (GPL-3.0).
"""Local test server for the installer page (GitHub Pages already serves these types correctly)."""
import functools
import http.server
import sys

TYPES = {'.mjs': 'text/javascript', '.js': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json'}


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, **TYPES}

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8195
    root = sys.argv[2] if len(sys.argv) > 2 else '.'
    handler = functools.partial(Handler, directory=root)
    http.server.ThreadingHTTPServer(('127.0.0.1', port), handler).serve_forever()
