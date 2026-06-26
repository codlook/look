#include "look/template.h"
#include "look/stdlib.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;
namespace look {

// ──────────────────────────────────────────────────────────────────────────────
// Utilities
// ──────────────────────────────────────────────────────────────────────────────

static std::string tpl_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string TemplateEngine::html_escape(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  r += "&amp;";  break;
            case '<':  r += "&lt;";   break;
            case '>':  r += "&gt;";   break;
            case '"':  r += "&quot;"; break;
            case '\'': r += "&#39;";  break;
            default:   r += c;
        }
    }
    return r;
}

std::string TemplateEngine::to_str(const Value& v) {
    switch (v.type()) {
        case Value::INT:    return std::to_string(v.as_int());
        case Value::FLOAT: {
            std::ostringstream oss;
            double d = v.as_float();
            long long li = (long long)d;
            if ((double)li == d) oss << li;
            else                 oss << d;
            return oss.str();
        }
        case Value::BOOL:   return v.as_bool() ? "true" : "false";
        case Value::STRING: return v.as_string();
        default:            return "";
    }
}

bool TemplateEngine::is_truthy(const Value& v) {
    switch (v.type()) {
        case Value::INT:    return v.as_int() != 0;
        case Value::FLOAT:  return v.as_float() != 0.0;
        case Value::STRING: return !v.as_string().empty();
        case Value::BOOL:   return v.as_bool();
        case Value::ARRAY:  return v.as_array() && !v.as_array()->empty();
        case Value::NONE:   return false;
        default:            return true;
    }
}

// Resolve "varname", "varname.field", "varname.field.sub"
Value TemplateEngine::resolve(const std::string& path, const TplContext& ctx) {
    if (path.empty()) return Value();

    // Split by '.'
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '.') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else           cur += c;
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return Value();

    auto it = ctx.find(parts[0]);
    if (it == ctx.end()) return Value();
    Value v = it->second;

    for (size_t i = 1; i < parts.size(); ++i) {
        if (!v.as_array()) return Value();
        const auto& arr = *v.as_array();
        const std::string& key = parts[i];

        // Assoc array: [sentinel, k0, v0, k1, v1, ...]
        // Sentinel is a special string "__assoc__" or "__struct__" at index 0
        size_t start = 0;
        bool   is_assoc = !arr.empty() &&
                          arr[0].type() == Value::STRING &&
                          (arr[0].as_string() == "__assoc__" ||
                           arr[0].as_string() == "__struct__");
        if (is_assoc) start = 1;

        // Try key lookup in k/v pairs
        bool found = false;
        for (size_t j = start; j + 1 < arr.size(); j += 2) {
            if (arr[j].type() == Value::STRING && arr[j].as_string() == key) {
                v = arr[j + 1];
                found = true;
                break;
            }
        }
        if (!found) {
            // Try numeric index in plain array
            bool is_num = !key.empty() &&
                          std::all_of(key.begin(), key.end(), ::isdigit);
            if (is_num) {
                int idx = std::stoi(key);
                if (idx >= 0 && (size_t)idx < arr.size()) { v = arr[idx]; found = true; }
            }
        }
        if (!found) return Value();
    }
    return v;
}

// ──────────────────────────────────────────────────────────────────────────────
// Condition evaluation
// Supports: $var, !$var, $var == "x", $var != "x", $var > 0, etc.
// ──────────────────────────────────────────────────────────────────────────────

static Value parse_rhs(const std::string& rhs, const TplContext& ctx) {
    std::string r = tpl_trim(rhs);
    if (r.size() >= 2 && r.front() == '"' && r.back() == '"')
        return Value(r.substr(1, r.size() - 2));
    if (!r.empty() && r[0] == '$')
        return TemplateEngine::resolve(r.substr(1), ctx);
    try { return Value((int)std::stoll(r)); } catch (...) {}
    try { return Value(std::stod(r)); }        catch (...) {}
    return Value(r);
}

bool TemplateEngine::eval_cond(const std::string& cond, const TplContext& ctx) {
    std::string s = tpl_trim(cond);

    bool negate = false;
    if (!s.empty() && s[0] == '!') {
        negate = true;
        s = tpl_trim(s.substr(1));
    }
    // Strip leading $
    if (!s.empty() && s[0] == '$') s = s.substr(1);

    // Look for comparison operators (order matters: >= before >, etc.)
    static const char* ops[] = { ">=", "<=", "!=", "==", ">", "<", nullptr };
    for (int i = 0; ops[i]; ++i) {
        std::string op = ops[i];
        size_t pos = s.find(op);
        if (pos == std::string::npos) continue;

        std::string lhs_str = tpl_trim(s.substr(0, pos));
        std::string rhs_str = tpl_trim(s.substr(pos + op.size()));

        if (!lhs_str.empty() && lhs_str[0] == '$') lhs_str = lhs_str.substr(1);

        Value lv = resolve(lhs_str, ctx);
        Value rv = parse_rhs(rhs_str, ctx);

        bool result;
        if (op == "==" || op == "!=") {
            bool eq = (to_str(lv) == to_str(rv));
            result = (op == "==") ? eq : !eq;
        } else {
            auto to_dbl = [](const Value& v) -> double {
                if (v.type() == Value::INT)   return (double)v.as_int();
                if (v.type() == Value::FLOAT) return v.as_float();
                try { return std::stod(TemplateEngine::to_str(v)); } catch (...) { return 0.0; }
            };
            double ld = to_dbl(lv), rd = to_dbl(rv);
            if      (op == ">")  result = ld > rd;
            else if (op == "<")  result = ld < rd;
            else if (op == ">=") result = ld >= rd;
            else                 result = ld <= rd;
        }
        return negate ? !result : result;
    }

    // Simple truthy check
    Value v = resolve(s, ctx);
    return negate ? !is_truthy(v) : is_truthy(v);
}

// ──────────────────────────────────────────────────────────────────────────────
// Parser
// ──────────────────────────────────────────────────────────────────────────────

struct TplParser {
    const std::string& src;
    size_t             pos = 0;
    std::string        origin;

    TplParser(const std::string& s, const std::string& o) : src(s), pos(0), origin(o) {}

    bool at_end() const { return pos >= src.size(); }

    // Read characters until '}', consuming the '}'
    std::string read_until_close(const std::string& ctx_msg) {
        size_t start = pos;
        while (pos < src.size() && src[pos] != '}') ++pos;
        if (pos >= src.size())
            throw std::runtime_error("Template parse error in '" + origin +
                                     "': unclosed '{' near: " + ctx_msg);
        std::string content = src.substr(start, pos - start);
        ++pos; // consume '}'
        return content;
    }

    // Validate a variable path — no expressions or function calls allowed
    void validate_var_path(const std::string& path) {
        for (char c : path) {
            if (c == '(' || c == ')' || c == '+' || c == '*' || c == '%' ||
                c == '/' || c == '!' || c == '&' || c == '|' || c == '?') {
                throw std::runtime_error(
                    "Template hatası: '{$" + path + "}' içinde ifade veya fonksiyon "
                    "çağrısı kullanılamaz. Hesaplamayı .lk dosyasında yapın.");
            }
            // Disallow :: (module calls)
            if (c == ':' && path.find("::") != std::string::npos) {
                throw std::runtime_error(
                    "Template hatası: '{$" + path + "}' içinde modül çağrısı yapılamaz.");
            }
        }
    }

    // Try to read a template block at current position.
    // Returns true and sets kind/args if this is a template directive.
    // Returns false if '{' is literal text.
    // kind: "var", "rawvar", "#if", "#else", "#each", "#empty",
    //       "#extends", "#block", "#include", "/if", "/each", "/block"
    bool try_block(std::string& kind, std::string& args) {
        if (pos >= src.size() || src[pos] != '{') return false;
        size_t save = pos;
        ++pos; // consume '{'

        if (pos >= src.size()) { pos = save; return false; }
        char c = src[pos];

        // {!$var} — raw (unescaped) variable
        if (c == '!' && pos + 1 < src.size() && src[pos+1] == '$') {
            pos += 2; // consume '!$'
            std::string path = tpl_trim(read_until_close("!$..."));
            validate_var_path(path);
            kind = "rawvar";
            args = path;
            return true;
        }

        // {$var} — escaped variable
        if (c == '$') {
            ++pos; // consume '$'
            std::string path = tpl_trim(read_until_close("$..."));
            validate_var_path(path);
            kind = "var";
            args = path;
            return true;
        }

        // {#directive args} — block start
        if (c == '#') {
            ++pos; // consume '#'
            std::string inner = tpl_trim(read_until_close("#..."));
            auto sp = inner.find(' ');
            std::string directive = sp == std::string::npos ? inner : inner.substr(0, sp);
            args = sp == std::string::npos ? "" : tpl_trim(inner.substr(sp + 1));
            kind = "#" + directive;
            return true;
        }

        // {/directive} — block end
        if (c == '/') {
            ++pos; // consume '/'
            std::string inner = tpl_trim(read_until_close("/..."));
            kind = "/" + inner;
            args = "";
            return true;
        }

        // Not a template directive — restore and treat as literal
        pos = save;
        return false;
    }

    // Parse nodes until EOF or until a directive in `stops` is found.
    // Returns the stop directive encountered, or "" for EOF.
    std::string parse_nodes(std::vector<TplNode>& out,
                             const std::vector<std::string>& stops = {}) {
        std::string text_buf;

        auto flush_text = [&]() {
            if (!text_buf.empty()) {
                out.push_back({TplNodeKind::Text, text_buf});
                text_buf.clear();
            }
        };

        while (!at_end()) {
            if (src[pos] != '{') {
                text_buf += src[pos++];
                continue;
            }

            std::string kind, args;
            if (!try_block(kind, args)) {
                text_buf += src[pos++]; // literal '{'
                continue;
            }

            // Check if this is a stop directive
            for (const auto& stop : stops) {
                if (kind == stop) {
                    flush_text();
                    return kind;
                }
            }

            flush_text();

            if (kind == "var") {
                out.push_back({TplNodeKind::Var, args});
            }
            else if (kind == "rawvar") {
                out.push_back({TplNodeKind::RawVar, args});
            }
            else if (kind == "#if") {
                TplNode node;
                node.kind  = TplNodeKind::If;
                node.extra = args; // condition string
                std::string stop = parse_nodes(node.children, {"#else", "/if"});
                if (stop == "#else") {
                    parse_nodes(node.alt, {"/if"});
                }
                out.push_back(std::move(node));
            }
            else if (kind == "#each") {
                // expected: "$arr as $item"
                TplNode node;
                node.kind = TplNodeKind::Each;
                auto as_pos = args.find(" as ");
                if (as_pos == std::string::npos)
                    throw std::runtime_error(
                        "Template hatası (" + origin + "): {#each} sözdizimi hatası. "
                        "Doğru kullanım: {#each $liste as $eleman}");
                std::string arr_part  = tpl_trim(args.substr(0, as_pos));
                std::string item_part = tpl_trim(args.substr(as_pos + 4));
                if (!arr_part.empty()  && arr_part[0]  == '$') arr_part  = arr_part.substr(1);
                if (!item_part.empty() && item_part[0] == '$') item_part = item_part.substr(1);
                node.text  = arr_part;   // array variable path
                node.extra = item_part;  // item variable name
                std::string stop = parse_nodes(node.children, {"#empty", "/each"});
                if (stop == "#empty") {
                    parse_nodes(node.alt, {"/each"});
                }
                out.push_back(std::move(node));
            }
            else if (kind == "#extends") {
                std::string path = args;
                if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
                    path = path.substr(1, path.size() - 2);
                out.push_back({TplNodeKind::Extends, path});
            }
            else if (kind == "#block") {
                std::string name = args;
                if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                    name = name.substr(1, name.size() - 2);
                TplNode node;
                node.kind = TplNodeKind::Block;
                node.text = name;
                parse_nodes(node.children, {"/block"});
                out.push_back(std::move(node));
            }
            else if (kind == "#include") {
                // {#include "path"} or {#include "path" data=$var}
                std::string path_str = args;
                std::string data_var;
                auto data_pos = args.find(" data=");
                if (data_pos != std::string::npos) {
                    path_str = tpl_trim(args.substr(0, data_pos));
                    data_var = tpl_trim(args.substr(data_pos + 6));
                    if (!data_var.empty() && data_var[0] == '$')
                        data_var = data_var.substr(1);
                }
                if (path_str.size() >= 2 && path_str.front() == '"' && path_str.back() == '"')
                    path_str = path_str.substr(1, path_str.size() - 2);
                TplNode node;
                node.kind  = TplNodeKind::Include;
                node.text  = path_str;
                node.extra = data_var;
                out.push_back(std::move(node));
            }
            else if (kind[0] == '/' || kind == "#else" || kind == "#empty") {
                // Unexpected stop directive
                throw std::runtime_error(
                    "Template hatası (" + origin + "): beklenmeyen direktif '{" + kind + "}'");
            }
            else {
                throw std::runtime_error(
                    "Template hatası (" + origin + "): bilinmeyen direktif '{" + kind + "}'");
            }
        }

        flush_text();
        return ""; // EOF
    }
};

// ──────────────────────────────────────────────────────────────────────────────
// TemplateEngine public API
// ──────────────────────────────────────────────────────────────────────────────

std::vector<TplNode> TemplateEngine::parse(const std::string& src, const std::string& origin) {
    TplParser parser(src, origin);
    std::vector<TplNode> nodes;
    parser.parse_nodes(nodes);
    return nodes;
}

std::string TemplateEngine::resolve_path(const std::string& path) {
    fs::path p(path);
    // Add .html extension if no extension given
    if (!p.has_extension()) p += ".html";
    if (p.is_absolute() && fs::exists(p)) return p.string();

    // Try relative to working directory
    fs::path full = fs::current_path() / p;
    if (fs::exists(full)) return full.string();

    // Try TEMPLATE_DIR env variable
    const char* tdir = std::getenv("TEMPLATE_DIR");
    if (tdir && *tdir) {
        fs::path tp = fs::path(tdir) / p;
        if (fs::exists(tp)) return tp.string();
    }

    // Return best-guess path (will produce a clear error on open)
    return full.string();
}

void TemplateEngine::collect_blocks(const std::vector<TplNode>& nodes, TplBlocks& out) {
    for (const auto& n : nodes) {
        if (n.kind == TplNodeKind::Block) {
            if (!out.count(n.text)) // child definition wins; don't overwrite
                out[n.text] = n.children;
        }
        collect_blocks(n.children, out);
        collect_blocks(n.alt,      out);
    }
}

// Helper: load a file and render it with optional pre-collected blocks (for extends)
static std::string load_and_render(const std::string& path, const TplContext& ctx,
                                    const TplBlocks* blocks) {
    std::string full = TemplateEngine::resolve_path(path);
    std::ifstream f(full, std::ios::binary);
    if (!f.is_open())
        throw std::runtime_error("Template dosyası bulunamadı: " + full);
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto nodes = TemplateEngine::parse(src, path);
    return TemplateEngine::render(nodes, ctx, blocks);
}

std::string TemplateEngine::render_file(const std::string& path, const TplContext& ctx) {
    std::string full = resolve_path(path);
    std::ifstream f(full, std::ios::binary);
    if (!f.is_open())
        throw std::runtime_error("Template dosyası bulunamadı: " + full);
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto nodes = parse(src, path);

    // Check for {#extends} — must be at the top (first non-text node)
    for (const auto& n : nodes) {
        if (n.kind == TplNodeKind::Text) continue; // whitespace before extends is OK
        if (n.kind == TplNodeKind::Extends) {
            TplBlocks blocks;
            collect_blocks(nodes, blocks);
            return load_and_render(n.text, ctx, &blocks);
        }
        break; // non-extends first real node → not a child template
    }

    return render(nodes, ctx);
}

std::string TemplateEngine::render_string(const std::string& src, const TplContext& ctx) {
    auto nodes = parse(src, "<string>");
    for (const auto& n : nodes) {
        if (n.kind == TplNodeKind::Text) continue;
        if (n.kind == TplNodeKind::Extends) {
            TplBlocks blocks;
            collect_blocks(nodes, blocks);
            return load_and_render(n.text, ctx, &blocks);
        }
        break;
    }
    return render(nodes, ctx);
}

std::string TemplateEngine::render(const std::vector<TplNode>& nodes,
                                    const TplContext& ctx,
                                    const TplBlocks* blocks) {
    std::string out;

    for (const auto& n : nodes) {
        switch (n.kind) {
            case TplNodeKind::Text:
                out += n.text;
                break;

            case TplNodeKind::Var: {
                Value v = resolve(n.text, ctx);
                out += html_escape(to_str(v));
                break;
            }

            case TplNodeKind::RawVar: {
                Value v = resolve(n.text, ctx);
                out += to_str(v);
                break;
            }

            case TplNodeKind::If:
                if (eval_cond(n.extra, ctx))
                    out += render(n.children, ctx, blocks);
                else
                    out += render(n.alt,      ctx, blocks);
                break;

            case TplNodeKind::Each: {
                Value arr_val = resolve(n.text, ctx);
                if (arr_val.type() != Value::ARRAY || !arr_val.as_array()) {
                    out += render(n.alt, ctx, blocks); // empty branch
                    break;
                }
                const auto& arr = *arr_val.as_array();

                // Determine whether to skip sentinel at index 0
                bool is_assoc = !arr.empty() &&
                                arr[0].type() == Value::STRING &&
                                (arr[0].as_string() == "__assoc__" ||
                                 arr[0].as_string() == "__struct__");

                std::vector<const Value*> elems;
                if (is_assoc) {
                    // Assoc: [sentinel, k0, v0, k1, v1, ...] → iterate values
                    for (size_t j = 2; j < arr.size(); j += 2)
                        elems.push_back(&arr[j]);
                } else {
                    for (const auto& e : arr)
                        elems.push_back(&e);
                }

                if (elems.empty()) {
                    out += render(n.alt, ctx, blocks);
                    break;
                }
                for (const Value* elem : elems) {
                    TplContext child = ctx;
                    child[n.extra] = *elem;
                    out += render(n.children, child, blocks);
                }
                break;
            }

            case TplNodeKind::Extends:
                // Handled in render_file / render_string before this call
                break;

            case TplNodeKind::Block: {
                if (blocks) {
                    auto it = blocks->find(n.text);
                    if (it != blocks->end()) {
                        out += render(it->second, ctx, blocks);
                        break;
                    }
                }
                // No override → use default block content
                out += render(n.children, ctx, blocks);
                break;
            }

            case TplNodeKind::Include: {
                // Build include context
                TplContext inc_ctx;
                if (!n.extra.empty()) {
                    // data=$var → use that var as context (assoc array → k/v pairs)
                    Value data = resolve(n.extra, ctx);
                    if (data.type() == Value::ARRAY && data.as_array()) {
                        const auto& arr = *data.as_array();
                        size_t start = 0;
                        if (!arr.empty() &&
                            arr[0].type() == Value::STRING &&
                            (arr[0].as_string() == "__assoc__" ||
                             arr[0].as_string() == "__struct__"))
                            start = 1;
                        for (size_t j = start; j + 1 < arr.size(); j += 2) {
                            if (arr[j].type() == Value::STRING)
                                inc_ctx[arr[j].as_string()] = arr[j+1];
                        }
                    }
                } else {
                    inc_ctx = ctx; // inherit parent context
                }
                try {
                    out += render_file(n.text, inc_ctx);
                } catch (const std::exception& e) {
                    throw std::runtime_error(
                        std::string("Template include hatası (") + n.text + "): " + e.what());
                }
                break;
            }
        }
    }

    return out;
}

// ──────────────────────────────────────────────────────────────────────────────
// LOOK module: use template;
// ──────────────────────────────────────────────────────────────────────────────

static TplContext value_to_context(const Value& v) {
    TplContext ctx;
    if (v.type() != Value::ARRAY || !v.as_array()) return ctx;
    const auto& arr = *v.as_array();
    size_t start = 0;
    if (!arr.empty() &&
        arr[0].type() == Value::STRING &&
        (arr[0].as_string() == "__assoc__" || arr[0].as_string() == "__struct__"))
        start = 1;
    for (size_t i = start; i + 1 < arr.size(); i += 2) {
        if (arr[i].type() == Value::STRING)
            ctx[arr[i].as_string()] = arr[i+1];
    }
    return ctx;
}

Module make_template_module(Interpreter* interp) {
    Module m;
    m.name = "template";

    // template::render("views/home", $data) → string
    // Returns rendered HTML. Use print(template::render(...)) in scripts.
    m.functions["render"] = [](std::vector<Value> args) -> Value {
        if (args.empty() || args.size() > 2)
            throw std::runtime_error("template::render() 1 veya 2 argüman bekliyor: (dosya_yolu [, $veri])");
        if (args[0].type() != Value::STRING)
            throw std::runtime_error("template::render(): ilk argüman string (dosya yolu) olmalı");
        TplContext ctx;
        if (args.size() == 2) ctx = value_to_context(args[1]);
        std::string html = TemplateEngine::render_file(args[0].as_string(), ctx);
        return Value(html);
    };

    // template::render_string("<h1>{$title}</h1>", $data) → string
    m.functions["render_string"] = [](std::vector<Value> args) -> Value {
        if (args.empty() || args.size() > 2)
            throw std::runtime_error("template::render_string() 1 veya 2 argüman bekliyor");
        if (args[0].type() != Value::STRING)
            throw std::runtime_error("template::render_string(): ilk argüman string olmalı");
        TplContext ctx;
        if (args.size() == 2) ctx = value_to_context(args[1]);
        std::string html = TemplateEngine::render_string(args[0].as_string(), ctx);
        return Value(html);
    };

    // template::escape($str) → HTML-escaped string (for manual use)
    m.functions["escape"] = [](std::vector<Value> args) -> Value {
        if (args.size() != 1)
            throw std::runtime_error("template::escape() 1 argüman bekliyor");
        std::string s = args[0].type() == Value::STRING
                        ? args[0].as_string()
                        : TemplateEngine::to_str(args[0]);
        return Value(TemplateEngine::html_escape(s));
    };

    return m;
}

} // namespace look
