#pragma once
// LOOK 2 — closure yakalama kuralı: bir closure yakaladığı şeyin DEĞERİNİ alır (oluşturulduğu
// andaki) ve onu içeride DEĞİŞTİREMEZ. İki motor da bu başlıktaki tek tanıma uyar.
//
// "Yakalanan" = `use (…)` listesindeki adlar + closure'ın kullandığı, kendi yereli olmayan,
// saran FONKSİYONUN yereli olan adlar (otomatik yakalama; iç içe closure'lar için geçişli).
// Üst düzey değişkenler `use` ile verilmedikçe yakalama değil GLOBAL erişimdir: canlı kalır.
//
// Ayrıştırıcının ağacından iki sınıf çıkarılır:
//   capture-write    closure yakaladığı değişkene yazıyor ($x = …, $x[i] = …, $x.f = …,
//                    $x += …, $x++, push($x, …), pop($x))            → YÜKLEMEDE HATA
//   capture-stale    değişken closure oluşturulduktan SONRA dışarıda değişiyor (ya da closure
//                    bir döngüde ve değişken o döngüde değişiyor): closure yakaladığı değeri
//                    tutar. LOOK 1'de sonraki değeri görürdü → UYARI (sessiz fark)
// Özyineleme: closure kendini yakalayamaz (o an henüz tanımsız) — adlı fonksiyon yazılır.
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

// Bir closure gövdesinin adları: `own` = kendi yerelleri (parametreler + ilk kullanımı düz atama
// olan adlar), `uses` = dışarıdan gelmesi gereken adlar (okunan/yazılan, kendi yereli olmayan;
// iç closure'ların dışarıdan istedikleri dahil — geçişli).
struct Names { std::set<std::string> own, uses; };
inline Names names_of(const FunctionExpression& f, const Scan& sc) {
    Names n;
    n.own.insert(f.parameters.begin(), f.parameters.end());
    const std::set<std::string> explicit_caps(f.captures.begin(), f.captures.end());
    for (auto& a : sc.plain_assigned)
        if (!explicit_caps.count(a) && sc.first.at(a) == 'w') n.own.insert(a);
    auto use = [&](const std::string& x) { if (!n.own.count(x)) n.uses.insert(x); };
    for (auto& r : sc.reads) use(r);
    for (auto& w : sc.writes) use(w.name);
    for (auto* c : sc.closures) { for (auto& x : c->free_names) use(x); for (auto& x : c->captures) use(x); }
    for (auto& x : explicit_caps) n.uses.insert(x);
    return n;
}

inline void check_scope(const Scan& sc, const std::set<std::string>& scope_locals, bool is_top_level,
                        std::vector<CaptureWarning>& out);

inline void check_closure(const FunctionExpression* f, const std::set<std::string>& enclosing_locals,
                          std::vector<CaptureWarning>& out) {
    Scan sc; sc.block(f->body.get());
    Names n = names_of(*f, sc);
    std::set<std::string> caps(f->captures.begin(), f->captures.end());
    for (auto& u : n.uses) if (enclosing_locals.count(u)) caps.insert(u);
    for (auto& w : sc.writes)
        if (caps.count(w.name))
            out.push_back({w.line, w.column, "capture-write",
                "closure changes captured variable $" + bare(w.name) + " (" + w.how
                + "); a closure captures the value and cannot change it — return the new value instead"});
    // İç closure'lar için "saran yereller": bu closure'ın yerelleri + yakaladıkları.
    std::set<std::string> locals(n.own); locals.insert(caps.begin(), caps.end());
    for (auto& a : sc.plain_assigned) locals.insert(a);
    check_scope(sc, locals, /*is_top_level=*/false, out);
}

inline void check_scope(const Scan& sc, const std::set<std::string>& scope_locals, bool is_top_level,
                        std::vector<CaptureWarning>& out) {
    for (auto* f : sc.closures) {
        check_closure(f, is_top_level ? std::set<std::string>{} : scope_locals, out);
        // Bu closure'ın BU kapsamdan yakaladığı adlar: açık liste her yerde; otomatik yakalama
        // yalnız fonksiyon içinde (üst düzeyde `use`suz ad = global erişim, canlı kalır).
        std::set<std::string> caps(f->captures.begin(), f->captures.end());
        if (!is_top_level) for (auto& u : f->free_names) if (scope_locals.count(u)) caps.insert(u);
        if (caps.empty()) continue;
        const int fline = sc.line_of.count(f) ? sc.line_of.at(f) : f->loc.line;
        std::set<std::string> reported;
        for (auto& w : sc.writes) {
            if (!caps.count(w.name) || reported.count(w.name) || w.line <= fline) continue;
            reported.insert(w.name);
            out.push_back({w.line, w.column, "capture-stale",
                "$" + bare(w.name) + " changes here after the closure at line " + std::to_string(fline)
                + " captured it; the closure keeps the value it captured"});
        }
        for (auto& lw : sc.closure_loop_writes) {
            if (lw.first != f) continue;
            for (auto& x : lw.second) {
                if (!caps.count(x) || reported.count(x)) continue;
                reported.insert(x);
                out.push_back({fline, f->loc.column, "capture-stale",
                    "closure captures $" + bare(x) + ", which changes in the surrounding loop; each closure "
                    "keeps the value of the iteration that created it"});
            }
        }
    }
}

inline void check_function_decls(const std::vector<std::unique_ptr<Statement>>& stmts, std::vector<CaptureWarning>& out);

inline void walk_decls(const Statement* s, std::vector<CaptureWarning>& out, const std::set<std::string>* enclosing = nullptr) {
    if (!s) return;
    if (auto* fd = dynamic_cast<const FunctionDeclaration*>(s)) {
        Scan sc; sc.block(fd->body.get());
        std::set<std::string> locals(fd->parameters.begin(), fd->parameters.end());
        // İÇ adlı fonksiyon (enclosing dolu) bir closure'dır: saran fonksiyonun yereline yazamaz.
        if (enclosing) {
            FunctionExpression shape; shape.parameters = fd->parameters;
            Names n = names_of(shape, sc);
            for (auto& w : sc.writes)
                if (!n.own.count(w.name) && enclosing->count(w.name))
                    out.push_back({w.line, w.column, "capture-write",
                        "function " + fd->name + "() changes captured variable $" + bare(w.name) + " (" + w.how
                        + "); a function declared inside another captures the value and cannot change it"});
            for (auto& u : n.uses) if (enclosing->count(u)) locals.insert(u);
        }
        locals.insert(sc.plain_assigned.begin(), sc.plain_assigned.end());
        check_scope(sc, locals, /*is_top_level=*/false, out);
        if (fd->body) for (auto& st : fd->body->statements) walk_decls(st.get(), out, &locals);
        return;
    }
    if (auto* b = dynamic_cast<const BlockStatement*>(s)) { for (auto& st : b->statements) walk_decls(st.get(), out, enclosing); return; }
    if (auto* x = dynamic_cast<const IfStatement*>(s)) { walk_decls(x->then_branch.get(), out, enclosing); walk_decls(x->else_branch.get(), out, enclosing); return; }
}
inline void check_function_decls(const std::vector<std::unique_ptr<Statement>>& stmts, std::vector<CaptureWarning>& out) {
    for (auto& s : stmts) walk_decls(s.get(), out);
}

} // namespace capture_check_detail

// Closure'ın dışarıdan istediği adlar (geçişli) — ayrıştırıcı düğüme yazar; yorumlayıcı closure
// kurulurken bu adlardan saran fonksiyon kapsamında bulunanların DEĞERİNİ alır. İç closure'ların
// free_names'i önce dolmuş olmalıdır (ayrıştırıcı içten dışa kurar).
// Adlı iç fonksiyon için aynı hesap (parametreler + gövde).
inline std::vector<std::string> capture_free_names(const std::vector<std::string>& params, const BlockStatement* body) {
    using namespace capture_check_detail;
    FunctionExpression shape; shape.parameters = params;
    Scan sc; sc.block(body);
    Names n = names_of(shape, sc);
    return std::vector<std::string>(n.uses.begin(), n.uses.end());
}

inline std::vector<std::string> capture_free_names(const FunctionExpression& f) {
    using namespace capture_check_detail;
    Scan sc; sc.block(f.body.get());
    Names n = names_of(f, sc);
    return std::vector<std::string>(n.uses.begin(), n.uses.end());
}

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
