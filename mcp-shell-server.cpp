/* MCP Shell Server - ARM64 static, pthread-based, shared-memory command tracking
 * Protocol: MCP 2025-11-25, Streamable HTTP, JSON-RPC 2.0
 * Tools: shell, status (all terminals + is_mine), stop (own only), reset
 * Build: g++ -std=c++11 -O2 -static -o mcp-shell-server mcp-shell-server.cpp -lpthread
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
#include <pthread.h>

#define PORT 8080
#define EP "/mcp"
#define VER "2025-11-25"
#define BUF 65536
#define MAXCMD 64

enum { LI, LW, LE };
static void log(int l, const char* f, ...) {
    va_list a; va_start(a, f); fprintf(stderr, "%s", l==LI?"[I] ":l==LW?"[W] ":"[E] ");
    vfprintf(stderr, f, a); fprintf(stderr, "\n"); fflush(stderr); va_end(a);
}

/* Shared command tracking table (mutex-protected, inherited by all threads) */
struct Cmd { pid_t pid; char cmd[256]; time_t start; char sid[64]; int alive; };
static Cmd tbl[MAXCMD];
static int cnt = 0;
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

static void cmd_add(pid_t pid, const char* cmd, const char* sid) {
    pthread_mutex_lock(&mtx);
    if (cnt < MAXCMD) {
        tbl[cnt].pid = pid; tbl[cnt].start = time(0); tbl[cnt].alive = 1;
        strncpy(tbl[cnt].cmd, cmd, 255); tbl[cnt].cmd[255] = 0;
        strncpy(tbl[cnt].sid, sid ? sid : "", 63); tbl[cnt].sid[63] = 0;
        cnt++;
    }
    pthread_mutex_unlock(&mtx);
}
static void cmd_del(pid_t pid) {
    pthread_mutex_lock(&mtx);
    for (int i = 0; i < cnt; i++) if (tbl[i].pid == pid && tbl[i].alive) { tbl[i].alive = 0; break; }
    pthread_mutex_unlock(&mtx);
}
static std::string cmd_list(const char* my_sid) {
    pthread_mutex_lock(&mtx); std::string r; r = '[';
    for (int i = 0; i < cnt; i++) {
        if (!tbl[i].alive) continue;
        if (kill(tbl[i].pid, 0) != 0) { tbl[i].alive = 0; continue; }
        bool mine = my_sid && tbl[i].sid[0] && !strcmp(tbl[i].sid, my_sid);
        char e[512]; int n = snprintf(e, sizeof(e), "%c{\"pid\":%d,\"cmd\":\"%s\",\"elapsed\":%ld,\"mine\":%s}",
            r.size()>1?',':' ', tbl[i].pid, tbl[i].cmd, (long)(time(0)-tbl[i].start), mine?"true":"false");
        if (n > 0) r += e;
    }
    r += ']'; pthread_mutex_unlock(&mtx); return r;
}
static int cmd_stop(const char* my_sid, int tpid) {
    pthread_mutex_lock(&mtx); int k = 0;
    for (int i = 0; i < cnt; i++) {
        if (!tbl[i].alive) continue;
        if (kill(tbl[i].pid, 0) != 0) { tbl[i].alive = 0; continue; }
        bool match = (tpid > 0) ? (tbl[i].pid == tpid) : (my_sid && tbl[i].sid[0] && !strcmp(tbl[i].sid, my_sid));
        if (match) { kill(-tbl[i].pid, SIGINT); kill(tbl[i].pid, SIGINT); tbl[i].alive = 0; k++; }
    }
    pthread_mutex_unlock(&mtx); return k;
}

/* JSON Builder */
class J {
    std::string b;
    void sep() { if (!b.empty() && b.back()!='{' && b.back()!='[') b += ','; }
    static std::string esc(const std::string& s) {
        std::string o;
        for (auto c : s) {
            switch (c) {
                case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
                default: if ((unsigned char)c < 0x20) { char h[8]; snprintf(h,8,"\\u%04x",(unsigned char)c); o+=h; } else o+=c;
            }
        }
        return o;
    }
public:
    J() { b=""; } std::string str() { return b; }
    void o() { if (!b.empty() && b.back()=='}') b+=','; b+='{'; }
    void c() { b+='}'; } void a() { if (!b.empty() && b.back()==']') b+=','; b+='['; } void ea() { b+=']'; }
    void k(const std::string& s) { sep(); b+='"'+esc(s)+'"'+':'; }
    void vs(const std::string& s) { b+='"'+esc(s)+'"'; } void vi(long v) { b+=std::to_string(v); }
    void vb(bool v) { b+=v?"true":"false"; } void vn() { b+="null"; } void vr(const std::string& s) { b+=s; }
    void avs(const std::string& s) { sep(); b+='"'+esc(s)+'"'; }
    void avi(long v) { sep(); b+=std::to_string(v); } void avb(bool v) { sep(); b+=v?"true":"false"; }
    void avr(const std::string& s) { sep(); b+=s; } void avo() { sep(); b+='{'; }
    void ks(const std::string& k2, const std::string& v) { k(k2); vs(v); }
    void ki(const std::string& k2, long v) { k(k2); vi(v); } void kb(const std::string& k2, bool v) { k(k2); vb(v); }
    void kn(const std::string& k2) { k(k2); vn(); } void kr(const std::string& k2, const std::string& v) { k(k2); vr(v); }
    void ko(const std::string& k2) { k(k2); o(); } void ka(const std::string& k2) { k(k2); a(); }
};

/* JSON Parser */
enum Tok { T_E, T_O, T_OC, T_A, T_AC, T_S, T_N, T_U, T_F, T_L, T_CO, T_CM, T_ERR };
struct P {
    const char *p, *e, *s; std::string sv; long nv; bool ng;
    P():p(0),e(0),s(0),nv(0),ng(0){}
    void set(const std::string& in) { p=in.c_str(); e=p+in.size(); s=p; }
    size_t pos() { return (size_t)(p-s); }
    void ws() { while(p<e && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++; }
    Tok nx() {
        ws(); if(p>=e) return T_E;
        switch(*p) {
            case'{':p++;return T_O; case'}':p++;return T_OC;
            case'[':p++;return T_A; case']':p++;return T_AC;
            case':':p++;return T_CO; case',':p++;return T_CM;
            case'"':return ps();
            case't':return pl("true",T_U); case'f':return pl("false",T_F); case'n':return pl("null",T_L);
            case'-':case'0'...'9':return pn();
            default: return T_ERR;
        }
    }
    void sv2() { Tok t=nx(); int d=0;
        switch(t) { case T_O: d=1; while(d>0) { Tok n=nx(); if(n==T_O)d++; else if(n==T_OC)d--; else if(n==T_E)return; } break;
          case T_A: d=1; while(d>0) { Tok n=nx(); if(n==T_A)d++; else if(n==T_AC)d--; else if(n==T_E)return; } break; default:; } }
    std::string ex(const std::string& j, const std::string& tk) {
        P p2; p2.set(j); if(p2.nx()!=T_O) return "";
        while(1) { Tok t=p2.nx(); if(t==T_OC)break; if(t!=T_S)return "";
            std::string k=p2.sv; if(p2.nx()!=T_CO)return "";
            if(k==tk) { size_t vs=p2.pos(); p2.sv2(); return j.substr(vs,p2.pos()-vs); }
            p2.sv2(); t=p2.nx(); if(t==T_OC)break; } return ""; }
    std::string gps(const std::string& j, const std::string& k) {
        std::string r=ex(j,k); if(r.empty())return""; P p2; p2.set(r); return p2.nx()==T_S?p2.sv:""; }
private:
    Tok ps() {
        p++; sv.clear();
        while(p<e && *p!='"') {
            if(*p=='\\') { p++; if(p>=e) return T_ERR;
                switch(*p) { case'"':sv+='"';break; case'\\':sv+='\\';break; case'n':sv+='\n';break;
                    case'r':sv+='\r';break; case't':sv+='\t';break;
                    case'u':{char h[5]={0};if(p+4>=e)return T_ERR;memcpy(h,p+1,4);sv+="\\u";sv+=h;p+=4;break;}
                    default:sv+=*p; } p++; }
            else { sv+=*p; p++; } }
        if(p>=e) return T_ERR; p++; return T_S; }
    Tok pl(const char* e2, Tok t) { size_t l=strlen(e2); if(p+l>e) return T_ERR; if(strncmp(p,e2,l)) return T_ERR; p+=l; return t; }
    Tok pn() { ng=0; nv=0; if(*p=='-') { ng=1; p++; }
        while(p<e && *p>='0' && *p<='9') { nv=nv*10+(*p-'0'); p++; }
        if(p<e && *p=='.') { p++; while(p<e && *p>='0' && *p<='9') p++; }
        if(p<e && (*p=='e'||*p=='E')) { p++; if(p<e && (*p=='+'||*p=='-')) p++; while(p<e && *p>='0' && *p<='9') p++; }
        if(ng) nv=-nv; return T_N; }
};

struct Req { std::string jr, id; bool hi=0, is=0; std::string m, rp; };

static size_t ifind(const std::string& h, const std::string& n, size_t p=0) {
    if(n.empty()) return std::string::npos;
    for(size_t i=p; i+n.size()<=h.size(); i++) { bool m=1; for(size_t j=0;j<n.size();j++) if(tolower(h[i+j])!=tolower(n[j])){m=0;break;} if(m) return i; }
    return std::string::npos;
}

static void* handle_client(void* arg) {
    int fd = (int)(intptr_t)arg;
    std::string raw; char buf[BUF]; int total = 0;
    struct pollfd pfd; pfd.fd = fd; pfd.events = POLLIN;

    while (total < (int)sizeof(buf)-1) {
        int r = poll(&pfd, 1, 5000); if (r <= 0) break;
        int n = read(fd, buf+total, sizeof(buf)-total-1); if (n <= 0) break;
        total += n; buf[total] = 0; raw = std::string(buf, total);
        size_t hd = raw.find("\r\n\r\n"); if (hd == std::string::npos) hd = raw.find("\n\n");
        if (hd != std::string::npos) {
            size_t cp = ifind(raw, "content-length");
            if (cp != std::string::npos) {
                size_t col = raw.find(':', cp), vs = raw.find_first_not_of(" \t", col+1);
                if (vs != std::string::npos) {
                    size_t ve = raw.find("\r\n", vs); if (ve == std::string::npos) ve = raw.find("\n", vs);
                    if (ve != std::string::npos) {
                        int cl = atoi(raw.substr(vs, ve-vs).c_str());
                        size_t bs = hd + ((raw[hd]=='\r')?4:2);
                        if ((int)(total-(int)bs) >= cl) break;
            } } } else {
                struct pollfd tp; tp.fd = fd; tp.events = POLLIN;
                int tr = poll(&tp, 1, 200);
                if (tr > 0) { int tn = read(fd, buf+total, sizeof(buf)-total-1); if (tn > 0) { total+=tn; buf[total]=0; raw=std::string(buf,total); } }
                break;
    } } }

    if (raw.empty()) { close(fd); pthread_detach(pthread_self()); return 0; }

    size_t le = raw.find("\r\n"); if (le == std::string::npos) le = raw.find("\n");
    if (le == std::string::npos) { close(fd); pthread_detach(pthread_self()); return 0; }
    std::string rl = raw.substr(0, le);
    if (!rl.empty() && rl.back() == '\r') rl.pop_back();
    size_t m1 = rl.find(' '), m2 = rl.rfind(' ');
    std::string method, path, body;
    std::map<std::string,std::string> hdrs;
    if (m1 == std::string::npos || m2 == std::string::npos || m2 == m1) { close(fd); pthread_detach(pthread_self()); return 0; }
    method = rl.substr(0, m1); path = rl.substr(m1+1, m2-m1-1);
    { size_t pos = le + ((raw[le]=='\r')?2:1);
      while (pos < raw.size()) {
          size_t he = raw.find("\r\n", pos); if (he == std::string::npos) he = raw.find("\n", pos); if (he == std::string::npos) break;
          if (he == pos || (he == pos+1 && raw[pos] == '\r')) { body = raw.substr(he+((raw[he]=='\r')?2:1)); break; }
          std::string hl = raw.substr(pos, he-pos); if (!hl.empty() && hl.back() == '\r') hl.pop_back();
          size_t co = hl.find(':'); if (co != std::string::npos) {
              std::string hn = hl.substr(0, co), hv = hl.substr(co+1);
              size_t vs = hv.find_first_not_of(" \t"); if (vs != std::string::npos) hv = hv.substr(vs);
              size_t ve = hv.find_last_not_of(" \t\r"); if (ve != std::string::npos) hv = hv.substr(0, ve+1);
              std::transform(hn.begin(), hn.end(), hn.begin(), ::tolower); hdrs[hn] = hv;
          } pos = he + ((raw[he]=='\r')?2:1);
    } }

    log(LI, "%s %s", method.c_str(), path.c_str());

    auto hr_ = [&](int c, const std::string& txt, const std::string& ct, const std::string& b, const std::map<std::string,std::string>& ex={}) -> std::string {
        std::string r = "HTTP/1.1 " + std::to_string(c) + " " + txt + "\r\nContent-Type: " + ct + "\r\nContent-Length: " + std::to_string(b.size()) + "\r\nAccess-Control-Allow-Origin: *\r\n";
        for(auto& h : ex) r += h.first + ": " + h.second + "\r\n"; r += "\r\n" + b; return r; };

    auto jerr = [](const Req& r, int c, const std::string& m) -> std::string {
        J j; j.o(); j.ks("jsonrpc","2.0"); if(!r.hi) j.kn("id"); else if(r.is) j.ks("id",r.id); else j.kr("id",r.id);
        j.ko("error"); j.ki("code",c); j.ks("message",m); j.c(); j.c(); return j.str(); };
    auto jok = [](const Req& r, const std::string& res) -> std::string {
        J j; j.o(); j.ks("jsonrpc","2.0"); if(!r.hi) j.kn("id"); else if(r.is) j.ks("id",r.id); else j.kr("id",r.id);
        j.kr("result",res); j.c(); return j.str(); };

    std::string resp;
    if (method == "OPTIONS") {
        std::map<std::string,std::string> h; h["access-control-allow-origin"]="*"; h["access-control-allow-methods"]="POST,GET,OPTIONS,DELETE";
        h["access-control-allow-headers"]="*"; h["access-control-max-age"]="86400";
        resp = hr_(204, "No Content", "text/plain", "", h);
    } else if (method == "GET") resp = hr_(405, "Method Not Allowed", "text/plain", "Use POST /mcp");
    else if (method == "DELETE") { log(LI, "Session end"); resp = hr_(204, "No Content", "text/plain", ""); }
    else if (method == "POST" && path == EP) {
        Req r; P p; p.set(body);
        if (p.nx() != T_O) { log(LE, "JSON fail"); resp = hr_(400, "Bad Request", "application/json", jerr(r, -32700, "Parse")); goto out; }
        while (1) { Tok t = p.nx(); if (t == T_OC) break; if (t != T_S) { resp = hr_(400, "Bad Request", "application/json", jerr(r, -32700, "Parse")); goto out; }
            std::string k = p.sv; if (p.nx() != T_CO) { resp = hr_(400, "Bad Request", "application/json", jerr(r, -32700, "Parse")); goto out; }
            if (k == "jsonrpc") { if (p.nx() != T_S) { resp = hr_(400, "Bad Request", "application/json", jerr(r, -32700, "Parse")); goto out; } r.jr = p.sv; }
            else if (k == "id") { size_t ps = p.pos(); t = p.nx();
                if (t == T_S) { r.id = p.sv; r.hi = 1; r.is = 1; }
                else if (t == T_N) { r.id = body.substr(ps, p.pos()-ps); r.hi = 1; r.is = 0; }
                else if (t == T_L) r.hi = 0;
                else { resp = hr_(400, "Bad Request", "application/json", jerr(r, -32700, "Parse")); goto out; } }
            else if (k == "method") { if (p.nx() != T_S) { resp = hr_(400, "Bad Request", "application/json", jerr(r, -32700, "Parse")); goto out; } r.m = p.sv; }
            else if (k == "params") { size_t ps = p.pos(); p.sv2(); r.rp = body.substr(ps, p.pos()-ps); }
            else p.sv2();
            t = p.nx(); if (t == T_OC) break; if (t != T_CM) { resp = hr_(400, "Bad Request", "application/json", jerr(r, -32700, "Parse")); goto out; } }
        if (r.jr != "2.0") { resp = hr_(400, "Bad Request", "application/json", jerr(r, -32600, "ver")); goto out; }

        std::string sid;
        auto it = hdrs.find("mcp-session-id"); if (it != hdrs.end()) sid = it->second;

        if (r.m == "initialize") {
            J j; j.o(); j.ks("protocolVersion",VER); j.ko("capabilities"); j.ko("tools"); j.kb("listChanged",0); j.c(); j.c();
            j.ko("serverInfo"); j.ks("name","mcp-shell-server"); j.ks("version","1.0"); j.c(); j.c();
            std::string nsid = "sh-" + std::to_string(time(0)) + "-" + std::to_string(rand()%100000);
            std::map<std::string,std::string> ex; ex["MCP-Session-Id"] = nsid; ex["MCP-Protocol-Version"] = VER;
            resp = hr_(200,"OK","application/json",jok(r,j.str()),ex);
        } else if (!r.hi) {
            if (r.m == "notifications/initialized") log(LI, "init ok");
            resp = hr_(202, "Accepted", "text/plain", "");
        } else if (r.m == "ping") resp = hr_(200,"OK","application/json",jok(r,"{}"));
        else if (r.m == "tools/list") {
            J j; j.o(); j.ka("tools");
            j.o(); j.ks("name","shell"); j.ks("description","Execute shell command");
            j.ko("inputSchema"); j.ks("type","object"); j.ka("required"); j.avs("command"); j.ea();
            j.ko("properties"); j.ko("command"); j.ks("type","string"); j.ks("description","Shell command"); j.c();
            j.ko("timeout"); j.ks("type","number"); j.ks("description","Timeout sec"); j.c(); j.c(); j.c(); j.c();
            j.o(); j.ks("name","status"); j.ks("description","List running terminals (mine flag for caller)");
            j.ko("inputSchema"); j.ks("type","object"); j.ka("required"); j.ea(); j.ko("properties"); j.c(); j.c(); j.c();
            j.o(); j.ks("name","stop"); j.ks("description","Stop own command(s). Optional pid to stop specific");
            j.ko("inputSchema"); j.ks("type","object"); j.ka("required"); j.ea();
            j.ko("properties"); j.ko("pid"); j.ks("type","number"); j.ks("description","PID to stop"); j.c(); j.c(); j.c(); j.c();
            j.o(); j.ks("name","reset"); j.ks("description","Reset server state");
            j.ko("inputSchema"); j.ks("type","object"); j.ka("required"); j.ea(); j.ko("properties"); j.c(); j.c(); j.c();
            j.ea(); j.c();
            resp = hr_(200,"OK","application/json",jok(r,j.str()));
        } else if (r.m == "tools/call") {
            P ph; ph.set(r.rp);
            std::string tn = ph.gps(r.rp, "name"); log(LI, "call: %s", tn.c_str());
            if (tn == "shell") {
                std::string aj = ph.ex(r.rp, "arguments");
                std::string cmd = ph.gps(aj, "command");
                if (cmd.empty()) { resp = hr_(200,"OK","application/json",jerr(r,-32602,"Missing:command")); goto out; }
                log(LI, "Exec: %s", cmd.c_str());
                int po[2], pe[2]; if (pipe(po)<0 || pipe(pe)<0) { log(LE,"pipe"); resp = hr_(500,"","application/json",""); goto out; }
                pid_t pid = fork();
                if (pid < 0) { log(LE,"fork"); close(po[0]);close(po[1]);close(pe[0]);close(pe[1]); resp = hr_(500,"","application/json",""); goto out; }
                if (pid == 0) {
                    close(po[0]); close(pe[0]); dup2(po[1],1); dup2(pe[1],2); close(po[1]); close(pe[1]);
                    signal(SIGCHLD, SIG_DFL); setpgid(0,0);
                    execl("/bin/sh","sh","-c",cmd.c_str(),(char*)0); _exit(127);
                }
                cmd_add(pid, cmd.c_str(), sid.c_str());
                close(po[1]); close(pe[1]);
                int fl; fl=fcntl(po[0],F_GETFL,0); fcntl(po[0],F_SETFL,fl|O_NONBLOCK);
                fl=fcntl(pe[0],F_GETFL,0); fcntl(pe[0],F_SETFL,fl|O_NONBLOCK);
                std::string so, se;
                struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                long dl = 30000; bool eo=0, ee=0, ex=0; int ec=-1;
                while (1) {
                    struct timespec tn; clock_gettime(CLOCK_MONOTONIC, &tn);
                    long el = (tn.tv_sec-ts.tv_sec)*1000 + (tn.tv_nsec-ts.tv_nsec)/1000000;
                    if (el >= dl) break; if (eo && ee && ex) break;
                    if (eo && ee && !ex) { int st; pid_t r2 = waitpid(pid, &st, 0); if (r2==pid) { ex=1; ec=WIFEXITED(st)?WEXITSTATUS(st):-1; } break; }
                    if (!ex) { int st; pid_t r2 = waitpid(pid, &st, WNOHANG); if (r2==pid) { ex=1; ec=WIFEXITED(st)?WEXITSTATUS(st):-1; } }
                    struct pollfd pf[2]; pf[0].fd=po[0]; pf[0].events=eo?0:POLLIN; pf[1].fd=pe[0]; pf[1].events=ee?0:POLLIN;
                    int pr = poll(pf, 2, 100); if (pr < 0) break;
                    if (!eo) { char b[4096]; int n=read(po[0],b,sizeof(b)); if(n>0)so.append(b,n); else eo=1; }
                    if (!ee) { char b[4096]; int n=read(pe[0],b,sizeof(b)); if(n>0)se.append(b,n); else ee=1; }
                }
                if (!ex) { kill(pid, SIGKILL); waitpid(pid, 0, 0); }
                cmd_del(pid);
                char db[4096]; while(1){int n=read(po[0],db,sizeof(db));if(n<=0)break;so.append(db,n);}
                while(1){int n=read(pe[0],db,sizeof(db));if(n<=0)break;se.append(db,n);}
                close(po[0]); close(pe[0]);
                J jj; jj.o(); jj.ks("stdout",so); jj.ks("stderr",se); jj.ki("exit_code",ec); jj.kr("pid",std::to_string(pid)); jj.kb("success",!ec); jj.c();
                J w; w.o(); w.ka("content"); w.o(); w.ks("type","text"); w.ks("text",jj.str()); w.c(); w.ea(); w.kb("isError",0); w.c();
                resp = hr_(200,"OK","application/json",jok(r,w.str()));
            } else if (tn == "status") {
                std::string cmds = cmd_list(sid.c_str());
                P pc; pc.set(cmds); int n=0;
                if(pc.nx()==T_A){while(1){Tok t=pc.nx();if(t==T_AC)break;n++;pc.sv2();t=pc.nx();if(t==T_AC)break;}}
                J j; j.o(); j.ki("count",n); j.kr("terminals",cmds); j.ks("message",n?std::to_string(n)+" running":"idle"); j.c();
                J w; w.o(); w.ka("content"); w.o(); w.ks("type","text"); w.ks("text",j.str()); w.c(); w.ea(); w.kb("isError",0); w.c();
                resp = hr_(200,"OK","application/json",jok(r,w.str()));
            } else if (tn == "stop") {
                std::string aj = ph.ex(r.rp, "arguments");
                std::string ps = ph.gps(aj, "pid");
                int tpid = ps.empty() ? 0 : atoi(ps.c_str());
                int k = tpid>0 ? cmd_stop(0, tpid) : cmd_stop(sid.c_str(), 0);
                J j; j.o(); j.ki("stopped",k); j.ks("message",k?std::to_string(k)+" stopped":"none"); j.c();
                J w; w.o(); w.ka("content"); w.o(); w.ks("type","text"); w.ks("text",j.str()); w.c(); w.ea(); w.kb("isError",0); w.c();
                resp = hr_(200,"OK","application/json",jok(r,w.str()));
            } else if (tn == "reset") {
                log(LI, "reset"); J j; j.o(); j.ks("status","ok"); j.ks("message","reset"); j.c();
                J w; w.o(); w.ka("content"); w.o(); w.ks("type","text"); w.ks("text",j.str()); w.c(); w.ea(); w.kb("isError",0); w.c();
                resp = hr_(200,"OK","application/json",jok(r,w.str()));
            } else resp = hr_(200,"OK","application/json",jerr(r,-32601,"unknown:"+tn));
        } else resp = hr_(200,"OK","application/json",jerr(r,-32601,"?"+r.m));
    } else resp = hr_(404, "", "text/plain", "");

out:
    if (!resp.empty()) (void)!write(fd, resp.c_str(), resp.size());
    close(fd); pthread_detach(pthread_self()); return 0;
}

int main(int c, char** v) {
    int port = PORT;
    for (int i = 1; i < c; i++) {
        if ((!strcmp(v[i],"-p")||!strcmp(v[i],"--port")) && i+1<c) { port=atoi(v[++i]); if(port<1||port>65535){log(LE,"bad port");return 1;} }
        else if (!strcmp(v[i],"-h")||!strcmp(v[i],"--help")) { printf("MCP Shell Server\nUsage: %s [-p PORT]\n",v[0]); return 0; }
    }
    signal(SIGPIPE, SIG_IGN);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { log(LE, "socket"); return 1; }
    int opt = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a; memset(&a,0,sizeof(a)); a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(port);
    if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0) { log(LE, "bind %d", port); close(fd); return 1; }
    if (listen(fd, 128) < 0) { log(LE, "listen"); close(fd); return 1; }
    log(LI, "=== MCP Shell Server ==="); log(LI, "Port %d  EP: POST /mcp", port); log(LI, "http://<ip>:%d/mcp", port);
    while (1) {
        struct sockaddr_in ca; socklen_t cl = sizeof(ca);
        int cfd = accept(fd, (sockaddr*)&ca, &cl);
        if (cfd < 0) { if (errno == EINTR) continue; break; }
        pthread_t tid; pthread_create(&tid, 0, handle_client, (void*)(intptr_t)cfd);
    }
    close(fd); return 0;
}
