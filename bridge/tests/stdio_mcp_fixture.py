"""A tiny stdio MCP server used by test_mcp_tools.py.

Speaks newline-delimited JSON-RPC 2.0 on stdin/stdout: initialize,
notifications/initialized, tools/list (one `echo` tool) and tools/call.
Not collected by pytest because the file name lacks the test_ prefix.
"""

import json
import sys


def reply(request: dict, result: dict) -> str:
    return json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result},
                      ensure_ascii=False)


def main() -> None:
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            request = json.loads(line)
        except json.JSONDecodeError:
            continue
        method = request.get("method")
        if method == "initialize":
            out = reply(request, {"protocolVersion": "2025-06-18",
                                  "serverInfo": {"name": "stdio-fake", "version": "1.0"}})
        elif method == "tools/list":
            out = reply(request, {"tools": [
                {"name": "echo", "description": "回声测试",
                 "inputSchema": {"type": "object",
                                 "properties": {"text": {"type": "string"}}}},
            ]})
        elif method == "tools/call":
            text = "echo:" + json.dumps(request["params"].get("arguments", {}),
                                        ensure_ascii=False)
            out = reply(request, {"content": [{"type": "text", "text": text}]})
        elif method is None:
            continue  # a reply on stdin would be malformed; ignore
        else:
            out = json.dumps({"jsonrpc": "2.0", "id": request.get("id"),
                              "error": {"code": -32601, "message": "no such method"}},
                             ensure_ascii=False)
        sys.stdout.write(out + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
