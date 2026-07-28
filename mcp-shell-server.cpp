/*
 * mcp-shell-server.cpp - ARM64 Static MCP Shell Server
 * 
 * Protocol: MCP 2025-11-25 (Streamable HTTP, JSON-RPC 2.0)
 * Endpoint: POST /mcp
 * Tool: shell(command) -> {stdout, stderr, exit_code}
 * Tool: reset -> reset server state
 *
 * Build: g++ -std=c++11 -O2 -static -o mcp-shell-server mcp-shell-server.cpp -lpthread
 *
 * Zero external dependencies. Single static binary.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <ctime>
#include <time.h>
#include <cstdarg>

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>

#define DEFAULT_PORT 8080
#define MCP_ENDPOINT "/mcp"
#define MCP_PROTOCOL_VERSION "2025-11-25"
#define MCP_SERVER_NAME "mcp-shell-server"
#define MCP_SERVER_VERSION "1.0.0"
#define BUFFER_SIZE 65536

enum LogLevel { LOG_INFO, LOG_WARN, LOG_ERROR };

static void log_msg(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const char* prefix = "";
    switch (level) {
        case LOG_INFO:  prefix = "[I] "; break;
        case LOG_WARN:  prefix = "[W] "; break;
        case LOG_ERROR: prefix = "[E] "; break;
    }
    fprintf(stderr, "%s", prefix);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    fflush(stderr);
    va_end(args);
}

// ============================================================
// Minimal JSON Builder - comma-safe
// ============================================================
class JsonBuilder {
private:
    std::string buf;

    void add_sep() {
        if (!buf.empty() && buf.back() != '{' && buf.back() != '[')
            buf += ',';
    }

    static std::string esc(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 2);
        for (size_t i = 0; i < s.size(); i++) {
            char c = s[i];
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char hex[8];
                        snprintf(hex, sizeof(hex), "\\u%04x", (unsigned char)c);
                        out += hex;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

public:
    JsonBuilder() { buf = ""; }
    void clear() { buf = ""; }

    void begin_object() {
        if (!buf.empty() && buf.back() == '}') buf += ',';
        buf += '{';
    }
    void end_object() { buf += '}'; }
    void begin_array() {
        if (!buf.empty() && buf.back() == ']') buf += ',';
        buf += '[';
    }
    void end_array() { buf += ']'; }

    void key(const std::string& k) {
        add_sep();
        buf += '"' + esc(k) + '"' + ':';
    }

    void value_string(const std::string& v) { buf += '"' + esc(v) + '"'; }
    void value_int(long v) { buf += std::to_string(v); }
    void value_bool(bool v) { buf += (v ? "true" : "false"); }
    void value_null() { buf += "null"; }
    void value_raw(const std::string& raw) { buf += raw; }

    void av_string(const std::string& v) { add_sep(); buf += '"' + esc(v) + '"'; }
    void av_int(long v) { add_sep(); buf += std::to_string(v); }
    void av_bool(bool v) { add_sep(); buf += (v ? "true" : "false"); }
    void av_raw(const std::string& raw) { add_sep(); buf += raw; }
    void av_object() { add_sep(); buf += '{'; }
    void av_array() { add_sep(); buf += '['; }

    void key_string(const std::string& k, const std::string& v) { key(k); value_string(v); }
    void key_int(const std::string& k, long v) { key(k); value_int(v); }
    void key_bool(const std::string& k, bool v) { key(k); value_bool(v); }
    void key_null(const std::string& k) { key(k); value_null(); }
    void key_raw(const std::string& k, const std::string& raw) { key(k); value_raw(raw); }
    void key_object(const std::string& k) { key(k); begin_object(); }
    void key_array(const std::string& k) { key(k); begin_array(); }

    std::string str() const { return buf; }
};

// ============================================================
// Minimal JSON Pull-Parser
// ============================================================
enum JsonToken {
    TOK_EOF, TOK_OBJECT_BEGIN, TOK_OBJECT_END,
    TOK_ARRAY_BEGIN, TOK_ARRAY_END,
    TOK_STRING, TOK_NUMBER, TOK_TRUE, TOK_FALSE, TOK_NULL,
    TOK_COLON, TOK_COMMA, TOK_ERROR
};

class JsonParser {
private:
    const char* p;
    const char* end;
    const char* start;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }

public:
    std::string string_value;
    long number_value;
    bool is_negative;

    JsonParser() : p(nullptr), end(nullptr), start(nullptr), number_value(0), is_negative(false) {}

    void set_input(const std::string& input) {
        p = input.c_str();
        end = p + input.size();
        start = p;
    }

    size_t pos() const { return static_cast<size_t>(p - start); }

    JsonToken next() {
        skip_ws();
        if (p >= end) return TOK_EOF;
        switch (*p) {
            case '{': p++; return TOK_OBJECT_BEGIN;
            case '}': p++; return TOK_OBJECT_END;
            case '[': p++; return TOK_ARRAY_BEGIN;
            case ']': p++; return TOK_ARRAY_END;
            case ':': p++; return TOK_COLON;
            case ',': p++; return TOK_COMMA;
            case '"': return parse_string();
            case 't': return parse_literal("true", TOK_TRUE);
            case 'f': return parse_literal("false", TOK_FALSE);
            case 'n': return parse_literal("null", TOK_NULL);
            case '-': case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                return parse_number();
            default: return TOK_ERROR;
        }
    }

    void skip_value() { skip_token(next()); }

    void skip_token(JsonToken t) {
        int depth = 0;
        switch (t) {
            case TOK_OBJECT_BEGIN: depth = 1; while (depth > 0) { JsonToken n = next(); if (n == TOK_OBJECT_BEGIN) depth++; else if (n == TOK_OBJECT_END) depth--; else if (n == TOK_EOF) return; } break;
            case TOK_ARRAY_BEGIN:  depth = 1; while (depth > 0) { JsonToken n = next(); if (n == TOK_ARRAY_BEGIN) depth++; else if (n == TOK_ARRAY_END) depth--; else if (n == TOK_EOF) return; } break;
            default: break;
        }
    }

    std::string extract_raw_value(const std::string& json_obj, const std::string& target_key) {
        JsonParser p2;
        p2.set_input(json_obj);
        if (p2.next() != TOK_OBJECT_BEGIN) return "";
        while (true) {
            JsonToken t = p2.next();
            if (t == TOK_OBJECT_END) break;
            if (t != TOK_STRING) return "";
            std::string k = p2.string_value;
            if (p2.next() != TOK_COLON) return "";
            if (k == target_key) {
                size_t vs = p2.pos();
                p2.skip_value();
                size_t ve = p2.pos();
                return json_obj.substr(vs, ve - vs);
            }
            p2.skip_value();
            t = p2.next();
            if (t == TOK_OBJECT_END) break;
        }
        return "";
    }

    std::string get_param_string(const std::string& json_obj, const std::string& key) {
        std::string raw = extract_raw_value(json_obj, key);
        if (raw.empty()) return "";
        JsonParser sp;
        sp.set_input(raw);
        if (sp.next() == TOK_STRING) return sp.string_value;
        return "";
    }

private:
    JsonToken parse_string() {
        p++;
        string_value.clear();
        while (p < end && *p != '"') {
            if (*p == '\\') {
                p++;
                if (p >= end) return TOK_ERROR;
                switch (*p) {
                    case '"': string_value += '"'; break;
                    case '\\': string_value += '\\'; break;
                    case '/': string_value += '/'; break;
                    case 'b': string_value += '\b'; break;
                    case 'f': string_value += '\f'; break;
                    case 'n': string_value += '\n'; break;
                    case 'r': string_value += '\r'; break;
                    case 't': string_value += '\t'; break;
                    case 'u': {
                        char hex[5] = {0};
                        if (p + 4 >= end) return TOK_ERROR;
                        memcpy(hex, p + 1, 4);
                        string_value += "\\u"; string_value += hex; p += 4;
                        break;
                    }
                    default: string_value += *p; break;
                }
                p++;
            } else {
                string_value += *p; p++;
            }
        }
        if (p >= end) return TOK_ERROR;
        p++;
        return TOK_STRING;
    }

    JsonToken parse_literal(const char* expected, JsonToken token) {
        size_t len = strlen(expected);
        if (static_cast<size_t>(end - p) < len) return TOK_ERROR;
        if (strncmp(p, expected, len) != 0) return TOK_ERROR;
        p += len;
        return token;
    }

    JsonToken parse_number() {
        is_negative = false; number_value = 0;
        if (*p == '-') { is_negative = true; p++; }
        while (p < end && *p >= '0' && *p <= '9') { number_value = number_value * 10 + (*p - '0'); p++; }
        if (p < end && *p == '.') { p++; while (p < end && *p >= '0' && *p <= '9') p++; }
        if (p < end && (*p == 'e' || *p == 'E')) { p++; if (p < end && (*p == '+' || *p == '-')) p++; while (p < end && *p >= '0' && *p <= '9') p++; }
        if (is_negative) number_value = -number_value;
        return TOK_NUMBER;
    }
};

struct MCPRequest {
    std::string jsonrpc;
    std::string id_str;
    bool has_id = false;
    bool id_is_string = false;
    std::string method;
    std::string raw_params;
};

// Case-insensitive find helper
static size_t ifind(const std::string& haystack, const std::string& needle, size_t pos = 0) {
    if (needle.empty()) return std::string::npos;
    for (size_t i = pos; i + needle.size() <= haystack.size(); i++) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); j++) {
            if (std::tolower(haystack[i + j]) != std::tolower(needle[j])) { match = false; break; }
        }
        if (match) return i;
    }
    return std::string::npos;
}

class MCPServer {
private:
    int server_fd;
    int port;
    bool initialized;
    JsonParser parser_helper;
    std::string my_session_id;

    std::string gen_session_id() {
        char buf[64];
        snprintf(buf, sizeof(buf), "mcp-shell-%ld-%d",
                 static_cast<long>(time(nullptr)), rand() % 100000);
        return std::string(buf);
    }

    size_t find_eol(const std::string& s, size_t pos) {
        size_t crlf = s.find("\r\n", pos);
        size_t lf = s.find("\n", pos);
        if (crlf != std::string::npos && lf != std::string::npos)
            return (crlf < lf) ? crlf : lf;
        if (crlf != std::string::npos) return crlf;
        return lf;
    }

    size_t find_hdr_end(const std::string& s) {
        size_t crlfcrlf = s.find("\r\n\r\n");
        size_t lflf = s.find("\n\n");
        if (crlfcrlf != std::string::npos && lflf != std::string::npos)
            return (crlfcrlf < lflf) ? crlfcrlf + 4 : lflf + 2;
        if (crlfcrlf != std::string::npos) return crlfcrlf + 4;
        if (lflf != std::string::npos) return lflf + 2;
        return std::string::npos;
    }

    bool parse_http(const std::string& raw, std::string& method,
                    std::string& path, std::map<std::string,std::string>& hdrs,
                    std::string& body) {
        size_t le = find_eol(raw, 0);
        if (le == std::string::npos) return false;
        std::string rl = raw.substr(0, le);
        if (!rl.empty() && rl.back() == '\r') rl.pop_back();
        size_t m1 = rl.find(' '), m2 = rl.rfind(' ');
        if (m1 == std::string::npos || m2 == std::string::npos || m2 == m1) return false;
        method = rl.substr(0, m1);
        path = rl.substr(m1 + 1, m2 - m1 - 1);

        size_t pos = le + ((raw[le] == '\r') ? 2 : 1);
        while (pos < raw.size()) {
            size_t he = find_eol(raw, pos);
            if (he == std::string::npos) break;
            if (he == pos || (he == pos + 1 && raw[pos] == '\r')) {
                size_t body_start = he + ((raw[he] == '\r') ? 2 : 1);
                body = raw.substr(body_start);
                return true;
            }
            std::string hl = raw.substr(pos, he - pos);
            if (!hl.empty() && hl.back() == '\r') hl.pop_back();
            size_t co = hl.find(':');
            if (co != std::string::npos) {
                std::string hn = hl.substr(0, co);
                std::string hv = hl.substr(co + 1);
                size_t vs = hv.find_first_not_of(" \t");
                if (vs != std::string::npos) hv = hv.substr(vs);
                size_t ve = hv.find_last_not_of(" \t\r");
                if (ve != std::string::npos) hv = hv.substr(0, ve + 1);
                std::transform(hn.begin(), hn.end(), hn.begin(), ::tolower);
                hdrs[hn] = hv;
            }
            pos = he + ((raw[he] == '\r') ? 2 : 1);
        }
        return false;
    }

    std::string http_resp(int code, const std::string& text,
                          const std::string& ct, const std::string& body,
                          const std::map<std::string,std::string>& extra = {}) {
        std::string r;
        r += "HTTP/1.1 " + std::to_string(code) + " " + text + "\r\n";
        r += "Content-Type: " + ct + "\r\n";
        r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        r += "Access-Control-Allow-Origin: *\r\n";
        for (const auto& h : extra) r += h.first + ": " + h.second + "\r\n";
        r += "\r\n" + body;
        return r;
    }

    std::string jerr(const MCPRequest& req, int code, const std::string& msg) {
        JsonBuilder j;
        j.begin_object();
        j.key_string("jsonrpc", "2.0");
        if (!req.has_id) j.key_null("id");
        else if (req.id_is_string) j.key_string("id", req.id_str);
        else j.key_raw("id", req.id_str);
        j.key_object("error");
        j.key_int("code", code);
        j.key_string("message", msg);
        j.end_object();
        j.end_object();
        return j.str();
    }

    std::string jok(const MCPRequest& req, const std::string& result_json) {
        JsonBuilder j;
        j.begin_object();
        j.key_string("jsonrpc", "2.0");
        if (!req.has_id) j.key_null("id");
        else if (req.id_is_string) j.key_string("id", req.id_str);
        else j.key_raw("id", req.id_str);
        j.key_raw("result", result_json);
        j.end_object();
        return j.str();
    }

    bool parse_jrpc(const std::string& body, MCPRequest& req) {
        JsonParser p; p.set_input(body);
        if (p.next() != TOK_OBJECT_BEGIN) return false;
        while (true) {
            JsonToken t = p.next();
            if (t == TOK_OBJECT_END) break;
            if (t != TOK_STRING) return false;
            std::string k = p.string_value;
            if (p.next() != TOK_COLON) return false;
            if (k == "jsonrpc") { if (p.next() != TOK_STRING) return false; req.jsonrpc = p.string_value; }
            else if (k == "id") {
                size_t ps = p.pos();
                t = p.next();
                if (t == TOK_STRING) { req.id_str = p.string_value; req.has_id = true; req.id_is_string = true; }
                else if (t == TOK_NUMBER) {
                    size_t pe = p.pos();
                    req.id_str = body.substr(ps, pe - ps);
                    req.has_id = true; req.id_is_string = false;
                }
                else if (t == TOK_NULL) { req.has_id = false; }
                else return false;
            } else if (k == "method") { if (p.next() != TOK_STRING) return false; req.method = p.string_value; }
            else if (k == "params") { size_t ps = p.pos(); p.skip_value(); req.raw_params = body.substr(ps, p.pos() - ps); }
            else p.skip_value();
            t = p.next();
            if (t == TOK_OBJECT_END) break;
            if (t != TOK_COMMA) return false;
        }
        return !req.method.empty();
    }

    std::string handle_initialize(const MCPRequest& req) {
        initialized = true;
        my_session_id = gen_session_id();
        JsonBuilder r;
        r.begin_object();
        r.key_string("protocolVersion", MCP_PROTOCOL_VERSION);
        r.key_object("capabilities");
        r.key_object("tools"); r.key_bool("listChanged", false); r.end_object();
        r.end_object();
        r.key_object("serverInfo");
        r.key_string("name", MCP_SERVER_NAME); r.key_string("version", MCP_SERVER_VERSION);
        r.end_object();
        r.end_object();
        std::map<std::string,std::string> ex;
        ex["MCP-Session-Id"] = my_session_id;
        ex["MCP-Protocol-Version"] = MCP_PROTOCOL_VERSION;
        return http_resp(200, "OK", "application/json", jok(req, r.str()), ex);
    }

    std::string handle_tools_list(const MCPRequest& req) {
        JsonBuilder r;
        r.begin_object();
        r.key_array("tools");

        r.begin_object();
        r.key_string("name", "shell");
        r.key_string("description", "Execute a shell command and return its output.");
        r.key_object("inputSchema");
        r.key_string("type", "object");
        r.key_array("required"); r.av_string("command"); r.end_array();
        r.key_object("properties");
        r.key_object("command"); r.key_string("type", "string"); r.key_string("description", "Shell command to execute"); r.end_object();
        r.key_object("timeout"); r.key_string("type", "number"); r.key_string("description", "Timeout in seconds"); r.end_object();
        r.end_object();
        r.end_object();
        r.end_object();

        r.begin_object();
        r.key_string("name", "reset");
        r.key_string("description", "Reset server state. Clears initialization flag, safe to call anytime.");
        r.key_object("inputSchema");
        r.key_string("type", "object");
        r.key_array("required"); r.end_array();
        r.key_object("properties");
        r.end_object();
        r.end_object();
        r.end_object();

        r.end_array();
        r.end_object();
        return http_resp(200, "OK", "application/json", jok(req, r.str()));
    }

    std::string exec_cmd(const std::string& cmd, long timeout_sec) {
        log_msg(LOG_INFO, "Executing: %s", cmd.c_str());
        int pout[2], perr[2];
        if (pipe(pout) < 0 || pipe(perr) < 0) {
            JsonBuilder j; j.begin_object(); j.key_string("stdout",""); j.key_string("stderr","pipe failed"); j.key_int("exit_code",-1); j.key_bool("success",false); j.end_object();
            return j.str();
        }
        pid_t pid = fork();
        if (pid < 0) {
            close(pout[0]); close(pout[1]); close(perr[0]); close(perr[1]);
            JsonBuilder j; j.begin_object(); j.key_string("stdout",""); j.key_string("stderr","fork failed"); j.key_int("exit_code",-1); j.key_bool("success",false); j.end_object();
            return j.str();
        }
        if (pid == 0) {
            close(pout[0]); close(perr[0]);
            dup2(pout[1], STDOUT_FILENO); dup2(perr[1], STDERR_FILENO);
            close(pout[1]); close(perr[1]);
            signal(SIGCHLD, SIG_DFL);
            execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
            _exit(127);
        }
        close(pout[1]); close(perr[1]);
        int fl; fl = fcntl(pout[0], F_GETFL, 0); fcntl(pout[0], F_SETFL, fl | O_NONBLOCK);
        fl = fcntl(perr[0], F_GETFL, 0); fcntl(perr[0], F_SETFL, fl | O_NONBLOCK);

        std::string so, se;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        long deadline_ms = (timeout_sec > 0 ? timeout_sec : 30) * 1000;
        bool eof_so = false, eof_se = false, exited = false;
        int ec = -1;

        while (true) {
            struct timespec tn; clock_gettime(CLOCK_MONOTONIC, &tn);
            long elapsed = (tn.tv_sec - ts.tv_sec) * 1000 + (tn.tv_nsec - ts.tv_nsec) / 1000000;
            if (elapsed >= deadline_ms) break;
            if (eof_so && eof_se && exited) break;
            if (eof_so && eof_se && !exited) {
                int st; pid_t r = waitpid(pid, &st, 0);
                if (r == pid) { exited = true; ec = WIFEXITED(st) ? WEXITSTATUS(st) : -1; }
                break;
            }
            if (!exited) { int st; pid_t r = waitpid(pid, &st, WNOHANG); if (r == pid) { exited = true; ec = WIFEXITED(st) ? WEXITSTATUS(st) : -1; } }

            struct pollfd pf[2];
            pf[0].fd = pout[0]; pf[0].events = eof_so ? 0 : POLLIN;
            pf[1].fd = perr[0]; pf[1].events = eof_se ? 0 : POLLIN;
            int pr = poll(pf, 2, 100);
            if (pr < 0) break;

            if (!eof_so) { char b[4096]; int n = read(pout[0], b, sizeof(b)); if (n > 0) so.append(b, n); else eof_so = true; }
            if (!eof_se) { char b[4096]; int n = read(perr[0], b, sizeof(b)); if (n > 0) se.append(b, n); else eof_se = true; }
        }

        if (!exited) { kill(pid, SIGKILL); waitpid(pid, nullptr, 0); }
        char db[4096];
        while (true) { int n = read(pout[0], db, sizeof(db)); if (n <= 0) break; so.append(db, n); }
        while (true) { int n = read(perr[0], db, sizeof(db)); if (n <= 0) break; se.append(db, n); }
        close(pout[0]); close(perr[0]);

        JsonBuilder j;
        j.begin_object();
        j.key_string("stdout", so);
        j.key_string("stderr", se);
        j.key_int("exit_code", ec);
        j.key_bool("success", ec == 0);
        j.end_object();
        return j.str();
    }

    std::string handle_tool_reset(const MCPRequest& req) {
        log_msg(LOG_INFO, "Tool call: reset");
        initialized = false;
        JsonBuilder r;
        r.begin_object();
        r.key_string("status", "ok");
        r.key_string("message", "Server state reset. Re-initialize before using tools.");
        r.end_object();
        JsonBuilder w;
        w.begin_object();
        w.key_array("content");
        w.begin_object();
        w.key_string("type", "text");
        w.key_string("text", r.str());
        w.end_object();
        w.end_array();
        w.key_bool("isError", false);
        w.end_object();
        return http_resp(200, "OK", "application/json", jok(req, w.str()));
    }

    std::string handle_tools_call(const MCPRequest& req) {
        std::string tn = parser_helper.get_param_string(req.raw_params, "name");
        log_msg(LOG_INFO, "Tool call: %s", tn.c_str());
        if (tn == "shell") {
            std::string aj = parser_helper.extract_raw_value(req.raw_params, "arguments");
            std::string cmd = parser_helper.get_param_string(aj, "command");
            if (cmd.empty()) {
                return http_resp(200, "OK", "application/json", jerr(req, -32602, "Missing: command"));
            }
            std::string er = exec_cmd(cmd, 30);
            JsonBuilder w;
            w.begin_object();
            w.key_array("content");
            w.begin_object();
            w.key_string("type", "text");
            w.key_string("text", er);
            w.end_object();
            w.end_array();
            w.key_bool("isError", false);
            w.end_object();
            return http_resp(200, "OK", "application/json", jok(req, w.str()));
        } else if (tn == "reset") {
            return handle_tool_reset(req);
        }
        return http_resp(200, "OK", "application/json", jerr(req, -32601, "Unknown tool: " + tn));
    }

    std::string handle_ping(const MCPRequest& req) {
        return http_resp(200, "OK", "application/json", jok(req, "{}"));
    }

    std::string handle_notif(const MCPRequest& req) {
        if (req.method == "notifications/initialized")
            log_msg(LOG_INFO, "Client initialized");
        return http_resp(202, "Accepted", "text/plain", "");
    }

    std::string dispatch(const std::string& body, const std::map<std::string,std::string>& hdrs) {
        MCPRequest req;
        if (!parse_jrpc(body, req)) {
            std::string logged = body.substr(0, 500);
            log_msg(LOG_ERROR, "JSON parse failed. Raw body (%zu bytes): %s", body.size(), logged.c_str());
            return http_resp(400, "Bad Request", "application/json", jerr(req, -32700, "Parse error"));
        }
        if (req.jsonrpc != "2.0") return http_resp(400, "Bad Request", "application/json", jerr(req, -32600, "Bad version"));
        if (req.method == "initialize") return handle_initialize(req);
        if (!req.has_id) return handle_notif(req);
        if (req.method == "ping") return handle_ping(req);
        if (req.method == "tools/list") return handle_tools_list(req);
        if (req.method == "tools/call") return handle_tools_call(req);
        return http_resp(200, "OK", "application/json", jerr(req, -32601, "Not found: " + req.method));
    }

    std::string handle_options() {
        std::map<std::string,std::string> h;
        h["access-control-allow-origin"] = "*";
        h["access-control-allow-methods"] = "POST, GET, OPTIONS, DELETE";
        h["access-control-allow-headers"] = "*";
        h["access-control-max-age"] = "86400";
        return http_resp(204, "No Content", "text/plain", "", h);
    }

    std::string handle_get() {
        return http_resp(405, "Method Not Allowed", "text/plain", "Use POST /mcp");
    }

    std::string handle_delete() {
        log_msg(LOG_INFO, "Session terminated by client");
        initialized = false;
        return http_resp(204, "No Content", "text/plain", "");
    }

    void handle_client(int fd) {
        std::string raw;
        char buf[BUFFER_SIZE];
        int total = 0;

        struct pollfd pfd;
        pfd.fd = fd; pfd.events = POLLIN;

        while (total < (int)sizeof(buf) - 1) {
            int r = poll(&pfd, 1, 5000);
            if (r <= 0) break;
            int n = read(fd, buf + total, sizeof(buf) - total - 1);
            if (n <= 0) break;
            total += n;
            buf[total] = '\0';
            raw = std::string(buf, total);

            size_t hdr_end = find_hdr_end(raw);
            if (hdr_end != std::string::npos) {
                size_t cl_pos = ifind(raw, "content-length");
                if (cl_pos != std::string::npos) {
                    size_t col = raw.find(':', cl_pos);
                    size_t vs = raw.find_first_not_of(" \t", col + 1);
                    if (vs != std::string::npos) {
                        size_t ve_v = find_eol(raw, vs);
                        if (ve_v != std::string::npos) {
                            int cl = atoi(raw.substr(vs, ve_v - vs).c_str());
                            size_t body_start = hdr_end;
                            int body_have = (int)total - (int)body_start;
                            if (body_have >= cl) break;
                        }
                    }
                } else {
                    struct pollfd tp; tp.fd = fd; tp.events = POLLIN;
                    int tr = poll(&tp, 1, 200);
                    if (tr > 0) {
                        int tn = read(fd, buf + total, sizeof(buf) - total - 1);
                        if (tn > 0) { total += tn; buf[total] = '\0'; raw = std::string(buf, total); }
                    }
                    break;
                }
            }
        }

        if (raw.empty()) { close(fd); return; }

        std::string method, path, body;
        std::map<std::string,std::string> hdrs;
        if (!parse_http(raw, method, path, hdrs, body)) {
            log_msg(LOG_ERROR, "HTTP parse failed for %zu bytes", raw.size());
            std::string r = http_resp(400, "Bad Request", "text/plain", "Bad HTTP");
            (void)!write(fd, r.c_str(), r.size()); close(fd); return;
        }

        log_msg(LOG_INFO, "%s %s", method.c_str(), path.c_str());
        std::string resp;
        if (method == "OPTIONS") resp = handle_options();
        else if (method == "GET") resp = handle_get();
        else if (method == "DELETE") resp = handle_delete();
        else if (method == "POST" && path == MCP_ENDPOINT) resp = dispatch(body, hdrs);
        else resp = http_resp(404, "Not Found", "text/plain", "Not Found");

        if (!resp.empty()) (void)!write(fd, resp.c_str(), resp.size());
        close(fd);
    }

public:
    MCPServer() : server_fd(-1), port(DEFAULT_PORT), initialized(false) {
        srand(time(nullptr) ^ getpid());
    }

    bool start(int listen_port = DEFAULT_PORT) {
        port = listen_port;
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) { log_msg(LOG_ERROR, "socket: %s", strerror(errno)); return false; }
        int opt = 1; setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            log_msg(LOG_ERROR, "bind port %d: %s", port, strerror(errno));
            close(server_fd); return false;
        }
        if (listen(server_fd, SOMAXCONN) < 0) {
            log_msg(LOG_ERROR, "listen: %s", strerror(errno));
            close(server_fd); return false;
        }
        log_msg(LOG_INFO, "============================================");
        log_msg(LOG_INFO, "  MCP Shell Server v" MCP_SERVER_VERSION);
        log_msg(LOG_INFO, "  Listen: 0.0.0.0:%d", port);
        log_msg(LOG_INFO, "  Endpoint: POST %s", MCP_ENDPOINT);
        log_msg(LOG_INFO, "  Protocol: MCP %s", MCP_PROTOCOL_VERSION);
        log_msg(LOG_INFO, "============================================");
        log_msg(LOG_INFO, "  Connect: http://<ip>:%d%s", port, MCP_ENDPOINT);
        return true;
    }

    void run() {
        while (true) {
            struct sockaddr_in ca;
            socklen_t cl = sizeof(ca);
            int fd = accept(server_fd, (struct sockaddr*)&ca, &cl);
            if (fd < 0) { if (errno == EINTR) continue; break; }
            handle_client(fd);
        }
    }

    ~MCPServer() { if (server_fd >= 0) close(server_fd); }
};

int main(int argc, char* argv[]) {
    int port = DEFAULT_PORT;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            port = atoi(argv[++i]);
            if (port <= 0 || port > 65535) {
                log_msg(LOG_ERROR, "Invalid port: %s", argv[i]); return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("MCP Shell Server - ARM64 Static Binary\n");
            printf("Usage: %s [options]\n", argv[0]);
            printf("  -p, --port PORT  Port (default: %d)\n", DEFAULT_PORT);
            printf("  -h, --help       This help\n");
            return 0;
        }
    }

    signal(SIGPIPE, SIG_IGN);

    MCPServer srv;
    if (!srv.start(port)) return 1;
    srv.run();
    return 0;
}
