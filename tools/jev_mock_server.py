#!/usr/bin/env python3
"""Minimal stand-in for the Claude Messages API, for testing the Jev client offline.

Validates the request shape the game sends (headers, structured-output schema,
fallbacks) and answers with the first allowed option as structured JSON."""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("content-length", 0))))
        problems = []
        if self.headers.get("x-api-key") != "test-key":
            problems.append("x-api-key")
        if self.headers.get("anthropic-version") != "2023-06-01":
            problems.append("anthropic-version")
        if "server-side-fallback-2026-07-01" not in (self.headers.get("anthropic-beta") or ""):
            problems.append("beta header")
        if body.get("fallbacks") != "default":
            problems.append("fallbacks")
        fmt = body.get("output_config", {}).get("format", {})
        enum = fmt.get("schema", {}).get("properties", {}).get("choice", {}).get("enum", [])
        if fmt.get("type") != "json_schema" or not enum:
            problems.append("schema")
        if not body.get("system") or not body.get("messages"):
            problems.append("prompt")
        if problems:
            self._send(400, {"type": "error", "error": {"type": "invalid_request_error", "message": ",".join(problems)}})
            return
        answer = {"choice": enum[0], "rationale": "（模拟）她权衡之后选择了第一个可行方案。"}
        self._send(200, {
            "id": "msg_mock", "type": "message", "role": "assistant", "model": body["model"],
            "stop_reason": "end_turn",
            "content": [{"type": "text", "text": json.dumps(answer, ensure_ascii=False)}],
        })

    def _send(self, code, obj):
        data = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8765), Handler).serve_forever()
