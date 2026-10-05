#pragma once
// LOOK 2 — closure yakalama denetimi (şimdilik yalnız UYARI; `lk --check` basar).
//
// Hedef kural: bir closure yakaladığı şeyin DEĞERİNİ alır ve onu içeride DEĞİŞTİREMEZ.
// Bu geçiş kuralı devreye girmeden önce, kuralın kıracağı her yeri ayrıştırıcının kendi
// ağacından (metin aramasından değil) satır numarasıyla çıkarır. Üç sınıf:
//   capture-write    closure, yakaladığı değişkene yazıyor ($x = …, $x[i] = …, $x.f = …,
//                    $x += …, $x++, push($x, …), pop($x))  → kural gelince yüklemede HATA
//   capture-stale    değişken closure oluşturulduktan SONRA dışarıda değişiyor (ya da closure
//                    bir döngünün içinde ve değişken o döngüde değişiyor) → closure eski değeri
//                    görür; SESSİZ davranış değişikliği
//   capture-self     closure, atandığı değişkenin kendisini yakalıyor (özyineleme kalıbı)
//                    → değer yakalamada o an null'dur
//
// "Yakalanan" = `use (…)` listesindeki adlar + closure'ın okuduğu, kendisinin yerel yapmadığı,
// saran FONKSİYONUN yereli olan adlar (otomatik yakalama). Üst düzey değişkenler `use` ile
// verilmedikçe yakalama değil global erişimdir ve bu denetimin konusu değildir.
#include "look/ast.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace look {

struct CaptureWarning { int line = 0; int column = 0; std::string kind; std::string message; };

namespace capture_check_detail {

struct Write { std::string name; int line; int column; std::string how; };

// Bir ifade/deyim ağacını gezer; iç FunctionExpression gövdelerine İNMEZ (onlar ayrı kapsam),
// ama onları `closures` listesine ekler.
struct Scan {
    std::set<std::string> reads;                 // okunan değişken adları
    std::map<std::string, char> first;           // adın İLK kullanımı: 'r' okuma, 'w' düz atama
    std::map<const FunctionExpression*, int> line_of;   // closure'ın satırı (ifade konumu boşsa deyiminki)
    int cur_line = 0;
    int ln(const Expression* e) const { return e->loc.line > 0 ? e->loc.line : cur_line; }
    std::set<std::string> plain_assigned;        // `$x = …` ile yerel yapılan adlar (+ foreach/catch)
    std::vector<Write> writes;                   // her türlü yazma
    std::vector<const FunctionExpression*> closures;
    std::vector<std::pair<const FunctionExpression*, std::string>> closure_assigned_to;  // $f = function…
    std::vector<std::pair<const FunctionExpression*, std::vector<std::string>>> closure_loop_writes;

    static const Variable* root_of(const Expression* e) {
        while (e) {
            if (auto* v = dynamic_cast<const Variable*>(e)) return v;
            if (auto* ix = dynamic_cast<const IndexExpression*>(e)) { e = ix->object.get(); continue; }
            if (auto* m = dynamic_cast<const MemberAccessExpression*>(e)) { e = m->object.get(); continue; }
            return nullptr;
        }
        return nullptr;
    }

    void expr(const Expression* e) {
        if (!e) return;
        if (auto* v = dynamic_cast<const Variable*>(e)) { reads.insert(v->name); first.emplace(v->name, 'r'); return; }
        if (auto* f = dynamic_cast<const FunctionExpression*>(e)) {
            closures.push_back(f); line_of[f] = ln(e);
            for (auto& d : f->defaults) expr(d.get());
            return;
        }
        if (auto* a = dynamic_cast<const AssignmentExpression*>(e)) {
            expr(a->value.get());
            if (a->index || a->object) {
                expr(a->index.get());
                const Variable* r = a->object ? root_of(a->object.get()) : nullptr;
                std::string n = r ? r->name : a->name;
                if (a->object && !r) { expr(a->object.get()); return; }
                if (a->object) expr(a->object.get()); else { reads.insert(n); first.emplace(n, 'r'); }
                writes.push_back({n, ln(e), e->loc.column, "an element or field of it is assigned"});
            } else {
                if (a->op == "=") { plain_assigned.insert(a->name); first.emplace(a->name, 'w'); }
                else { reads.insert(a->name); first.emplace(a->name, 'r'); }
                writes.push_back({a->name, ln(e), e->loc.column,
                                  a->op == "=" ? "it is assigned" : "it is changed with " + a->op});
                if (auto* f = dynamic_cast<const FunctionExpression*>(a->value.get()))
                    closure_assigned_to.push_back({f, a->name});
            }
            return;
        }
        if (auto* u = dynamic_cast<const UnaryExpression*>(e)) {
            expr(u->right.get());
            if (u->op == "++" || u->op == "--")
                if (auto* v = dynamic_cast<const Variable*>(u->right.get()))
                    writes.push_back({v->name, ln(e), e->loc.column, "it is changed with " + u->op});
            return;
        }
        if (auto* c = dynamic_cast<const CallExpression*>(e)) {
            expr(c->callee.get());
            for (auto& x : c->arguments) expr(x.get());
            if (auto* cv = dynamic_cast<const Variable*>(c->callee.get()))
                if ((cv->name == "push" || cv->name == "pop") && !c->arguments.empty())
                    if (auto* r = root_of(c->arguments[0].get()))
                        writes.push_back({r->name, ln(e), e->loc.column, cv->name + "() changes it"});
            return;
        }
        if (auto* x = dynamic_cast<const IndexExpression*>(e)) { expr(x->object.get()); expr(x->index.get()); return; }
        if (auto* x = dynamic_cast<const MemberAccessExpression*>(e)) { expr(x->object.get()); return; }
        if (auto* x = dynamic_cast<const BinaryExpression*>(e)) { expr(x->left.get()); expr(x->right.get()); return; }
        if (auto* x = dynamic_cast<const TernaryExpression*>(e)) { expr(x->condition.get()); expr(x->then_expr.get()); expr(x->else_expr.get()); return; }
        if (auto* x = dynamic_cast<const ArrayLiteral*>(e)) { for (auto& el : x->elements) expr(el.get()); return; }
        if (auto* x = dynamic_cast<const AssocArrayLiteral*>(e)) { for (auto& p : x->pairs) { expr(p.first.get()); expr(p.second.get()); } return; }
        if (auto* x = dynamic_cast<const StructLiteralExpression*>(e)) { for (auto& p : x->fields) expr(p.second.get()); return; }
    }

    // Döngü gövdesi: içindeki closure'lar için "aynı döngüde yazılan adlar" kaydı tutulur.
    void loop_body(const std::vector<const Statement*>& parts, const std::vector<const Expression*>& exprs,
                   const std::vector<std::string>& loop_vars) {
        const size_t c0 = closures.size(), w0 = writes.size();
        for (auto* x : exprs) expr(x);
        for (auto* s : parts) stmt(s);
        if (closures.size() == c0) return;
        std::vector<std::string> names(loop_vars);
        for (size_t i = w0; i < writes.size(); ++i) names.push_back(writes[i].name);
        for (size_t i = c0; i < closures.size(); ++i) closure_loop_writes.push_back({closures[i], names});
    }

    void block(const BlockStatement* b) { if (b) for (auto& s : b->statements) stmt(s.get()); }

    void stmt(const Statement* s) {
        if (!s) return;
        if (s->loc.line > 0) cur_line = s->loc.line;
        if (auto* x = dynamic_cast<const ExpressionStatement*>(s)) { expr(x->expression.get()); return; }
        if (auto* x = dynamic_cast<const PrintStatement*>(s)) { for (auto& e : x->expressions) expr(e.get()); return; }
        if (auto* x = dynamic_cast<const WriteStatement*>(s)) { for (auto& e : x->expressions) expr(e.get()); return; }
        if (auto* x = dynamic_cast<const ReturnStatement*>(s)) { expr(x->expression.get()); return; }
        if (auto* x = dynamic_cast<const ThrowStatement*>(s)) { expr(x->expression.get()); return; }
        if (auto* x = dynamic_cast<const BlockStatement*>(s)) { block(x); return; }
        if (auto* x = dynamic_cast<const IfStatement*>(s)) { expr(x->condition.get()); block(x->then_branch.get()); block(x->else_branch.get()); return; }
        if (auto* x = dynamic_cast<const WhileStatement*>(s)) { loop_body({x->body.get()}, {x->condition.get()}, {}); return; }
        if (auto* x = dynamic_cast<const ForStatement*>(s)) {
            stmt(x->init.get());
            loop_body({x->body.get()}, {x->condition.get(), x->post.get()}, {});
            return;
        }
        if (auto* x = dynamic_cast<const ForeachStatement*>(s)) {
            expr(x->iterable.get());
            std::vector<std::string> lv;
            if (!x->key_var.empty()) { plain_assigned.insert(x->key_var); first.emplace(x->key_var, 'w'); lv.push_back(x->key_var); }
            if (!x->value_var.empty()) { plain_assigned.insert(x->value_var); first.emplace(x->value_var, 'w'); lv.push_back(x->value_var); }
            loop_body({x->body.get()}, {}, lv);
            return;
        }
        if (auto* x = dynamic_cast<const TryCatchStatement*>(s)) {
            block(x->try_block.get());
            if (!x->catch_var.empty()) { plain_assigned.insert(x->catch_var); first.emplace(x->catch_var, 'w'); }
            block(x->catch_block.get()); block(x->finally_block.get());
            return;
        }
        if (auto* x = dynamic_cast<const SwitchStatement*>(s)) {
            expr(x->subject.get());
            for (auto& c : x->cases) { for (auto& v : c.values) expr(v.get()); for (auto& b : c.body) stmt(b.get()); }
            return;
        }
        // FunctionDeclaration ayrı kapsamdır: check_function ile ayrıca gezilir.
    }
};

inline std::string bare(const std::string& n) { return (!n.empty() && n[0] == '$') ? n.substr(1) : n; }

// Bir fonksiyon kapsamını (adlı fonksiyon, closure ya da üst düzey) denetler.
//   enclosing_locals : saran FONKSİYON kapsamlarının yerelleri (üst düzeyde boş)
inline void check_scope(const Scan& sc, const std::set<std::string>& scope_locals, bool is_top_level,
                        std::vector<CaptureWarning>& out);

inline void check_closure(const FunctionExpression* f, const std::set<std::string>& enclosing_locals,
                          std::vector<CaptureWarning>& out) {
    Scan sc; sc.block(f->body.get());
    std::set<std::string> own(f->parameters.begin(), f->parameters.end());
    std::set<std::string> explicit_caps(f->captures.begin(), f->captures.end());
    for (auto& n : sc.plain_assigned)
        if (!explicit_caps.count(n) && !(sc.first.at(n) == 'r' && enclosing_locals.count(n))) own.insert(n);
    // Yakalananlar: açık liste + okunan/yazılan, kendi yereli olmayan, saran fonksiyonun yereli.
    std::set<std::string> caps(explicit_caps);
    auto consider = [&](const std::string& n) { if (!own.count(n) && enclosing_locals.count(n)) caps.insert(n); };
    for (auto& n : sc.reads) consider(n);
    for (auto& w : sc.writes) consider(w.name);
    for (auto& w : sc.writes)
        if (caps.count(w.name))
            out.push_back({w.line, w.column, "capture-write",
                "closure writes to captured variable $" + bare(w.name) + " (" + w.how
                + "); a closure will capture the value and may not change it — return the new value instead"});
    // İç closure'lar: bu closure'ın yerelleri + yakaladıkları onların "saran yereli"dir.
    std::set<std::string> locals(own); locals.insert(caps.begin(), caps.end());
    check_scope(sc, locals, /*is_top_level=*/false, out);
}

inline void check_scope(const Scan& sc, const std::set<std::string>& scope_locals, bool is_top_level,
                        std::vector<CaptureWarning>& out) {
    for (auto* f : sc.closures) {
        check_closure(f, is_top_level ? std::set<std::string>{} : scope_locals, out);
        // Bu closure'ın bu kapsamdan yakaladığı adlar (stale/self için): açık liste her yerde;
        // otomatik yakalama yalnız fonksiyon içinde.
        Scan in; in.block(f->body.get());
        std::set<std::string> own(f->parameters.begin(), f->parameters.end());
        std::set<std::string> caps(f->captures.begin(), f->captures.end());
        for (auto& n : in.plain_assigned)
            if (!caps.count(n) && !(in.first.at(n) == 'r' && !is_top_level && scope_locals.count(n))) own.insert(n);
        if (!is_top_level) for (auto& n : in.reads) if (!own.count(n) && scope_locals.count(n)) caps.insert(n);
        if (caps.empty()) continue;
        const int fline = sc.line_of.count(f) ? sc.line_of.at(f) : f->loc.line;
        for (auto& p : sc.closure_assigned_to)
            if (p.first == f && caps.count(p.second))
                out.push_back({fline, f->loc.column, "capture-self",
                    "closure captures $" + bare(p.second) + ", the variable it is being assigned to; "
                    "with value capture it is still null at that point (recursive closures need another form)"});
        std::set<std::string> reported;
        for (auto& w : sc.writes) {
            if (!caps.count(w.name) || reported.count(w.name)) continue;
            const bool after = w.line > fline;
            if (!after) continue;
            reported.insert(w.name);
            out.push_back({w.line, w.column, "capture-stale",
                "$" + bare(w.name) + " changes here after the closure at line " + std::to_string(fline)
                + " captured it; with value capture the closure keeps the earlier value"});
        }
        for (auto& lw : sc.closure_loop_writes) {
            if (lw.first != f) continue;
            for (auto& n : lw.second) {
                if (!caps.count(n) || reported.count(n)) continue;
                reported.insert(n);
                out.push_back({fline, f->loc.column, "capture-stale",
                    "closure captures $" + bare(n) + ", which changes in the surrounding loop; with value "
                    "capture each closure keeps the value of its own iteration"});
            }
        }
    }
}

inline void check_function_decls(const std::vector<std::unique_ptr<Statement>>& stmts, std::vector<CaptureWarning>& out);

inline void walk_decls(const Statement* s, std::vector<CaptureWarning>& out) {
    if (!s) return;
    if (auto* fd = dynamic_cast<const FunctionDeclaration*>(s)) {
        Scan sc; sc.block(fd->body.get());
        std::set<std::string> locals(fd->parameters.begin(), fd->parameters.end());
        locals.insert(sc.plain_assigned.begin(), sc.plain_assigned.end());
        check_scope(sc, locals, /*is_top_level=*/false, out);
        if (fd->body) check_function_decls(fd->body->statements, out);
        return;
    }
    if (auto* b = dynamic_cast<const BlockStatement*>(s)) { check_function_decls(b->statements, out); return; }
    if (auto* x = dynamic_cast<const IfStatement*>(s)) { walk_decls(x->then_branch.get(), out); walk_decls(x->else_branch.get(), out); return; }
}
inline void check_function_decls(const std::vector<std::unique_ptr<Statement>>& stmts, std::vector<CaptureWarning>& out) {
    for (auto& s : stmts) walk_decls(s.get(), out);
}

} // namespace capture_check_detail

inline std::vector<CaptureWarning> check_captures(const Program& program) {
    using namespace capture_check_detail;
    std::vector<CaptureWarning> out;
    Scan top;
    for (auto& s : program.statements) top.stmt(s.get());
    check_scope(top, {}, /*is_top_level=*/true, out);
    check_function_decls(program.statements, out);
    return out;
}

} // namespace look
