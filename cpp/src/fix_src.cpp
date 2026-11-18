// lk fix — kullanımdan kalkan push/pop/array::pop yazımlarını yeni yazıma çevirir.
// Tasarım ve güvence: look/fix_src.h.
#include "look/fix_src.h"
#include "look/lexer.h"
#include "look/parser.h"
#include "look/ast.h"
#include "look/ast_spine.h"
#include "look/stack_guard.h"
#include "look/value.h"   // LookParseError
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

namespace look {

namespace {

using Key = std::pair<int, int>;   // (satır, sütun) — deyimin / çağrının ilk token'ı

// AST'den: hangi push/pop çağrısı DEYİM olarak duruyor, hangi array::pop nerede.
struct AstIndex {
    std::map<Key, const CallExpression*> stmt_calls;          // deyim: push(...) / pop(...)
    std::map<Key, const AssignmentExpression*> assign_pops;   // deyim: $v = pop(...)
    std::map<Key, const CallExpression*> array_pops;          // array::pop(...) (callee konumu)
};

const Variable* bare_callee(const CallExpression* c) {
    return c ? node_as<Variable>(c->callee.get()) : nullptr;
}

struct Walker {
    AstIndex& ix;

    void block(const BlockStatement* b) {
        if (b) for (const auto& s : b->statements) stmt(s.get(), true);
    }

    // rewritable=false: for başlığındaki deyim (oraya yeni satır konamaz).
    void stmt(const Statement* s, bool rewritable) {
        if (!s) return;
        look_ast_guard(s->loc.line, s->loc.column);
        if (auto* x = node_as<ExpressionStatement>(s)) {
            if (rewritable) classify(*x);
            expr(x->expression.get());
        } else if (auto* x = node_as<PrintStatement>(s)) { for (auto& e : x->expressions) expr(e.get()); }
        else if (auto* x = node_as<WriteStatement>(s)) { for (auto& e : x->expressions) expr(e.get()); }
        else if (auto* x = node_as<ReturnStatement>(s)) expr(x->expression.get());
        else if (auto* x = node_as<ThrowStatement>(s)) expr(x->expression.get());
        else if (auto* x = node_as<BlockStatement>(s)) block(x);
        else if (auto* x = node_as<IfStatement>(s)) { expr(x->condition.get()); block(x->then_branch.get()); block(x->else_branch.get()); }
        else if (auto* x = node_as<WhileStatement>(s)) { expr(x->condition.get()); block(x->body.get()); }
        else if (auto* x = node_as<ForStatement>(s)) { stmt(x->init.get(), false); expr(x->condition.get()); expr(x->post.get()); block(x->body.get()); }
        else if (auto* x = node_as<ForeachStatement>(s)) { expr(x->iterable.get()); block(x->body.get()); }
        else if (auto* x = node_as<TryCatchStatement>(s)) { block(x->try_block.get()); block(x->catch_block.get()); block(x->finally_block.get()); }
        else if (auto* x = node_as<SwitchStatement>(s)) {
            expr(x->subject.get());
            for (auto& c : x->cases) { for (auto& v : c.values) expr(v.get()); for (auto& b : c.body) stmt(b.get(), true); }
        }
        else if (auto* x = node_as<FunctionDeclaration>(s)) { for (auto& d : x->defaults) expr(d.get()); block(x->body.get()); }
        else if (auto* x = node_as<ConstBlock>(s)) { for (auto& it : x->items) expr(it.value.get()); }
        else if (auto* x = node_as<StructDeclaration>(s)) { for (auto& f : x->fields) expr(f.default_expr.get()); }
    }

    void classify(const ExpressionStatement& s) {
        const Key k{s.loc.line, s.loc.column};
        if (auto* c = node_as<CallExpression>(s.expression.get())) {
            if (auto* v = bare_callee(c); v && (v->name == "push" || v->name == "pop")) ix.stmt_calls[k] = c;
        } else if (auto* a = node_as<AssignmentExpression>(s.expression.get())) {
            if (a->op == "=" && !a->index && !a->object)
                if (auto* v = bare_callee(node_as<CallExpression>(a->value.get())); v && v->name == "pop")
                    ix.assign_pops[k] = a;
        }
    }

    void expr(const Expression* e) {
        if (!e) return;
        look_ast_guard(e->loc.line, e->loc.column);
        if (auto* x = node_as<CallExpression>(e)) {
            if (auto* sr = node_as<ScopeResolution>(x->callee.get());
                sr && sr->module_name == "array" && sr->member_name == "pop")
                ix.array_pops[{sr->loc.line, sr->loc.column}] = x;
            expr(x->callee.get());
            for (auto& a : x->arguments) expr(a.get());
        }
        else if (auto* x = node_as<ArrayLiteral>(e)) { for (auto& el : x->elements) expr(el.get()); }
        else if (auto* x = node_as<AssocArrayLiteral>(e)) { for (auto& p : x->pairs) { expr(p.first.get()); expr(p.second.get()); } }
        else if (auto* x = node_as<FunctionExpression>(e)) { for (auto& d : x->defaults) expr(d.get()); block(x->body.get()); }
        else if (auto* x = node_as<IndexExpression>(e)) { expr(x->object.get()); expr(x->index.get()); }
        else if (auto* x = node_as<UnaryExpression>(e)) expr(x->right.get());
        else if (auto* x = node_as<BinaryExpression>(e)) {   // sol omurga döngüyle (look/ast_spine.h)
            std::vector<const BinaryExpression*> sp;
            expr(binary_left_spine(x, sp));
            for (size_t k = sp.size(); k-- > 0;) expr(sp[k]->right.get());
        }
        else if (auto* x = node_as<AssignmentExpression>(e)) { expr(x->object.get()); expr(x->index.get()); expr(x->value.get()); }
        else if (auto* x = node_as<TernaryExpression>(e)) { expr(x->condition.get()); expr(x->then_expr.get()); expr(x->else_expr.get()); }
        else if (auto* x = node_as<MemberAccessExpression>(e)) expr(x->object.get());
        else if (auto* x = node_as<StructLiteralExpression>(e)) { for (auto& f : x->fields) expr(f.second.get()); }
    }
};

// İndeks/üye yolunun anahtarı yan etkisiz mi (iki kez okunabilir mi)?
bool pure_key(const Expression* e) {
    if (!e) return false;
    switch (e->kind) {
        case NodeKind::NumberLiteral: case NodeKind::FloatLiteral: case NodeKind::BooleanLiteral:
        case NodeKind::NullLiteral: case NodeKind::Variable:
            return true;
        case NodeKind::StringLiteral: {
            const auto& s = static_cast<const StringLiteral&>(*e);
            return s.raw || s.value.find('{') == std::string::npos;   // {$…} parçası: temkinli
        }
        case NodeKind::UnaryExpression: {
            const auto& u = static_cast<const UnaryExpression&>(*e);
            return (u.op == "-" || u.op == "!") && pure_key(u.right.get());
        }
        case NodeKind::BinaryExpression: {
            const auto& b = static_cast<const BinaryExpression&>(*e);
            return pure_key(b.left.get()) && pure_key(b.right.get());
        }
        case NodeKind::IndexExpression: {
            const auto& x = static_cast<const IndexExpression&>(*e);
            return pure_key(x.object.get()) && pure_key(x.index.get());
        }
        case NodeKind::MemberAccessExpression:
            return pure_key(static_cast<const MemberAccessExpression&>(*e).object.get());
        default:
            return false;
    }
}

// Yan etkisiz anahtardaki değişken adları.
void collect_vars(const Expression* e, std::set<std::string>& names) {
    if (!e) return;
    if (auto* v = node_as<Variable>(e)) names.insert(v->name);
    else if (auto* u = node_as<UnaryExpression>(e)) collect_vars(u->right.get(), names);
    else if (auto* b = node_as<BinaryExpression>(e)) { collect_vars(b->left.get(), names); collect_vars(b->right.get(), names); }
    else if (auto* x = node_as<IndexExpression>(e)) { collect_vars(x->object.get(), names); collect_vars(x->index.get(), names); }
    else if (auto* m = node_as<MemberAccessExpression>(e)) collect_vars(m->object.get(), names);
}

// X bir değişken ya da yan etkisiz anahtarlı indeks/üye yolu mu? names: yolda okunan değişkenler.
bool list_path(const Expression* e, std::set<std::string>& names) {
    std::vector<const Expression*> keys;
    for (const Expression* cur = e; cur; ) {
        if (auto* v = node_as<Variable>(cur)) {
            if (v->name.empty() || v->name[0] != '$') return false;
            names.insert(v->name);
            for (auto* k : keys) {
                if (!pure_key(k)) return false;
                collect_vars(k, names);
            }
            return true;
        }
        if (auto* ix = node_as<IndexExpression>(cur)) { keys.push_back(ix->index.get()); cur = ix->object.get(); }
        else if (auto* ma = node_as<MemberAccessExpression>(cur)) cur = ma->object.get();
        else return false;
    }
    return false;
}

// Değer ifadesi bir değişkeni değiştiriyor mu (atama, ++/--, push/pop)? Closure gövdesine inilmez.
bool changes_vars(const Expression* e) {
    if (!e) return false;
    if (node_as<AssignmentExpression>(e)) return true;
    if (auto* u = node_as<UnaryExpression>(e)) return u->op == "++" || u->op == "--" || changes_vars(u->right.get());
    if (auto* c = node_as<CallExpression>(e)) {
        if (auto* v = bare_callee(c); v && (v->name == "push" || v->name == "pop")) return true;
        if (changes_vars(c->callee.get())) return true;
        for (auto& a : c->arguments) if (changes_vars(a.get())) return true;
        return false;
    }
    if (auto* b = node_as<BinaryExpression>(e)) {
        std::vector<const BinaryExpression*> sp;
        if (changes_vars(binary_left_spine(b, sp))) return true;
        for (auto* x : sp) if (changes_vars(x->right.get())) return true;
        return false;
    }
    if (auto* x = node_as<ArrayLiteral>(e)) { for (auto& el : x->elements) if (changes_vars(el.get())) return true; return false; }
    if (auto* x = node_as<AssocArrayLiteral>(e)) { for (auto& p : x->pairs) if (changes_vars(p.first.get()) || changes_vars(p.second.get())) return true; return false; }
    if (auto* x = node_as<IndexExpression>(e)) return changes_vars(x->object.get()) || changes_vars(x->index.get());
    if (auto* x = node_as<MemberAccessExpression>(e)) return changes_vars(x->object.get());
    if (auto* x = node_as<TernaryExpression>(e)) return changes_vars(x->condition.get()) || changes_vars(x->then_expr.get()) || changes_vars(x->else_expr.get());
    if (auto* x = node_as<StructLiteralExpression>(e)) { for (auto& f : x->fields) if (changes_vars(f.second.get())) return true; return false; }
    if (auto* x = node_as<StringLiteral>(e)) return !x->raw && x->value.find('{') != std::string::npos &&
        (x->value.find('=') != std::string::npos || x->value.find("++") != std::string::npos ||
         x->value.find("--") != std::string::npos || x->value.find("push") != std::string::npos ||
         x->value.find("pop") != std::string::npos);
    return false;
}

// Ham kaynak aralığında (string dışında) yorum var mı? Yeniden yazma onu kodun ortasına taşırdı.
bool has_comment(const std::string& s, size_t a, size_t b) {
    char q = 0;
    for (size_t i = a; i < b; ++i) {
        const char c = s[i];
        if (q) {
            if (c == '\\' && q != '`') { ++i; continue; }
            if (c == q) q = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') { q = c; continue; }
        if (c == '#') return true;
        if (c == '/' && i + 1 < b && (s[i + 1] == '/' || s[i + 1] == '*')) return true;
    }
    return false;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Gösterim için tek satır: satır sonu + girinti → ayraç.
std::string one_line(const std::string& s, const char* sep) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' || s[i] == '\n') {
            while (i + 1 < s.size() && (s[i + 1] == ' ' || s[i + 1] == '\t' || s[i + 1] == '\n' || s[i + 1] == '\r')) ++i;
            out += sep;
        } else out += s[i];
    }
    return out;
}

enum Kind { K_PUSH, K_POP, K_POP_ASSIGN, K_ARRAY_POP };

struct Edit {
    size_t start = 0, end = 0;                        // değiştirilen kaynak aralığı [start, end)
    Kind kind = K_PUSH;
    std::vector<std::pair<size_t, size_t>> args;      // argümanların ham aralıkları
    std::string var;                                  // K_POP_ASSIGN: $v
    std::string indent;                               // K_POP_ASSIGN: satır sonu (CRLF dosyada CRLF) + ikinci satırın girintisi
    bool paren = false;                               // K_ARRAY_POP: X'i parantezle
    int line = 0;
};

struct Rewriter {
    const std::string& src;
    std::vector<Edit> edits;   // başa göre artan; aynı başta dıştaki önce

    std::string render(size_t s, size_t e) const {
        std::string out;
        size_t pos = s;
        for (const auto& E : edits) {
            if (E.start >= e) break;
            if (E.start < pos || E.end > e) continue;   // aralık dışı ya da işlenmiş bir düzenlemenin içi
            out.append(src, pos, E.start - pos);
            out += render_edit(E);
            pos = E.end;
        }
        out.append(src, pos, e - pos);
        return out;
    }

    std::string render_edit(const Edit& E) const {
        auto arg = [&](size_t k) { return trim(render(E.args[k].first, E.args[k].second)); };
        const std::string X = arg(0);
        switch (E.kind) {
            case K_PUSH:       return X + " = array::push(" + X + ", " + arg(1) + ")";
            case K_POP:        return X + " = array::slice(" + X + ", 0, -1)";
            case K_POP_ASSIGN: return E.var + " = " + X + "[-1]" + E.indent + X + " = array::slice(" + X + ", 0, -1)";
            case K_ARRAY_POP:  return (E.paren ? "(" + X + ")" : X) + "[-1]";
        }
        return "";
    }
};

} // namespace

FixResult look_fix_source(const std::string& src) {
    FixResult r;
    r.output = src;
    std::vector<Token> toks;
    std::unique_ptr<Program> program;
    try {
        toks = Lexer(src).scan_tokens();
        Parser parser(toks);
        program = parser.parse();
    } catch (const LookParseError& e) {
        r.error = "cannot parse (line " + std::to_string(e.line) + "): " + e.message;
        return r;
    } catch (const std::exception& e) {
        r.error = std::string("cannot parse: ") + e.what();
        return r;
    }

    AstIndex ix;
    Walker w{ix};
    try {
        for (const auto& s : program->statements) w.stmt(s.get(), true);
    } catch (const std::exception& e) {
        r.error = std::string("cannot process: ") + e.what();
        return r;
    }

    // Token → bayt konumu: kaynağı token sırasıyla yürü (boşluk ve yorumları atla, token metni
    // kaynaktaki ham metindir). Token sütunu buna yetmez (satır içinde kayabiliyor). ASI'nin eklediği
    // sanal ';' kaynakta yoktur (-1). Metin uymazsa o noktadan sonrası bilinmez (-1): oradaki çağrı
    // yeniden yazılmaz (bozma yok).
    std::vector<long> offs(toks.size(), -1);
    {
        size_t pos = (src.size() >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB &&
                      (unsigned char)src[2] == 0xBF) ? 3 : 0;
        for (size_t k = 0; k < toks.size(); ++k) {
            for (;;) {   // boşluk ve yorum
                while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r' || src[pos] == '\n')) ++pos;
                if (pos < src.size() && (src[pos] == '#' || src.compare(pos, 2, "//") == 0)) {
                    while (pos < src.size() && src[pos] != '\n') ++pos;
                } else if (src.compare(pos, 2, "/*") == 0) {
                    const size_t e = src.find("*/", pos + 2);
                    pos = e == std::string::npos ? src.size() : e + 2;
                } else break;
            }
            const Token& t = toks[k];
            if (t.type == TokenType::EOF_TOKEN) break;
            if (!t.lexeme.empty() && src.compare(pos, t.lexeme.size(), t.lexeme) == 0) {
                offs[k] = (long)pos;
                pos += t.lexeme.size();
            } else if (t.type != TokenType::SEMICOLON) {
                break;   // beklenmeyen: geri kalan bilinmiyor
            }
        }
    }

    std::vector<Edit> edits;
    auto skip = [&](int line, const std::string& why) {
        r.skipped.push_back(std::to_string(line) + ": not rewritten: " + why);
    };

    for (size_t i = 0; i + 1 < toks.size(); ++i) {
        const Token& t = toks[i];
        if (t.type != TokenType::IDENT) continue;
        bool bare = false, apop = false;
        size_t lp = 0;   // '(' token'ı
        if ((t.lexeme == "push" || t.lexeme == "pop") && toks[i + 1].type == TokenType::LPAREN &&
            !(i > 0 && (toks[i - 1].type == TokenType::COLON_COLON || toks[i - 1].type == TokenType::FUNCTION))) {
            bare = true; lp = i + 1;
        } else if (t.lexeme == "array" && i + 3 < toks.size() && toks[i + 1].type == TokenType::COLON_COLON &&
                   toks[i + 2].type == TokenType::IDENT && toks[i + 2].lexeme == "pop" &&
                   toks[i + 3].type == TokenType::LPAREN) {
            apop = true; lp = i + 3;
        }
        if (!bare && !apop) continue;
        const std::string name = apop ? "array::pop()" : t.lexeme + "()";

        // Eşleşen ')' ve üst düzey virgüller.
        size_t rp = 0; int depth = 0;
        std::vector<size_t> commas;
        for (size_t k = lp; k < toks.size(); ++k) {
            const TokenType ty = toks[k].type;
            if (ty == TokenType::LPAREN || ty == TokenType::LBRACKET || ty == TokenType::LBRACE) ++depth;
            else if (ty == TokenType::RPAREN || ty == TokenType::RBRACKET || ty == TokenType::RBRACE) {
                if (--depth == 0) { rp = k; break; }
            } else if (ty == TokenType::COMMA && depth == 1) commas.push_back(k);
        }
        if (!rp) { skip(t.line, "could not find the end of the " + name + " call"); continue; }
        std::vector<std::pair<size_t, size_t>> args;
        {
            long a = offs[lp];
            bool ok = a >= 0;
            size_t from = (size_t)a + 1;
            for (size_t c : commas) {
                long o = offs[c];
                if (o < 0) { ok = false; break; }
                args.push_back({from, (size_t)o}); from = (size_t)o + 1;
            }
            long e = offs[rp];
            if (e < 0) ok = false;
            if (ok && !(commas.empty() && trim(src.substr(from, (size_t)e - from)).empty()))
                args.push_back({from, (size_t)e});
            if (!ok) { skip(t.line, "could not locate the " + name + " call in the source"); continue; }
        }

        if (offs[i] < 0) { skip(t.line, "could not locate the " + name + " call in the source"); continue; }
        Edit E;
        E.line = t.line;
        E.end = (size_t)offs[rp] + 1;
        E.args = args;
        const CallExpression* call = nullptr;
        if (apop) {
            auto it = ix.array_pops.find({t.line, t.column});
            if (it == ix.array_pops.end()) { skip(t.line, "could not locate the array::pop() call in the source"); continue; }
            call = it->second;
            if (call->arguments.size() != 1 || args.size() != 1) { skip(t.line, "array::pop() takes one argument"); continue; }
            E.kind = K_ARRAY_POP;
            E.start = (size_t)offs[i];
            const Expression* x = call->arguments[0].get();
            E.paren = !(node_as<Variable>(x) || node_as<IndexExpression>(x) || node_as<MemberAccessExpression>(x) ||
                        node_as<CallExpression>(x));
        } else {
            auto it = ix.stmt_calls.find({t.line, t.column});
            const AssignmentExpression* asg = nullptr;
            if (it != ix.stmt_calls.end() && bare_callee(it->second)->name == t.lexeme) {
                call = it->second;
                E.kind = t.lexeme == "push" ? K_PUSH : K_POP;
                E.start = (size_t)offs[i];
            } else if (t.lexeme == "pop" && i >= 2 && toks[i - 1].type == TokenType::ASSIGN &&
                       toks[i - 2].type == TokenType::IDENT && !toks[i - 2].lexeme.empty() && toks[i - 2].lexeme[0] == '$') {
                auto it2 = ix.assign_pops.find({toks[i - 2].line, toks[i - 2].column});
                if (it2 != ix.assign_pops.end()) {
                    asg = it2->second;
                    call = node_as<CallExpression>(asg->value.get());
                    E.kind = K_POP_ASSIGN;
                    E.var = toks[i - 2].lexeme;
                    const long vs = offs[i - 2];
                    if (vs < 0) { skip(t.line, "could not locate the pop() call in the source"); continue; }
                    E.start = (size_t)vs;
                    size_t ls = (size_t)vs;   // satırın başı (girinti ikinci satıra da)
                    while (ls > 0 && src[ls - 1] != '\n') --ls;
                    size_t le = ls;
                    while (le < src.size() && (src[le] == ' ' || src[le] == '\t')) ++le;
                    const size_t nl = src.find('\n', (size_t)vs);   // dosyanın satır sonu biçimi korunur
                    E.indent = std::string(nl != std::string::npos && nl > 0 && src[nl - 1] == '\r' ? "\r\n" : "\n") +
                               src.substr(ls, le - ls);
                }
            }
            if (!call) {
                skip(t.line, name + " is used inside a larger expression (only a statement push(X, v), pop(X) "
                             "or $v = pop(X) is rewritten)");
                continue;
            }
            const size_t want = E.kind == K_PUSH ? 2 : 1;
            if (call->arguments.size() != want || args.size() != want) {
                skip(t.line, name + (want == 2 ? " takes two arguments" : " takes one argument"));
                continue;
            }
            std::set<std::string> names;
            if (!list_path(call->arguments[0].get(), names)) {
                skip(t.line, "the list in " + name + " is not a variable or an index/member path");
                continue;
            }
            if (E.kind == K_PUSH && changes_vars(call->arguments[1].get())) {
                skip(t.line, "the value given to push() changes a variable");
                continue;
            }
            if (asg && names.count(asg->name)) {
                skip(t.line, "the result of pop() is assigned to a variable the list uses");
                continue;
            }
        }
        if (has_comment(src, E.start, E.end)) { skip(t.line, "a comment inside the " + name + " call"); continue; }
        edits.push_back(std::move(E));
    }

    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) {
        return a.start != b.start ? a.start < b.start : a.end > b.end;
    });
    Rewriter rw{src, edits};
    // Gösterim: yalnız en dıştaki düzenlemeler (içtekiler onların metninde).
    size_t outer_end = 0;
    for (const auto& E : rw.edits) {
        if (E.kind == K_ARRAY_POP)
            r.notes.push_back(std::to_string(E.line) + ": NOTE: array::pop(X) became X[-1]; on an empty list "
                              "X[-1] stops with an error where array::pop() returned null");
        else if (E.kind == K_POP_ASSIGN)
            r.notes.push_back(std::to_string(E.line) + ": NOTE: $v = pop(X) became $v = X[-1]; on an empty list "
                              "X[-1] stops with an error where pop() returned null");
        if (E.start < outer_end) continue;
        outer_end = E.end;
        r.changes.push_back(std::to_string(E.line) + ": " + one_line(src.substr(E.start, E.end - E.start), " ") +
                            "  ->  " + one_line(rw.render_edit(E), "; "));
    }
    if (edits.empty()) return r;

    std::string out = rw.render(0, src.size());
    // push()/pop() global fonksiyonlardı; yeni yazım array:: kullanır → `use array` yoksa ekle (yoksa
    // dönüştürülen dosya "Module 'array' not loaded." ile durur). array::pop → X[-1] modül gerektirmez.
    bool needs_array = false;
    for (const auto& E : rw.edits) needs_array |= E.kind != K_ARRAY_POP;
    bool has_use_array = false;
    for (const auto& s : program->statements)
        if (auto* u = node_as<UseStatement>(s.get()); u && u->module_name == "array" && u->alias.empty())
            has_use_array = true;
    if (needs_array && !has_use_array) {
        const std::string nl = src.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        size_t at = 0;
        if (out.rfind("#!", 0) == 0) {   // shebang ilk satırda kalmalı
            size_t e = out.find('\n');
            at = e == std::string::npos ? out.size() : e + 1;
            if (e == std::string::npos) out += nl, at = out.size();
        }
        out.insert(at, "use array" + nl);
        r.changes.push_back("1: (no use array)  ->  use array");
    }
    // Güvence: sonuç ayrışmalı. Ayrışmazsa hiçbir şey yazılmaz.
    try {
        Parser p(Lexer(out).scan_tokens());
        p.parse();
    } catch (const LookParseError& e) {
        r.error = "the rewritten source does not parse (line " + std::to_string(e.line) + ": " + e.message +
                  "); file unchanged";
        return r;
    } catch (const std::exception& e) {
        r.error = std::string("the rewritten source does not parse (") + e.what() + "); file unchanged";
        return r;
    }
    r.output = std::move(out);
    return r;
}

int run_fix(const std::vector<std::string>& files, bool check) {
    auto report = [](std::ostream& os, const std::string& file, const FixResult& r, bool list_changes) {
        if (list_changes) for (const auto& c : r.changes) os << file << ":" << c << "\n";
        for (const auto& n : r.notes) os << file << ":" << n << "\n";
        for (const auto& s : r.skipped) os << file << ":" << s << "\n";
    };

    if (files.empty()) {   // stdin → stdout; raporlar stderr'e
        std::stringstream ss; ss << std::cin.rdbuf();
        FixResult r = look_fix_source(ss.str());
        if (!r.error.empty()) {
            std::cout << ss.str();
            std::cerr << "lk fix: <stdin>: " << r.error << "\n";
            return 2;
        }
        if (!check) std::cout << r.output;
        report(std::cerr, "<stdin>", r, check);
        if (check && !r.changes.empty()) return 1;
        return r.skipped.empty() ? 0 : 1;
    }

    int rc = 0;
    for (const std::string& file : files) {
        std::string src;
        {
            std::ifstream f(file, std::ios::binary);
            if (!f) { std::cerr << "lk fix: could not read: " << file << "\n"; rc = 2; continue; }
            std::stringstream ss; ss << f.rdbuf(); src = ss.str();
        }
        FixResult r = look_fix_source(src);
        if (!r.error.empty()) { std::cerr << "lk fix: " << file << ": " << r.error << "\n"; rc = 2; continue; }
        report(std::cout, file, r, check);
        if (!r.skipped.empty() && rc == 0) rc = 1;
        if (r.changes.empty()) continue;
        if (check) {
            if (rc == 0) rc = 1;
        } else {
            std::ofstream of(file, std::ios::binary | std::ios::trunc);
            of << r.output;
            if (!of) { std::cerr << "lk fix: could not write: " << file << "\n"; rc = 2; continue; }
            std::cout << "fixed: " << file << " (" << r.changes.size() << (r.changes.size() == 1 ? " rewrite" : " rewrites") << ")\n";
        }
    }
    return rc;
}

} // namespace look
