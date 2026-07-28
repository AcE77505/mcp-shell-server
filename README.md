# MCP Shell Server

轻量级 [Model Context Protocol](https://modelcontextprotocol.io/) 服务器，通过 JSON-RPC 2.0 over HTTP 执行 shell 命令。

**单文件静态 ARM64 二进制 — 零外部依赖，直接运行。**

简体中文 | [English](./README.en.md)

---

## 特性

- 🔧 **两个工具**: `shell` (执行命令) + `reset` (复位服务端状态)
- 📦 **静态编译**: ~760KB，无需 libc/libstdc++/任何运行时
- 🚀 **零配置**: `./mcp-shell-server` 直接启动，默认端口 8080
- 🌐 **局域网可用**: 默认监听 `0.0.0.0`
- 🔓 **无命令限制**: 所有 shell 命令默认放行
- ⚡ **JSON-RPC 2.0** 基于 MCP Streamable HTTP 传输

---

## 快速开始

```bash
# 启动服务（默认端口 8080）
./mcp-shell-server

# 指定端口
./mcp-shell-server -p 8080

# 查看帮助
./mcp-shell-server -h
```

启动后日志输出：
```
[I] ============================================
[I]   MCP Shell Server v1.0.0
[I]   Listen: 0.0.0.0:8080
[I]   Endpoint: POST /mcp
[I]   Protocol: MCP 2025-11-25
[I] ============================================
[I]   Connect: http://<ip>:8080/mcp
```

---

## 编译方式

### ARM64 Linux (Ubuntu/Debian)

```bash
# 安装编译工具链
apt install g++ libc6-dev

# 静态编译
g++ -std=c++11 -O2 -static -o mcp-shell-server mcp-shell-server.cpp -lpthread

# 瘦身（可选）
strip mcp-shell-server
```

### ARM64 Android (Termux)

```bash
# 安装编译工具链
pkg install build-essential

# 静态编译
g++ -std=c++11 -O2 -static -o mcp-shell-server mcp-shell-server.cpp -lpthread

# 瘦身（可选）
strip mcp-shell-server
```

### ARM64 macOS

```bash
# 安装 LLVM
brew install llvm

# 编译（macOS 默认不支持 -static，会编译为动态链接）
clang++ -std=c++11 -O2 -o mcp-shell-server mcp-shell-server.cpp
```

> **注意**: macOS 上无法静态链接 libc，编译产物为动态链接。

### 交叉编译（x86_64 → ARM64）

```bash
# 安装交叉编译工具链
apt install g++-aarch64-linux-gnu

# 静态交叉编译
aarch64-linux-gnu-g++ -std=c++11 -O2 -static -o mcp-shell-server mcp-shell-server.cpp -lpthread
```

### 编译选项说明

| 选项 | 说明 |
|------|------|
| `-std=c++11` | C++11 标准 |
| `-O2` | 二级优化 |
| `-static` | 静态链接，生成独立二进制 |
| `-lpthread` | 链接 pthread（静态编译时必需） |
| `strip` | 去除调试符号，减小体积（760KB→可选） |

---

## 协议

服务端实现 **MCP 2025-11-25** 规范，使用 **Streamable HTTP** 传输。

### 端点

`POST /mcp`

### 工具

#### `shell` — 执行 shell 命令

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `command` | string | ✅ | 要执行的 shell 命令 |
| `timeout` | number | ❌ | 超时秒数（默认 30） |

**请求示例：**

```json
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
```

**响应示例：**

```json
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

`text` 字段内嵌 JSON，包含以下字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| `stdout` | string | 标准输出 |
| `stderr` | string | 错误输出 |
| `exit_code` | int | 退出码（-1 表示超时） |
| `success` | bool | 是否成功（exit_code == 0） |

#### `reset` — 复位服务端

复位 `initialized` 标志。无参数。

复位后需重新发送 `initialize` 请求才能继续使用工具。

---

## 快速测试

```bash
# 1. 初始化
curl -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"1","method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}'

# 2. 列出工具
curl -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"2","method":"tools/list","params":{}}'

# 3. 执行命令
curl -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"3","method":"tools/call","params":{"name":"shell","arguments":{"command":"echo Hello MCP"}}}'

# 4. 复位
curl -X POST http://localhost:8080/mcp \
  -H "Content-Type: application/json" \
  -d '{"jsonrpc":"2.0","id":"4","method":"tools/call","params":{"name":"reset","arguments":{}}}'
```

---

## 架构

```
┌──────────────┐     HTTP POST /mcp      ┌──────────────────┐
│  MCP 客户端  │ ──────────────────────▶  │  mcp-shell-server │
│  (Claude,    │     JSON-RPC 2.0        │  (ARM64 静态)     │
│   Cursor,    │ ◀────────────────────── │  端口 8080        │
│   自定义)    │     JSON-RPC 响应       └──────────────────┘
└──────────────┘
```

服务端处理流程：

1. 接收一个 HTTP POST 请求
2. 解析 JSON-RPC 2.0 消息
3. 路由到对应处理器（`initialize` / `tools/list` / `tools/call`）
4. 通过 `fork()` + `exec()` 执行 shell 命令，pipe 捕获输出
5. 返回 JSON-RPC 响应，关闭连接

**零外部 JSON 库** — JSON 构造器和解析器全部自实现，~800 行 C++。

### 文件结构

```
mcp-shell-server/
├── mcp-shell-server.cpp    # 完整源码（单文件）
├── mcp-shell-server        # 预编译 ARM64 静态二进制（需自行编译）
├── README.md               # 本文档
└── README.en.md            # English version（如有）
```

---

## License

MIT
