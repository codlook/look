#pragma once
// LOOK 2 — argüman sayısı denetimi (şimdilik yalnız UYARI; `lk --check` basar).
//
// 1.0.x'te iki motor ayrışıyordu: adlı bir fonksiyon fazla ya da eksik argümanla çağrılınca
// yorumlayıcı hata veriyordu, VM sessizce kabul ediyordu (eksik parametre null olur).
// Kural kesinleşmeden önce, kaç yerin etkileneceği ayrıştırıcının ağacından sayılır.
//
// Kapsam: aynı dosyada bildirilen ADLI fonksiyona, adıyla yapılan çağrılar. Değişkendeki
// closure'ın çağrısı ($f(...)) ve çalışma zamanının geri çağırmaya verdiği argümanlar
// (array::map, http::stream ...) statik olarak bilinemez; onlar bu denetimin dışındadır.
#include "look/ast.h"
#include <map>
#include <string>
#include <vector>

namespace look {

struct ArgWarning { int line = 0; int column = 0; std::string message; };

namespace arg_check_detail {

struct Sig { size_t required = 0, max = 0; bool variadic = false; int line = 0; bool dup = false; };

struct Walk {
    std::map<std::string, Sig> sigs;
    std::vector<ArgWarning> out;
    int cur_line = 0;
    bool collecting = true;

    void decl(const FunctionDeclaration& f) {
        if (!collecting) return;
        Sig s; s.max = f.parameters.size(); s.variadic = f.is_variadic; s.line = f.loc.line;
        for (size_t i = 0; i < f.parameters.size(); ++i) {
            const bool has_default = i < f.defaults.size() && f.defaults[i] != nullptr;
            const bool rest = f.is_variadic && i + 1 == f.parameters.size();
            if (!has_default && !rest) s.required = i + 1;
        }
        auto it = sigs.find(f.name);
        if (it != sigs.end()) it->second.dup = true;   // aynı ad iki kez: hangisi çağrılacağı belirsiz
        else sigs.emplace(f.name, s);
    }

    void call(const CallExpression& c) {
        if (collecting) return;
        auto* v = node_as<Variable>(c.callee.get());
        if (!v) return;
        auto it = sigs.find(v->name);
        if (it == sigs.end() || it->second.dup) return;
        const Sig& s = it->second; const size_t n = c.arguments.size();
        const int line = c.loc.line > 0 ? c.loc.line : cur_line;
        if (n < s.required)
            out.push_back({line, c.loc.column, v->name + "() is called with " + std::to_string(n)
                + " argument(s) but needs " + (s.required == s.max && !s.variadic ? "" : "at least ")
                + std::to_string(s.required)});
        else if (!s.variadic && n > s.max)
            out.push_back({line, c.loc.column, v->name + "() is called with " + std::to_string(n)
                + " argument(s) but takes " + (s.required == s.max ? "" : "at most ") + std::to_string(s.max)});
    }

    void expr(const Expression* e) {
        if (!e) return;
        if (auto* x = node_as<CallExpression>(e)) { call(*x); expr(x->callee.get()); for (auto& a : x->arguments) expr(a.get()); return; }
        if (auto* x = node_as<FunctionExpression>(e)) { for (auto& d : x->defaults) expr(d.get()); block(x->body.get()); return; }
        if (auto* x = node_as<AssignmentExpression>(e)) { expr(x->object.get()); expr(x->index.get()); expr(x->value.get()); return; }
        if (auto* x = node_as<UnaryExpression>(e)) { expr(x->right.get()); return; }
        if (auto* x = node_as<BinaryExpression>(e)) { expr(x->left.get()); expr(x->right.get()); return; }
        if (auto* x = node_as<TernaryExpression>(e)) { expr(x->condition.get()); expr(x->then_expr.get()); expr(x->else_expr.get()); return; }
        if (auto* x = node_as<IndexExpression>(e)) { expr(x->object.get()); expr(x->index.get()); return; }
        if (auto* x = node_as<MemberAccessExpression>(e)) { expr(x->object.get()); return; }
        if (auto* x = node_as<ArrayLiteral>(e)) { for (auto& el : x->elements) expr(el.get()); return; }
        if (auto* x = node_as<AssocArrayLiteral>(e)) { for (auto& p : x->pairs) { expr(p.first.get()); expr(p.second.get()); } return; }
        if (auto* x = node_as<StructLiteralExpression>(e)) { for (auto& p : x->fields) expr(p.second.get()); return; }
    }
    void block(const BlockStatement* b) { if (b) for (auto& s : b->statements) stmt(s.get()); }
    void stmt(const Statement* s) {
        if (!s) return;
        if (s->loc.line > 0) cur_line = s->loc.line;
        if (auto* x = node_as<FunctionDeclaration>(s)) { decl(*x); for (auto& d : x->defaults) expr(d.get()); block(x->body.get()); return; }
        if (auto* x = node_as<ExpressionStatement>(s)) { expr(x->expression.get()); return; }
        if (auto* x = node_as<PrintStatement>(s)) { for (auto& e : x->expressions) expr(e.get()); return; }
        if (auto* x = node_as<WriteStatement>(s)) { for (auto& e : x->expressions) expr(e.get()); return; }
        if (auto* x = node_as<ReturnStatement>(s)) { expr(x->expression.get()); return; }
        if (auto* x = node_as<ThrowStatement>(s)) { expr(x->expression.get()); return; }
        if (auto* x = node_as<BlockStatement>(s)) { block(x); return; }
        if (auto* x = node_as<IfStatement>(s)) { expr(x->condition.get()); block(x->then_branch.get()); block(x->else_branch.get()); return; }
        if (auto* x = node_as<WhileStatement>(s)) { expr(x->condition.get()); block(x->body.get()); return; }
        if (auto* x = node_as<ForStatement>(s)) { stmt(x->init.get()); expr(x->condition.get()); expr(x->post.get()); block(x->body.get()); return; }
        if (auto* x = node_as<ForeachStatement>(s)) { expr(x->iterable.get()); block(x->body.get()); return; }
        if (auto* x = node_as<TryCatchStatement>(s)) { block(x->try_block.get()); block(x->catch_block.get()); block(x->finally_block.get()); return; }
        if (auto* x = node_as<SwitchStatement>(s)) {
            expr(x->subject.get());
            for (auto& c : x->cases) { for (auto& v : c.values) expr(v.get()); for (auto& b : c.body) stmt(b.get()); }
            return;
        }
        if (auto* x = node_as<StructDeclaration>(s)) { for (auto& f : x->fields) expr(f.default_expr.get()); return; }
        if (auto* x = node_as<ConstBlock>(s)) { for (auto& i : x->items) expr(i.value.get()); return; }
    }
};

} // namespace arg_check_detail

inline std::vector<ArgWarning> check_arg_counts(const Program& program) {
    arg_check_detail::Walk w;
    for (auto& s : program.statements) w.stmt(s.get());    // 1. geçiş: bildirimler
    w.collecting = false;
    for (auto& s : program.statements) w.stmt(s.get());    // 2. geçiş: çağrılar
    return std::move(w.out);
}

} // namespace look
