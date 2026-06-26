#pragma once
#include "look/interpreter.h"
#include <string>
#include <vector>
#include <map>

namespace look {

enum class TplNodeKind {
    Text,     // literal HTML
    Var,      // {$name} or {$name.field} — auto HTML escape
    RawVar,   // {!$name} — no escape
    If,       // {#if cond} ... {#else} ... {/if}
    Each,     // {#each $arr as $item} ... {#empty} ... {/each}
    Extends,  // {#extends "path"}
    Block,    // {#block "name"} ... {/block}
    Include,  // {#include "path"} or {#include "path" data=$var}
};

struct TplNode {
    TplNodeKind           kind  = TplNodeKind::Text;
    std::string           text;    // Text: literal | Var/RawVar: var path | Extends/Include: file path | Block: block name
    std::string           extra;   // If: condition | Each: item var name | Include: data var path
    std::vector<TplNode>  children; // If: then branch | Each: body | Block: default content
    std::vector<TplNode>  alt;      // If: else branch | Each: empty branch
};

using TplContext = std::map<std::string, Value>;
using TplBlocks  = std::map<std::string, std::vector<TplNode>>;

class TemplateEngine {
public:
    static std::vector<TplNode> parse(const std::string& src, const std::string& origin = "");
    static std::string render(const std::vector<TplNode>& nodes,
                              const TplContext& ctx,
                              const TplBlocks* blocks = nullptr);
    static std::string render_file(const std::string& path, const TplContext& ctx);
    static std::string render_string(const std::string& src, const TplContext& ctx);

    static std::string html_escape(const std::string& s);
    static std::string to_str(const Value& v);
    static bool        is_truthy(const Value& v);
    static Value       resolve(const std::string& path, const TplContext& ctx);
    static bool        eval_cond(const std::string& cond, const TplContext& ctx);
    static std::string resolve_path(const std::string& path);

private:
    static void collect_blocks(const std::vector<TplNode>& nodes, TplBlocks& out);
};

Module make_template_module(Interpreter* interp);

} // namespace look
