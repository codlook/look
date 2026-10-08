#pragma once
// LOOK 2 — parametreye yazma denetimi.
//
// Diziler ve struct'lar DEĞERDİR: fonksiyona verilen dizi çağıranınkinin kopyası gibi davranır,
// fonksiyonun içine yazdığı şey çağırana ULAŞMAZ. LOOK 1'de ulaşıyordu. Bu fark hata vermez,
// kod çalışır ve çağıran değişikliği görmez — sessiz davranış değişikliği. Bu denetim onu
// yüksek sesle söyler:
//
//   param-write   fonksiyon bir parametresinin İÇİNE yazıyor ($p[i] = …, $p.f = …, $p[i]++,
//                 push($p, …), pop($p)) ve o parametreyi sonra ne döndürüyor ne de bir yere
//                 veriyor → yazılan şey kayboluyor.
//
// "Döndürüyor / bir yere veriyor" = parametre BÜTÜN olarak (bir elemanı değil) ilk yazmadan
// sonra şu yerlerden birinde geçiyor: return ifadesi, bir atamanın sağ tarafı, bir çağrının
// argümanı (yalnız okuyan count/len/empty/… hariç), dizi/map/struct literali, print/write/throw,
// ya da bir iç closure onu yakalıyor. Yazma bir döngüdeyse aynı döngüdeki kullanım da sayılır
// (sonraki turda yazmadan sonra çalışır).
//
// Bilinçli olarak dar: parametre fonksiyonda düz atanıyorsa (`$p = …`) artık çağıranın değeri
// değildir — LOOK 1'de de öyleydi — denetlenmez. Değişken sayıda argüman parametresi (`...$a`)
// her zaman yeni bir dizidir, denetlenmez. Yanlış alarm vermektense kaçırmayı seçer.
#include "look/ast.h"
#include "look/capture_check.h"
#include "look/logger.h"
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace look {
namespace param_write_detail {

using capture_check_detail::bare;

struct Event { int seq; int line; int column; std::string how; std::vector<int> loops; };

struct Walk {
    std::set<std::string> params;                       // çıplak adlar
    std::map<std::string, std::vector<Event>> writes;   // parametrenin İÇİNE yazmalar
    std::map<std::string, std::vector<Event>> uses;     // bütün olarak döndürme / verme
    std::set<std::string> rebound;                      // düz atanan parametreler
    std::vector<const FunctionExpression*> closures;    // iç closure'lar (ayrıca denetlenir)
    std::vector<const FunctionDeclaration*> inner_fns;  // iç adlı fonksiyonlar
    std::vector<int> loop_stack;
    int seq = 0, next_loop = 0, cur_line = 0;

    static const Variable* root_of(const Expression* e) { return capture_check_detail::Scan::root_of(e); }
    int ln(const Expression* e) const { return e->loc.line > 0 ? e->loc.line : cur_line; }
    bool is_param(const std::string& n) const { return params.count(bare(n)) > 0; }

    void write(const std::string& name, const Expression* at, const std::string& how) {
        if (is_param(name)) writes[bare(name)].push_back({++seq, ln(at), at->loc.column, how, loop_stack});
    }
    void use(const std::string& name) {
        if (is_param(name)) uses[bare(name)].push_back({++seq, 0, 0, "", loop_stack});
    }

    static bool reads_only(const Expression* callee) {
        static const std::set<std::string> pure = {
            "count", "len", "empty", "isset", "is_array", "is_map", "is_null", "is_string",
            "is_int", "in_array", "typeof", "type" };
        if (auto* v = dynamic_cast<const Variable*>(callee)) return pure.count(bare(v->name)) > 0;
        return false;
    }

    // `value` = bu ifadenin DEĞERİ bir yere akıyor mu (döndürülüyor, atanıyor, veriliyor).
    void expr(const Expression* e, bool value) {
        if (!e) return;
        if (auto* v = dynamic_cast<const Variable*>(e)) { if (value) use(v->name); return; }
        if (auto* f = dynamic_cast<const FunctionExpression*>(e)) {
            closures.push_back(f);
            for (auto& d : f->defaults) expr(d.get(), false);
            // Closure parametreyi yakalıyorsa değeri onunla birlikte gider.
            for (auto& n : f->free_names) use(n);
            for (auto& n : f->captures)   use(n);
            return;
        }
        if (auto* a = dynamic_cast<const AssignmentExpression*>(e)) {
            expr(a->value.get(), true);
            if (a->index || a->object) {
                expr(a->index.get(), false);
                const Variable* r = a->object ? root_of(a->object.get()) : nullptr;
                if (a->object && !r) { expr(a->object.get(), false); return; }
                write(r ? r->name : a->name, e, "an element or field of it is assigned");
            } else if (is_param(a->name)) {
                rebound.insert(bare(a->name));
            }
            return;
        }
        if (auto* u = dynamic_cast<const UnaryExpression*>(e)) {
            if (u->op == "++" || u->op == "--") {
                if (auto* v = dynamic_cast<const Variable*>(u->right.get())) {
                    if (is_param(v->name)) rebound.insert(bare(v->name));   // sayı: değer zaten kopya
                } else if (auto* r = root_of(u->right.get())) {
                    write(r->name, e, "an element or field of it is changed with " + u->op);
                }
                return;
            }
            expr(u->right.get(), false);
            return;
        }
        if (auto* c = dynamic_cast<const CallExpression*>(e)) {
            expr(c->callee.get(), false);
            const auto* cv = dynamic_cast<const Variable*>(c->callee.get());
            const bool pushpop = cv && (bare(cv->name) == "push" || bare(cv->name) == "pop");
            const bool pure = reads_only(c->callee.get());
            for (size_t i = 0; i < c->arguments.size(); ++i) {
                if (pushpop && i == 0) {
                    if (auto* r = root_of(c->arguments[0].get())) {
                        write(r->name, e, bare(cv->name) + "() changes it");
                        if (auto* ix = dynamic_cast<const IndexExpression*>(c->arguments[0].get())) expr(ix->index.get(), false);
                        continue;
                    }
                }
                expr(c->arguments[i].get(), !pure);
            }
            return;
        }
        if (auto* x = dynamic_cast<const IndexExpression*>(e)) { expr(x->object.get(), false); expr(x->index.get(), false); return; }
        if (auto* x = dynamic_cast<const MemberAccessExpression*>(e)) { expr(x->object.get(), false); return; }
        if (auto* x = dynamic_cast<const BinaryExpression*>(e)) { expr(x->left.get(), false); expr(x->right.get(), false); return; }
        if (auto* x = dynamic_cast<const TernaryExpression*>(e)) { expr(x->condition.get(), false); expr(x->then_expr.get(), value); expr(x->else_expr.get(), value); return; }
        if (auto* x = dynamic_cast<const ArrayLiteral*>(e)) { for (auto& el : x->elements) expr(el.get(), true); return; }
        if (auto* x = dynamic_cast<const AssocArrayLiteral*>(e)) { for (auto& p : x->pairs) { expr(p.first.get(), false); expr(p.second.get(), true); } return; }
        if (auto* x = dynamic_cast<const StructLiteralExpression*>(e)) { for (auto& p : x->fields) expr(p.second.get(), true); return; }
    }

    void block(const BlockStatement* b) { if (b) for (auto& s : b->statements) stmt(s.get()); }

    template <class F> void in_loop(F&& body) {
        loop_stack.push_back(++next_loop);
        body();
        loop_stack.pop_back();
    }

    void stmt(const Statement* s) {
        if (!s) return;
        if (s->loc.line > 0) cur_line = s->loc.line;
        if (auto* x = dynamic_cast<const ExpressionStatement*>(s)) { expr(x->expression.get(), false); return; }
        if (auto* x = dynamic_cast<const PrintStatement*>(s)) { for (auto& e : x->expressions) expr(e.get(), true); return; }
        if (auto* x = dynamic_cast<const WriteStatement*>(s)) { for (auto& e : x->expressions) expr(e.get(), true); return; }
        if (auto* x = dynamic_cast<const ReturnStatement*>(s)) { expr(x->expression.get(), true); return; }
        if (auto* x = dynamic_cast<const ThrowStatement*>(s)) { expr(x->expression.get(), true); return; }
        if (auto* x = dynamic_cast<const BlockStatement*>(s)) { block(x); return; }
        if (auto* x = dynamic_cast<const IfStatement*>(s)) { expr(x->condition.get(), false); block(x->then_branch.get()); block(x->else_branch.get()); return; }
        if (auto* x = dynamic_cast<const WhileStatement*>(s)) {
            in_loop([&] { expr(x->condition.get(), false); stmt(x->body.get()); });
            return;
        }
        if (auto* x = dynamic_cast<const ForStatement*>(s)) {
            stmt(x->init.get());
            in_loop([&] { expr(x->condition.get(), false); stmt(x->body.get()); expr(x->post.get(), false); });
            return;
        }
        if (auto* x = dynamic_cast<const ForeachStatement*>(s)) {
            expr(x->iterable.get(), false);
            if (is_param(x->key_var))   rebound.insert(bare(x->key_var));
            if (is_param(x->value_var)) rebound.insert(bare(x->value_var));
            in_loop([&] { stmt(x->body.get()); });
            return;
        }
        if (auto* x = dynamic_cast<const TryCatchStatement*>(s)) {
            block(x->try_block.get());
            if (is_param(x->catch_var)) rebound.insert(bare(x->catch_var));
            block(x->catch_block.get()); block(x->finally_block.get());
            return;
        }
        if (auto* x = dynamic_cast<const SwitchStatement*>(s)) {
            expr(x->subject.get(), false);
            for (auto& c : x->cases) { for (auto& v : c.values) expr(v.get(), false); for (auto& b : c.body) stmt(b.get()); }
            return;
        }
        if (auto* x = dynamic_cast<const FunctionDeclaration*>(s)) {
            inner_fns.push_back(x);
            for (auto& n : x->free_names) use(n);   // iç fonksiyon parametreyi yakalıyorsa
            return;
        }
    }
};

inline void check_fn(const std::string& label, const std::vector<std::string>& parameters, bool is_variadic,
                     const BlockStatement* body, std::vector<CaptureWarning>& out) {
    Walk w;
    for (size_t i = 0; i < parameters.size(); ++i)
        if (!(is_variadic && i + 1 == parameters.size())) w.params.insert(bare(parameters[i]));
    w.block(body);
    for (auto& [p, ws] : w.writes) {
        if (w.rebound.count(p)) continue;
        const Event& first = ws.front();
        bool kept = false;
        auto it = w.uses.find(p);
        if (it != w.uses.end()) {
            for (const Event& u : it->second) {
                if (u.seq > first.seq) { kept = true; break; }
                // aynı döngüde bir yazma varsa, döngünün başındaki kullanım sonraki turda yazmayı görür
                for (const Event& wr : ws)
                    for (int l : wr.loops)
                        for (int ul : u.loops) if (l == ul) kept = true;
                if (kept) break;
            }
        }
        if (kept) continue;
        out.push_back({first.line, first.column, "param-write",
            label + " changes its parameter $" + p + " (" + first.how + ") but never returns it or passes it on; "
            "arrays and structs are values, so the caller's $" + p + " does not change — return the new value"});
    }
    for (auto* f : w.closures)
        check_fn("a closure", f->parameters, f->is_variadic, f->body.get(), out);
    for (auto* f : w.inner_fns)
        check_fn("function " + f->name + "()", f->parameters, f->is_variadic, f->body.get(), out);
}

} // namespace param_write_detail

// Programdaki her fonksiyon ve closure için parametreye-yazma bulguları (satır sırasıyla).
inline std::vector<CaptureWarning> check_param_writes(const Program& program) {
    std::vector<CaptureWarning> out;
    // Üst düzey: parametresi olmayan bir "fonksiyon" gibi gezilir — içindeki fonksiyonlar ve
    // closure'lar check_fn'in özyinelemesiyle denetlenir.
    param_write_detail::Walk top;
    for (auto& s : program.statements) top.stmt(s.get());
    for (auto* f : top.closures)
        param_write_detail::check_fn("a closure",
                                     f->parameters, f->is_variadic, f->body.get(), out);
    for (auto* f : top.inner_fns)
        param_write_detail::check_fn("function " + f->name + "()", f->parameters, f->is_variadic, f->body.get(), out);
    std::stable_sort(out.begin(), out.end(), [](const CaptureWarning& a, const CaptureWarning& b) { return a.line < b.line; });
    return out;
}

// Yüklemede göster: her bulgu için bir WARN satırı (dosya:satır). CLI, web başlangıcı ve `use`
// ile yüklenen dosyalar aynı yoldan geçer.
inline void log_param_writes(const std::vector<CaptureWarning>& ws, const std::string& file) {
    for (const auto& w : ws)
        if (w.kind == "param-write")
            Logger::instance().log(LogLevel::LOG_WARN, "check", file + ":" + std::to_string(w.line) + ": " + w.message);
}

} // namespace look
