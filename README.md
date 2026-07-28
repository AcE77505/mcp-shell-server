# MCP Shell Server

轻量级 MCP Shell 服务器 — JSON-RPC 2.0 / ARM64 静态 / 零依赖

## 工具

| 工具 | 说明 |
|------|------|
| `shell` | 执行命令，返回 `pid` 用于后续操作 |
| `status` | 列出所有终端，含 `mine` 标记 |
| `stop` | 停止命令（支持 `pid` 参数或仅停自己的） |
| `reset` | 复位服务端 |

## 快速开始

```bash
./mcp-shell-server
```

## 编译

```bash
g++ -std=c++11 -O2 -static -o mcp-shell-server mcp-shell-server.cpp -lpthread
```

https://github.com/AcE77505/mcp-shell-server
