// ============================================================================
// shya WASM 入口（库式 API）
//
// 目的：把编译器从 CLI 形态包一层库式接口，供浏览器 / 无名杀扩展直接调用，
//       不依赖 argv / 文件系统（主源码以字符串传入）。
//
// 导出：
//   const char* shya_compile(const char* source, const char* filename)
//       返回 malloc 出来的 JSON（UTF-8，NUL 结尾），形如：
//       {
//         "ok": true,
//         "code": "<产物 JS>",
//         "diagnostics": [
//           {"severity":"error","line":3,"col":5,"offset":17,"code":"TC003","message":"..."}
//         ],
//         "rendered": "<带源码片段的诊断文本>"
//       }
//       调用方读完 JSON 后必须调 shya_free() 释放。
//   void shya_free(char* p)
//
// 注意：本文件不改动任何既有文件，也不改变编译器的语言行为；
//       它只是新增一个入口。`import "./x.shya"` 依赖虚拟文件系统（见 build-wasm 脚本
//       的 --embed-file 与 JS 侧的 MEMFS 预处理）。
// ============================================================================

#include "shya.h"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include <emscripten/emscripten.h>

namespace {

std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

const char* severityName(shya::Severity s) {
    switch (s) {
        case shya::Severity::Error:   return "error";
        case shya::Severity::Warning: return "warning";
        case shya::Severity::Note:    return "note";
    }
    return "error";
}

std::string toJson(const shya::CompileResult& r, const std::string& source,
                   const std::string& filename) {
    std::ostringstream os;
    os << "{\"ok\":" << (r.ok ? "true" : "false");
    os << ",\"code\":\"" << jsonEscape(r.code) << "\"";
    os << ",\"diagnostics\":[";
    const std::vector<shya::Diagnostic>& items = r.diags.items();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const shya::Diagnostic& d = items[i];
        if (i) os << ',';
        os << "{\"severity\":\"" << severityName(d.severity) << "\""
           << ",\"line\":" << d.pos.line
           << ",\"col\":" << d.pos.col
           << ",\"offset\":" << d.pos.offset
           << ",\"code\":\"" << jsonEscape(d.code) << "\""
           << ",\"message\":\"" << jsonEscape(d.message) << "\"}";
    }
    os << "]";
    std::string rendered = shya::renderDiagnostics(r.diags, source, filename);
    os << ",\"rendered\":\"" << jsonEscape(rendered) << "\"}";
    return os.str();
}

}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE
const char* shya_compile(const char* source, const char* filename) {
    const std::string src = source ? std::string(source) : std::string();
    const std::string file = (filename && *filename) ? std::string(filename)
                                                     : std::string("/work/input.shya");
    shya::CompileOptions opt;
    // 与 CLI 的 build 命令语义一致：不做 dump，保留默认类型检查
    const shya::CompileResult result = shya::compileSource(src, file, opt);
    const std::string json = toJson(result, src, file);
    char* out = static_cast<char*>(std::malloc(json.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, json.c_str(), json.size() + 1);
    return out;
}

EMSCRIPTEN_KEEPALIVE
void shya_free(char* p) {
    std::free(p);
}

}  // extern "C"
