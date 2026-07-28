/*
 * mcp-shell-server.cpp - ARM64 Static MCP Shell Server
 * 
 * Protocol: MCP 2025-11-25 (Streamable HTTP, JSON-RPC 2.0)
 * Endpoint: POST /mcp
 * Tool: shell(command) -> {stdout, stderr, exit_code}
 * Tool: reset -> reset server state
 * Tool: status -> check running command
 * Tool: stop -> SIGINT current command
 *
 * Build: g++ -std=c++11 -O2 -static -o mcp-shell-server mcp-shell-server.cpp -lpthread
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
    va_list args; va_start(args, fmt);
    const char* pfx = level == LOG_INFO ? "[I] " : level == LOG_WARN ? "[W] " : "[E] ";
    fprintf(stderr, "%s", pfx); vfprintf(stderr, fmt, args); fprintf(stderr, "\n"); fflush(stderr);
    va_end(args);
}

class JsonBuilder {
    std::string buf;
    void add_sep() { if (!buf.empty() && buf.back() != '{' && buf.back() != '[') buf += ','; }
    static std::string esc(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); i++) {
            char c = s[i];
            switch (c) {
                case '"': out += "\\\""; break; case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break; case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: if ((unsigned char)c < 0x20) { char h[8]; snprintf(h,8,"\\u%04x",(unsigned char)c); out+=h; } else out+=c;
            }
        }
        return out;
    }
public:
    JsonBuilder() { buf=""; }
    void clear() { buf=""; }
    void begin_object() { if (!buf.empty() && buf.back() == '}') buf += ','; buf += '{'; }
    void end_object() { buf += '}'; }
    void begin_array() { if (!buf.empty() && buf.back() == ']') buf += ','; buf += '['; }
    void end_array() { buf += ']'; }
    void key(const std::string& k) { add_sep(); buf += '"' + esc(k) + '"' + ':'; }
    void value_string(const std::string& v) { buf += '"' + esc(v) + '"'; }
    void value_int(long v) { buf += std::to_string(v); }
    void value_bool(bool v) { buf += (v ? "true" : "false"); }
    void value_null() { buf += "null"; }
    void value_raw(const std::string& r) { buf += r; }
    void av_string(const std::string& v) { add_sep(); buf += '"' + esc(v) + '"'; }
    void av_int(long v) { add_sep(); buf += std::to_string(v); }
    void av_bool(bool v) { add_sep(); buf += (v ? "true" : "false"); }
    void av_raw(const std::string& r) { add_sep(); buf += r; }
    void av_object() { add_sep(); buf += '{'; }
    void av_array() { add_sep(); buf += '['; }
    void key_string(const std::string& k, const std::string& v) { key(k); value_string(v); }
    void key_int(const std::string& k, long v) { key(k); value_int(v); }
    void key_bool(const std::string& k, bool v) { key(k); value_bool(v); }
    void key_null(const std::string& k) { key(k); value_null(); }
    void key_raw(const std::string& k, const std::string& r) { key(k); value_raw(r); }
    void key_object(const std::string& k) { key(k); begin_object(); }
    void key_array(const std::string& k) { key(k); begin_array(); }
    std::string str() const { return buf; }
};

enum JsonToken { TOK_EOF, TOK_OBJECT_BEGIN, TOK_OBJECT_END, TOK_ARRAY_BEGIN, TOK_ARRAY_END,
    TOK_STRING, TOK_NUMBER, TOK_TRUE, TOK_FALSE, TOK_NULL, TOK_COLON, TOK_COMMA, TOK_ERROR };

class JsonParser {
    const char *p, *end, *start;
    void skip_ws() { while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++; }
public:
    std::string string_value; long number_value; bool is_negative;
    JsonParser() : p(nullptr), end(nullptr), start(nullptr), number_value(0), is_negative(false) {}
    void set_input(const std::string& in) { p=in.c_str(); end=p+in.size(); start=p; }
    size_t pos() const { return (size_t)(p-start); }
    JsonToken next() {
        skip_ws(); if (p>=end) return TOK_EOF;
        switch(*p) {
            case '{': p++; return TOK_OBJECT_BEGIN; case '}': p++; return TOK_OBJECT_END;
            case '[': p++; return TOK_ARRAY_BEGIN; case ']': p++; return TOK_ARRAY_END;
            case ':': p++; return TOK_COLON; case ',': p++; return TOK_COMMA;
            case '"': return parse_string();
            case 't': return parse_literal("true",TOK_TRUE);
            case 'f': return parse_literal("false",TOK_FALSE);
            case 'n': return parse_literal("null",TOK_NULL);
            case '-': case '0'...'9': return parse_number();
            default: return TOK_ERROR;
        }
    }
    void skip_value() { skip_token(next()); }
    void skip_token(JsonToken t) {
        int d=0;
        switch(t) {
            case TOK_OBJECT_BEGIN: d=1; while(d>0){JsonToken n=next();if(n==TOK_OBJECT_BEGIN)d++;else if(n==TOK_OBJECT_END)d--;else if(n==TOK_EOF)return;}break;
            case TOK_ARRAY_BEGIN: d=1; while(d>0){JsonToken n=next();if(n==TOK_ARRAY_BEGIN)d++;else if(n==TOK_ARRAY_END)d--;else if(n==TOK_EOF)return;}break;
            default: break;
        }
    }
    std::string extract_raw_value(const std::string& j, const std::string& tk) {
        JsonParser p2; p2.set_input(j);
        if (p2.next()!=TOK_OBJECT_BEGIN) return "";
        while (true) {
            JsonToken t=p2.next(); if(t==TOK_OBJECT_END) break; if(t!=TOK_STRING) return "";
            std::string k=p2.string_value; if(p2.next()!=TOK_COLON) return "";
            if(k==tk) { size_t vs=p2.pos(); p2.skip_value(); return j.substr(vs,p2.pos()-vs); }
            p2.skip_value(); t=p2.next(); if(t==TOK_OBJECT_END) break;
        }
        return "";
    }
    std::string get_param_string(const std::string& j, const std::string& k) {
        std::string r=extract_raw_value(j,k); if(r.empty()) return "";
        JsonParser sp; sp.set_input(r);
        return (sp.next()==TOK_STRING) ? sp.string_value : "";
    }
private:
    JsonToken parse_string() {
        p++; string_value.clear();
        while(p<end && *p!='"') {
            if(*p=='\\') { p++; if(p>=end) return TOK_ERROR;
                switch(*p) {
                    case'"':string_value+='"';break; case'\\':string_value+='\\';break;
                    case'/':string_value+='/';break; case'b':string_value+='\b';break;
                    case'f':string_value+='\f';break; case'n':string_value+='\n';break;
                    case'r':string_value+='\r';break; case't':string_value+='\t';break;
                    case'u':{char h[5]={0};if(p+4>=end)return TOK_ERROR;memcpy(h,p+1,4);string_value+="\\u";string_value+=h;p+=4;break;}
                    default:string_value+=*p;
                }
                p++;
            } else { string_value+=*p; p++; }
        }
        if(p>=end) return TOK_ERROR; p++; return TOK_STRING;
    }
    JsonToken parse_literal(const char* e, JsonToken t) {
        size_t l=strlen(e); if((size_t)(end-p)<l) return TOK_ERROR; if(strncmp(p,e,l)!=0) return TOK_ERROR; p+=l; return t;
    }
    JsonToken parse_number() {
        is_negative=false; number_value=0;
        if(*p=='-'){is_negative=true;p++;}
        while(p<end && *p>='0' && *p<='9'){number_value=number_value*10+(*p-'0');p++;}
        if(p<end && *p=='.'){p++;while(p<end && *p>='0' && *p<='9')p++;}
        if(p<end && (*p=='e'||*p=='E')){p++;if(p<end && (*p=='+'||*p=='-'))p++;while(p<end && *p>='0' && *p<='9')p++;}
        if(is_negative) number_value=-number_value; return TOK_NUMBER;
    }
};

struct MCPRequest { std::string jsonrpc, id_str; bool has_id=false, id_is_string=false; std::string method, raw_params; };

static size_t ifind(const std::string& h, const std::string& n, size_t p=0) {
    if (n.empty()) return std::string::npos;
    for (size_t i=p; i+n.size()<=h.size(); i++) { bool m=true; for(size_t j=0;j<n.size();j++)if(tolower(h[i+j])!=tolower(n[j])){m=false;break;} if(m) return i; }
    return std::string::npos;
}

class MCPServer {
    int server_fd, port; bool initialized; JsonParser ph; std::string sid;
    std::string gen_sid() { char b[64]; snprintf(b,64,"mcp-shell-%ld-%d",(long)time(nullptr),rand()%100000); return b; }
    size_t find_eol(const std::string& s, size_t p) {
        size_t c=s.find("\r\n",p), l=s.find("\n",p);
        if(c!=std::string::npos && l!=std::string::npos) return c<l?c:l;
        return c!=std::string::npos ? c : l;
    }
    size_t find_hdr_end(const std::string& s) {
        size_t c=s.find("\r\n\r\n"), l=s.find("\n\n");
        if(c!=std::string::npos && l!=std::string::npos) return c<l?c+4:l+2;
        if(c!=std::string::npos) return c+4; if(l!=std::string::npos) return l+2;
        return std::string::npos;
    }
    bool parse_http(const std::string& raw, std::string& m, std::string& pth, std::map<std::string,std::string>& h, std::string& body) {
        size_t le=find_eol(raw,0); if(le==std::string::npos) return false;
        std::string rl=raw.substr(0,le); if(!rl.empty() && rl.back()=='\r') rl.pop_back();
        size_t m1=rl.find(' '), m2=rl.rfind(' ');
        if(m1==std::string::npos||m2==std::string::npos||m2==m1) return false;
        m=rl.substr(0,m1); pth=rl.substr(m1+1,m2-m1-1);
        size_t pos=le+((raw[le]=='\r')?2:1);
        while(pos<raw.size()) {
            size_t he=find_eol(raw,pos); if(he==std::string::npos) break;
            if(he==pos||(he==pos+1&&raw[pos]=='\r')){body=raw.substr(he+((raw[he]=='\r')?2:1));return true;}
            std::string hl=raw.substr(pos,he-pos); if(!hl.empty()&&hl.back()=='\r') hl.pop_back();
            size_t co=hl.find(':'); if(co!=std::string::npos) {
                std::string hn=hl.substr(0,co), hv=hl.substr(co+1);
                size_t vs=hv.find_first_not_of(" \t"); if(vs!=std::string::npos) hv=hv.substr(vs);
                size_t ve=hv.find_last_not_of(" \t\r"); if(ve!=std::string::npos) hv=hv.substr(0,ve+1);
                std::transform(hn.begin(),hn.end(),hn.begin(),::tolower); h[hn]=hv;
            }
            pos=he+((raw[he]=='\r')?2:1);
        }
        return false;
    }
    std::string http_resp(int c, const std::string& txt, const std::string& ct, const std::string& b, const std::map<std::string,std::string>& ex={}) {
        std::string r="HTTP/1.1 "+std::to_string(c)+" "+txt+"\r\nContent-Type: "+ct+"\r\nContent-Length: "+std::to_string(b.size())+"\r\nAccess-Control-Allow-Origin: *\r\n";
        for(auto& h: ex) r+=h.first+": "+h.second+"\r\n"; r+="\r\n"+b; return r;
    }
    std::string jerr(const MCPRequest& r, int c, const std::string& m) {
        JsonBuilder j; j.begin_object(); j.key_string("jsonrpc","2.0");
        if(!r.has_id) j.key_null("id"); else if(r.id_is_string) j.key_string("id",r.id_str); else j.key_raw("id",r.id_str);
        j.key_object("error"); j.key_int("code",c); j.key_string("message",m); j.end_object(); j.end_object(); return j.str();
    }
    std::string jok(const MCPRequest& r, const std::string& res) {
        JsonBuilder j; j.begin_object(); j.key_string("jsonrpc","2.0");
        if(!r.has_id) j.key_null("id"); else if(r.id_is_string) j.key_string("id",r.id_str); else j.key_raw("id",r.id_str);
        j.key_raw("result",res); j.end_object(); return j.str();
    }
    // PID file helpers
    static pid_t read_run_pid() { FILE* f=fopen("/tmp/mcp-run.pid","r"); if(!f) return 0; pid_t p=0; (void)fscanf(f,"%d",&p); fclose(f); return p; }
    static void write_run_pid(pid_t p) { FILE* f=fopen("/tmp/mcp-run.pid","w"); if(f){fprintf(f,"%d",p);fclose(f);} }
    static void clear_run_pid() { unlink("/tmp/mcp-run.pid"); }
    static bool is_pid_alive(pid_t p) { return p>0 && kill(p,0)==0; }

    bool parse_jrpc(const std::string& body, MCPRequest& r) {
        JsonParser p; p.set_input(body); if(p.next()!=TOK_OBJECT_BEGIN) return false;
        while(true) {
            JsonToken t=p.next(); if(t==TOK_OBJECT_END) break; if(t!=TOK_STRING) return false;
            std::string k=p.string_value; if(p.next()!=TOK_COLON) return false;
            if(k=="jsonrpc"){if(p.next()!=TOK_STRING)return false;r.jsonrpc=p.string_value;}
            else if(k=="id"){size_t ps=p.pos();t=p.next();if(t==TOK_STRING){r.id_str=p.string_value;r.has_id=true;r.id_is_string=true;}else if(t==TOK_NUMBER){r.id_str=body.substr(ps,p.pos()-ps);r.has_id=true;r.id_is_string=false;}else if(t==TOK_NULL)r.has_id=false;else return false;}
            else if(k=="method"){if(p.next()!=TOK_STRING)return false;r.method=p.string_value;}
            else if(k=="params"){size_t ps=p.pos();p.skip_value();r.raw_params=body.substr(ps,p.pos()-ps);} else p.skip_value();
            t=p.next(); if(t==TOK_OBJECT_END) break; if(t!=TOK_COMMA) return false;
        }
        return !r.method.empty();
    }
