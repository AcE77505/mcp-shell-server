# MCP Shell Server

A lightweight [Model Context Protocol](https://modelcontextprotocol.io/) server that executes shell commands via JSON-RPC 2.0 over HTTP.

**Single static ARM64 binary — zero external dependencies. Just run it.**

## Features

- 🔧 **Two tools**: `shell` (execute commands) + `reset` (reset server state)
- 📦 **Static binary**: ~760KB, no libc/libstdc++/any runtime dependency
- 🚀 **Zero-config**: start with `./mcp-shell-server`, runs on port 8080
- 🌐 **LAN accessible**: listens on `0.0.0.0` by default
- 🔓 **No command restrictions**: all shell commands allowed
- ⚡ **JSON-RPC 2.0** over MCP Streamable HTTP transport

## Quick Start

```bash
# Start server (default port 8080)
./mcp-shell-server

# Or specify a port
./mcp-shell-server -p 8080
```

## Build from Source

```bash
# Requirements: g++ with static libstdc++
g++ -std=c++11 -O2 -static -o mcp-shell-server mcp-shell-server.cpp -lpthread
```

Tested on:
- Ubuntu 22.04 ARM64 (native)
- Android (Termux) ARM64

## Protocol

The server implements **MCP 2025-11-25** over **Streamable HTTP** transport.

### Endpoint

`POST /mcp`

### Tools

#### `shell`

Execute any shell command.

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `command` | string | ✅ | Shell command to execute |
| `timeout` | number | ❌ | Timeout in seconds (default: 30) |

**Example:**

```json
// Request
{
  "jsonrpc": "2.0",
  "id": "req-001",
  "method": "tools/call",
  "params": {
    "name": "shell",
    "arguments": {
      "command": "echo hello; uname -a"
    }
  }
}

// Response
{
  "jsonrpc": "2.0",
  "id": "req-001",
  "result": {
    "content": [{
      "type": "text",
      "text": "{\"stdout\":\"hello\\nLinux ...\\n\",\"stderr\":\"\",\"exit_code\":0,\"success\":true}"
    }],
    "isError": false
  }
}
```

#### `reset`

Reset server state (clears initialized flag).

No parameters required.

### Quick Test

```bash
# Initialize
curl -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"1","method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}'

# List tools
curl -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"2","method":"tools/list","params":{}}'

# Execute command
curl -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"3","method":"tools/call","params":{"name":"shell","arguments":{"command":"echo Hello MCP"}}}'
```

## Architecture

```
┌──────────────┐     HTTP POST /mcp      ┌──────────────────┐
│  MCP Client  │ ──────────────────────▶  │  mcp-shell-server │
│  (Claude,    │     JSON-RPC 2.0        │  (ARM64 static)   │
│   Cursor,    │ ◀────────────────────── │  Port 8080        │
│   custom)    │     JSON-RPC Response   └──────────────────┘
└──────────────┘
```

The server:
1. Reads one HTTP POST request per connection
2. Parses JSON-RPC 2.0 message
3. Routes to handler (`initialize`, `tools/list`, `tools/call`)
4. Executes shell commands via `fork()` + `exec()` with pipe capture
5. Returns JSON-RPC response, then closes connection

No external JSON library — full JSON builder and parser are self-contained in ~800 lines of C++.

## License

MIT
