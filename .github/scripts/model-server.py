"""A stand-in for a local model server, for CI: answers OpenAI chat completions by streaming one
reply a word at a time, and lets pages from any site call it, as a server for web games must."""

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPLY = ["Honey", " cakes,", " still", " warm."]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def allow_any_site(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")

    def do_OPTIONS(self):
        self.send_response(204)
        self.allow_any_site()
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        self.rfile.read(int(self.headers["Content-Length"]))
        events = ""
        for word in REPLY:
            chunk = {"choices": [{"index": 0, "delta": {"content": word}}]}
            events += "data: " + json.dumps(chunk) + "\n\n"
        last = {
            "model": "stand-in",
            "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
            "usage": {"prompt_tokens": 5, "completion_tokens": len(REPLY)},
        }
        events += "data: " + json.dumps(last) + "\n\ndata: [DONE]\n\n"
        body = events.encode()
        self.send_response(200)
        self.allow_any_site()
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
