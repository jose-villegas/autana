#!/usr/bin/env python3
"""docs_search.py as a Model Context Protocol server, over stdio.

    python scripts/docs/docs_mcp.py

Three tools: docs_search (a question, answered by the best sections' excerpts
and where to read on), docs_section (one section whole) and docs_outline (a
document's headings with their sizes). The index is built once and rebuilt
when a document changes, so an edit made mid-session is searchable at once.
"""
import json
import sys
from pathlib import Path

import docs_search

PROTOCOL = "2025-06-18"
TOOLS = [
    {"name": "docs_search",
     "description": "Search this repository's documentation. Returns the few sections that "
                    "answer the question, excerpted, each with a path:line range to read "
                    "further and the code it cites. Ask one focused question per call.",
     "inputSchema": {"type": "object", "required": ["question"], "properties": {
         "question": {"type": "string"},
         "top": {"type": "integer", "description": "sections excerpted (default 3)"}}}},
    {"name": "docs_section",
     "description": "One documentation section whole, by PATH:LINE or PATH#heading words "
                    "(as docs_search prints). deep includes its subsections.",
     "inputSchema": {"type": "object", "required": ["ref"], "properties": {
         "ref": {"type": "string"}, "deep": {"type": "boolean"}}}},
    {"name": "docs_outline",
     "description": "A document's headings with line numbers and sizes, to pick a section "
                    "without reading the whole file.",
     "inputSchema": {"type": "object", "required": ["path"], "properties": {
         "path": {"type": "string"}}}},
]


class Server:
    def __init__(self, root):
        self.root = root
        self.index = None
        self.signature = None

    def current_index(self):
        signature = docs_search.corpus_signature(self.root)
        if self.index is None or signature != self.signature:
            self.index = docs_search.Index(self.root)
            self.signature = signature
        return self.index

    def call(self, name, arguments):
        index = self.current_index()
        if name == "docs_search":
            reply = docs_search.answer(index, arguments["question"], int(arguments.get("top", 3)))
            return docs_search.format_answer(reply), False
        if name == "docs_section":
            section = index.find_section(arguments["ref"])
            if not section:
                return f"no section {arguments['ref']}; docs_outline lists a document's", True
            return docs_search.format_section(index, section, bool(arguments.get("deep"))), False
        if name == "docs_outline":
            text = docs_search.format_outline(index, arguments["path"])
            return text or f"no document {arguments['path']}", text is None
        return f"no tool {name}", True

    def handle(self, message):
        method, ident = message.get("method"), message.get("id")
        if ident is None:
            return None
        if method == "initialize":
            result = {"protocolVersion": message.get("params", {}).get("protocolVersion", PROTOCOL),
                      "capabilities": {"tools": {}},
                      "serverInfo": {"name": "autana-docs", "version": "1"}}
        elif method == "tools/list":
            result = {"tools": TOOLS}
        elif method == "tools/call":
            params = message.get("params", {})
            try:
                text, error = self.call(params.get("name"), params.get("arguments") or {})
            except (KeyError, ValueError) as problem:
                text, error = f"bad arguments: {problem}", True
            result = {"content": [{"type": "text", "text": text}], "isError": error}
        elif method == "ping":
            result = {}
        else:
            return {"jsonrpc": "2.0", "id": ident,
                    "error": {"code": -32601, "message": f"no method {method}"}}
        return {"jsonrpc": "2.0", "id": ident, "result": result}


def serve(root, stdin=sys.stdin, stdout=sys.stdout):
    server = Server(root)
    for line in stdin:
        if not line.strip():
            continue
        try:
            reply = server.handle(json.loads(line))
        except ValueError:
            reply = {"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "parse error"}}
        if reply is not None:
            stdout.write(json.dumps(reply) + "\n")
            stdout.flush()


if __name__ == "__main__":
    sys.stdin.reconfigure(encoding="utf-8")
    sys.stdout.reconfigure(encoding="utf-8")
    serve(Path(sys.argv[1]) if len(sys.argv) > 1 else docs_search.repo_root())
