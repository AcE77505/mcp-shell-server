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

## 安全模型（务必先读）

**本服务会把收到的命令原样交给 `execl("/bin/sh", "sh", "-c", cmd, NULL)` 执行，并以服务器进程的身份运行**（root 用户可以启动服务器，因此命令也会以 root 用户执行）。
“能执行任意命令”是本项目的功能本体，不是缺陷——对任意命令做“校验/净化”在逻辑上不成立（同类 shell MCP 服务均如此），因此本项目**没有**命令白名单，也不打算加。

同时，当前默认配置**没有任何访问控制**：

| 事实 | 说明 |
|------|------|
| 默认监听 `0.0.0.0` | 同一局域网内任何主机都能访问 |
| 无认证 | 不需要 token/cookie/session，**也不需要先 `initialize`**，直接 `POST /mcp` + `tools/call shell` 即可执行 |
| CORS 为 `Access-Control-Allow-Origin: *` | 任意网页来源都放行，且攻击页还能**读到命令输出**；`OPTIONS` 预检返回 `*` + `Allow-Headers: *` |
| 不校验 `Content-Type` | `text/plain` 也能触发执行，属浏览器“简单请求”，**连预检都不需要** |
| 不校验 `Origin` | 浏览器“投毒网页驱动执行”（CSRF 式 drive-by）在本机/局域网内可行 |

实测（自建实例，非线上 8080）：不带任何认证头、不 `initialize`、带 `Origin: http://evil.example` 的 `POST /mcp` + `tools/call shell` 返回 `200`，并成功执行 `echo PWNED uid=$(id -u)` → `uid=0`。

**建议**

- 只在**可信环境**里运行：本机回环，或你自己的、可信任的局域网。**不要**把端口暴露到公网、做端口转发、或在公共 Wi-Fi 下开启。
- 需要更强的边界时，请自行加一层：只绑回环（当前版本没有 `-H`，可用 iptables/端口转发或自行改 `sin_addr`）、或用反向代理加 token 校验。
- `-f`（踢掉占用者）只是个便利功能，**不是安全机制**。
- 本机部署下命令以 **root** 执行，一旦被触发，影响面相当于设备完全失守。

## 安全边界与 issue 政策

**使用前提（硬性）**：本服务只在**完全受信的运行环境**下使用 —— 本机回环，或你自己完全掌控且可信的局域网。**不满足该前提的部署方式不在支持范围内**（包括但不限于：映射到公网、端口转发、公共 Wi-Fi、共享/不可信网络，以及让不可信页面能访问到该端口的场景）。

因此以下报告**不作为缺陷**处理（按设计 by design，不开工）：

- “命令注入（CWE-78）”“缺少输入校验/命令白名单” —— 执行任意命令是本项目的功能本体，无法“校验”；
- “默认绑 `0.0.0.0`”“缺少认证/授权”“CORS `*`” —— 已在上一节列为**已知、已文档化**的暴露面，需要更强边界请自行加一层（只绑回环 / 反代加 token）；
- 以上前提下造成的任何损失（含 root 命令被执行） —— 风险自负。

我们只处理**针对当前源码可复现的具体缺陷**：崩溃、内存/越界问题、逻辑错误等。提交 issue 前请先读本节与上一节，并给出最小 PoC + 版本/commit；否则会被直接引用本节关闭。

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
- 2026-10-07：README 新增「安全模型」章节，明确“任意命令执行是功能本体”，并列出默认无认证/绑 `0.0.0.0`/CORS `*` 等暴露面、实测结论与使用建议（含对相关 issue 的回应口径）；代码未变。
- 2026-10-07：README 新增「安全边界与 issue 政策」：把“只在受信环境运行”定为硬性使用前提，并声明“命令可注入 / 缺少认证 / 绑 `0.0.0.0` / CORS `*`”不作为缺陷处理（删除原先“以后可能加入访问控制”的表述）；代码未变。

https://github.com/AcE77505/mcp-shell-server
