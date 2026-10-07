# MCP Shell Server

轻量级 MCP Shell 服务器 — JSON-RPC 2.0 / ARM64 静态 / 零依赖

- MCP 协议 `2025-11-25`，Streamable HTTP，入口 `POST /mcp`
- 单文件 C++11，pthread 多线程，无第三方依赖

## 工具

| 工具 | 说明 |
|------|------|
| `shell` | 执行命令，返回 `pid` 用于后续操作 |
| `status` | 列出所有终端，含 `mine` 标记 |
| `stop` | 停止命令（支持 `pid` 参数或仅停自己的） |
| `reset` | 复位服务端 |

## 快速开始

```bash
./mcp-shell-server              # 监听 0.0.0.0:8080
./mcp-shell-server -p 9000      # 指定端口
./mcp-shell-server -f           # 8080 被占用时，先踢掉占用进程再启动
./mcp-shell-server -p 9000 -f   # 指定端口 + 踢掉该端口占用者
```

客户端地址：`http://<ip>:<port>/mcp`

## 参数

| 参数 | 说明 |
|------|------|
| `-p, --port PORT` | 监听端口，默认 8080（1–65535） |
| `-f, --force` | 端口被占用（EADDRINUSE）时，先踢掉占用者，再在该端口启动 |
| `-h, --help` | 用法帮助 |

退出码：`-h` 返回 0；参数非法、端口收回失败或 bind 失败返回 1；正常启动后常驻。

## 端口占用（-f）

Android 上 lsof 常常看不到占用者，`-f` 不依赖任何外部命令（不用 lsof/netstat/fuser/ss），自己找：

1. 从 `/proc/net/tcp` 与 `/proc/net/tcp6` 里找 **本地端口 == PORT 的 LISTEN socket inode**（没有 LISTEN 时退一步看其它已 bind 状态，忽略 TIME_WAIT/CLOSE）；
2. 扫描 `/proc/<pid>/fd` 中的 `socket:[inode]` 定位持有者（日志里打印 pid + cmdline）；
3. 对持有者发 **SIGTERM**，最多等约 1.5s 优雅退出，仍在则 **SIGKILL**；
4. 端口释放后重新 `bind()`，成功即正常启动。

成功时日志形如：

```
[I] port 8080 is busy, -f: killing the holder(s)
[I] SIGTERM -> pid 12345 (./mcp-shell-server)
[I] port 8080: holders=1 killed=1 skipped=0 denied=0
[I] === MCP Shell Server ===
[I] Port 8080  EP: POST /mcp
```

踢不掉时会直接说原因：

| 日志 | 含义 |
|------|------|
| `no permission to kill pid N` / `cannot signal ... permission denied, need root` | 权限不足，需要 root 运行 |
| `held by this process tree (pid N ...): nothing killed, start me from another parent` | 占用者属于本进程树，**一个进程也不会杀**（见下） |
| `cannot attribute holder pid N: nothing killed` | 读不到祖先链，保守起见不杀 |
| `no owning process is visible (permission?)` | 看不到持有者（权限或 /proc 受限） |
| `port N is still in use after -f` | 收回失败，端口仍被占用（返回 1） |

> **为什么“占用者属于本进程树就不杀”**：本服务器 fork 命令时**监听 fd（fd 3）会被每个命令子进程继承**，所以同一端口 socket 的持有者总是“服务器 + 它的命令子进程”这棵树。如果其中任何一个是你（新实例）的祖先，真正的占用者也在同一棵树里，杀其它持有者既不能释放端口、还会误杀无关命令。这种情况请从另一个父进程（例如终端）启动，或先停掉旧实例。

## 编译

```bash
g++ -std=c++11 -O2 -static -o mcp-shell-server mcp-shell-server.cpp -lpthread
```

本机（Android 10 / aarch64）没有可用的 glibc/NDK g++，用随包附带的 musl 工具链即可：

```bash
export PATH=/data/sdext2/tool/gcc/bin:$PATH      # aarch64-linux-musl-g++
g++ -std=c++11 -O2 -static -o mcp-shell-server mcp-shell-server.cpp -lpthread
```

## 变更

- 2026-10-07：新增 `-f` / `--force`；端口被占用时可自动踢掉占用者后在该端口启动（自实现 /proc 扫描，零外部依赖）。已实测：`-p 18080 -f` 可收回遗留实例占用的端口；占用者属于本进程树时拒绝接管且不杀任何进程。

https://github.com/AcE77505/mcp-shell-server
