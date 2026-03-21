#!/usr/bin/env python3
"""Local dev server with COOP/COEP headers required for SharedArrayBuffer."""

import http.server
import socketserver

PORT = 8080


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory='dist', **kwargs)

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        super().end_headers()


socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(('', PORT), Handler) as server:
    print(f'Serving dist/ at http://localhost:{PORT}')
    server.serve_forever()
