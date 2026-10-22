#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace look {

struct SourceLocation {
    std::string file;
    int line   = 0;
    int column = 0;
};

// Düğüm türü etiketi. Derleyici ve yükleme denetimleri her düğümde "hangi tür?" sorar;
// dynamic_cast zinciri (10–20 deneme) yerine etiket karşılaştırması: node_as<T>(p).
// Her somut düğüm `final` ve doğrudan Expression/Statement'tan türer → etiket eşitliği
// dynamic_cast ile aynı sonucu verir.
enum class NodeKind : uint8_t {
    StringLiteral, NumberLiteral, FloatLiteral, BooleanLiteral, NullLiteral, ArrayLiteral,
    AssocArrayLiteral, FunctionExpression, Variable, IndexExpression, UnaryExpression,
    BinaryExpression, AssignmentExpression, CallExpression, TernaryExpression, ScopeResolution,
    IotaExpression, MemberAccessExpression, StructLiteralExpression,
    ExpressionStatement, PrintStatement, WriteStatement, ReturnStatement, ThrowStatement,
    BreakStatement, ContinueStatement, UseStatement, UseFileStatement, BlockStatement,
    IfStatement, WhileStatement, ForStatement, ForeachStatement, TryCatchStatement,
    SwitchStatement, StructDeclaration, ConstBlock, FunctionDeclaration,
};

struct Expression {
    SourceLocation loc;
    NodeKind kind;
    virtual ~Expression() = default;
protected:
    explicit Expression(NodeKind k) : kind(k) {}
};

struct Statement {
    SourceLocation loc;
    NodeKind kind;
    virtual ~Statement() = default;
protected:
    explicit Statement(NodeKind k) : kind(k) {}
};

// Somut düğümün tabanı: etiketi yapıcıda koyar, türün etiketini KIND olarak taşır.
template <NodeKind K> struct ExprNode : Expression { static constexpr NodeKind KIND = K; ExprNode() : Expression(K) {} };
template <NodeKind K> struct StmtNode : Statement  { static constexpr NodeKind KIND = K; StmtNode() : Statement(K) {} };

// p T türündeyse T*, değilse (ya da p boşsa) nullptr — dynamic_cast<const T*>(p) ile aynı.
template <class T, class B> inline const T* node_as(const B* p) {
    return (p && p->kind == T::KIND) ? static_cast<const T*>(p) : nullptr;
}
template <class T, class B> inline T* node_as(B* p) {
    return (p && p->kind == T::KIND) ? static_cast<T*>(p) : nullptr;
}

// ── Literals ──────────────────────────────────────────────────────────────────

struct StringLiteral final : ExprNode<NodeKind::StringLiteral> {
    std::string value;
    bool raw = false;   // single-quoted: {$...} stays literal text
    explicit StringLiteral(std::string v, bool r = false) : value(std::move(v)), raw(r) {}
};

struct NumberLiteral final : ExprNode<NodeKind::NumberLiteral> {
    int64_t value;
    explicit NumberLiteral(int64_t v) : value(v) {}
};

struct FloatLiteral final : ExprNode<NodeKind::FloatLiteral> {
    double value;
    explicit FloatLiteral(double v) : value(v) {}
};

struct BooleanLiteral final : ExprNode<NodeKind::BooleanLiteral> {
    bool value;
    explicit BooleanLiteral(bool v) : value(v) {}
};

struct NullLiteral final : ExprNode<NodeKind::NullLiteral> {};

// $arr = [1, 2, "hello"]
struct ArrayLiteral final : ExprNode<NodeKind::ArrayLiteral> {
    std::vector<std::unique_ptr<Expression>> elements;
};

// $map = ["key" => "val", "x" => 1]
struct AssocArrayLiteral final : ExprNode<NodeKind::AssocArrayLiteral> {
    std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> pairs;
};

// Forward declare BlockStatement for FunctionExpression
struct BlockStatement;

// function($a, $b) use ($conn, $db) { ... }  — closure with explicit capture
struct FunctionExpression final : ExprNode<NodeKind::FunctionExpression> {
    std::vector<std::string> parameters;
    std::vector<std::unique_ptr<Expression>> defaults;  // paralel; nullptr = varsayılan yok
    std::vector<std::string> captures;   // use ($conn, $db) listesi
    bool is_variadic = false;            // son param ...$args ise true
    std::unique_ptr<BlockStatement> body;
    // LOOK 2: closure'ın dışarıdan istediği adlar (ayrıştırıcı doldurur — look/capture_check.h).
    // Yakalama denetimleri (capture_check.h, param_write_check.h) okur.
    std::vector<std::string> free_names;
};

// ── Expressions ───────────────────────────────────────────────────────────────

struct Variable final : ExprNode<NodeKind::Variable> {
    std::string name;
    explicit Variable(std::string n) : name(std::move(n)) {}
};

// $arr[0]  /  $arr[$i]
struct IndexExpression final : ExprNode<NodeKind::IndexExpression> {
    std::unique_ptr<Expression> object;
    std::unique_ptr<Expression> index;
    IndexExpression(std::unique_ptr<Expression> obj, std::unique_ptr<Expression> idx)
        : object(std::move(obj)), index(std::move(idx)) {}
};

struct UnaryExpression final : ExprNode<NodeKind::UnaryExpression> {
    std::string op;
    std::unique_ptr<Expression> right;
    bool prefix;
    UnaryExpression(std::string op, std::unique_ptr<Expression> right, bool prefix = true)
        : op(std::move(op)), right(std::move(right)), prefix(prefix) {}
};

struct BinaryExpression final : ExprNode<NodeKind::BinaryExpression> {
    std::unique_ptr<Expression> left;
    std::string op;
    std::unique_ptr<Expression> right;
    BinaryExpression(std::unique_ptr<Expression> l, std::string op, std::unique_ptr<Expression> r)
        : left(std::move(l)), op(std::move(op)), right(std::move(r)) {}
};

// $x = expr  /  $x += expr  /  $arr[i] = expr
struct AssignmentExpression final : ExprNode<NodeKind::AssignmentExpression> {
    std::string name;
    std::string op;
    std::unique_ptr<Expression> object;  // nullable — zincirli lvalue container
                                         // ($l.s.x → object=$l.s, index="x").
                                         // set ise `name` kullanılmaz.
    std::unique_ptr<Expression> index;   // nullable — set for $arr[i] = ...
    std::unique_ptr<Expression> value;
    // `$a[k]++` / `$a[k]--` (sonek): ifadenin değeri yazmadan ÖNCEKİ değerdir (değişkendeki ++ gibi).
    bool yield_old = false;
    AssignmentExpression(std::string name, std::string op,
                         std::unique_ptr<Expression> val,
                         std::unique_ptr<Expression> idx = nullptr,
                         std::unique_ptr<Expression> obj = nullptr)
        : name(std::move(name)), op(std::move(op)),
          object(std::move(obj)), index(std::move(idx)), value(std::move(val)) {}
};

struct CallExpression final : ExprNode<NodeKind::CallExpression> {
    std::unique_ptr<Expression> callee;
    std::vector<std::unique_ptr<Expression>> arguments;
    CallExpression(std::unique_ptr<Expression> callee, std::vector<std::unique_ptr<Expression>> args)
        : callee(std::move(callee)), arguments(std::move(args)) {}
};

// $x ? $a : $b
struct TernaryExpression final : ExprNode<NodeKind::TernaryExpression> {
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> then_expr;
    std::unique_ptr<Expression> else_expr;
    TernaryExpression(std::unique_ptr<Expression> cond,
                      std::unique_ptr<Expression> then_e,
                      std::unique_ptr<Expression> else_e)
        : condition(std::move(cond)), then_expr(std::move(then_e)), else_expr(std::move(else_e)) {}
};

struct ScopeResolution final : ExprNode<NodeKind::ScopeResolution> {
    std::string module_name;
    std::string member_name;
    ScopeResolution(std::string mod, std::string mem)
        : module_name(std::move(mod)), member_name(std::move(mem)) {}
};

// ── Statements ────────────────────────────────────────────────────────────────

struct ExpressionStatement final : StmtNode<NodeKind::ExpressionStatement> {
    std::unique_ptr<Expression> expression;
    explicit ExpressionStatement(std::unique_ptr<Expression> e) : expression(std::move(e)) {}
};

// print(expr, ...)  → output + newline
struct PrintStatement final : StmtNode<NodeKind::PrintStatement> {
    std::vector<std::unique_ptr<Expression>> expressions;
    explicit PrintStatement(std::vector<std::unique_ptr<Expression>> exprs)
        : expressions(std::move(exprs)) {}
};

// write(expr, ...)  → output, no newline
struct WriteStatement final : StmtNode<NodeKind::WriteStatement> {
    std::vector<std::unique_ptr<Expression>> expressions;
    explicit WriteStatement(std::vector<std::unique_ptr<Expression>> exprs)
        : expressions(std::move(exprs)) {}
};

struct ReturnStatement final : StmtNode<NodeKind::ReturnStatement> {
    std::unique_ptr<Expression> expression;
    explicit ReturnStatement(std::unique_ptr<Expression> e) : expression(std::move(e)) {}
};

struct ThrowStatement final : StmtNode<NodeKind::ThrowStatement> {
    std::unique_ptr<Expression> expression;   // fırlatılan değer (string, error::new, vb.)
    explicit ThrowStatement(std::unique_ptr<Expression> e) : expression(std::move(e)) {}
};

struct BreakStatement    final : StmtNode<NodeKind::BreakStatement> {};
struct ContinueStatement final : StmtNode<NodeKind::ContinueStatement> {};

struct UseStatement final : StmtNode<NodeKind::UseStatement> {
    std::string module_name;
    std::string alias;  // empty = no alias
    explicit UseStatement(std::string name, std::string alias = "")
        : module_name(std::move(name)), alias(std::move(alias)) {}
};

// use "routes/auth.lk"; — dosya modül sistemi (Phase 18.5)
// Sadece function ve const tanımları caller scope'a aktarılır; $var paylaşılmaz.
struct UseFileStatement final : StmtNode<NodeKind::UseFileStatement> {
    std::string path;   // "routes/auth.lk" gibi — tırnak içindeki değer
    explicit UseFileStatement(std::string p) : path(std::move(p)) {}
};

struct BlockStatement final : StmtNode<NodeKind::BlockStatement> {
    std::vector<std::unique_ptr<Statement>> statements;
};

struct IfStatement final : StmtNode<NodeKind::IfStatement> {
    std::unique_ptr<Expression> condition;
    std::unique_ptr<BlockStatement> then_branch;
    std::unique_ptr<BlockStatement> else_branch; // nullable
    IfStatement(std::unique_ptr<Expression> cond,
                std::unique_ptr<BlockStatement> then_b,
                std::unique_ptr<BlockStatement> else_b)
        : condition(std::move(cond)), then_branch(std::move(then_b)), else_branch(std::move(else_b)) {}
};

struct WhileStatement final : StmtNode<NodeKind::WhileStatement> {
    std::unique_ptr<Expression> condition;
    std::unique_ptr<BlockStatement> body;
    WhileStatement(std::unique_ptr<Expression> cond, std::unique_ptr<BlockStatement> body)
        : condition(std::move(cond)), body(std::move(body)) {}
};

struct ForStatement final : StmtNode<NodeKind::ForStatement> {
    std::unique_ptr<Statement>  init;
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> post;
    std::unique_ptr<BlockStatement> body;
};

// foreach ($arr as $val)
// foreach ($arr as $key => $val)
struct ForeachStatement final : StmtNode<NodeKind::ForeachStatement> {
    std::unique_ptr<Expression> iterable;
    std::string key_var;   // empty = no key
    std::string value_var;
    std::unique_ptr<BlockStatement> body;
};

// try { } catch ($e) { } finally { }
struct TryCatchStatement final : StmtNode<NodeKind::TryCatchStatement> {
    std::unique_ptr<BlockStatement> try_block;
    std::string catch_var;               // "$e" — nullable (catch without var)
    std::unique_ptr<BlockStatement> catch_block;  // nullable
    std::unique_ptr<BlockStatement> finally_block; // nullable
};

// switch($x) { case 1: ... case 2, 3: ... default: ... }
struct SwitchCase {
    std::vector<std::unique_ptr<Expression>> values; // empty = default
    std::vector<std::unique_ptr<Statement>> body;
};

struct SwitchStatement final : StmtNode<NodeKind::SwitchStatement> {
    std::unique_ptr<Expression> subject;
    std::vector<SwitchCase> cases;
};

// iota keyword — only meaningful inside const block
struct IotaExpression final : ExprNode<NodeKind::IotaExpression> {};

// $obj.field  — member access on struct/assoc array
struct MemberAccessExpression final : ExprNode<NodeKind::MemberAccessExpression> {
    std::unique_ptr<Expression> object;
    std::string field;
    MemberAccessExpression(std::unique_ptr<Expression> obj, std::string f)
        : object(std::move(obj)), field(std::move(f)) {}
};

// struct Kullanici { ad, yas, aktif }
// struct Urun { ad, fiyat: 0.0, stok: 0 }
struct StructField {
    std::string name;
    std::string type;                         // LOOK 2: "" = tipsiz, "int", "?string", "User"
    std::unique_ptr<Expression> default_expr; // nullable
};

struct StructDeclaration final : StmtNode<NodeKind::StructDeclaration> {
    std::string name;
    std::vector<StructField> fields;
    // Tanımın imzası (alan adları, tipler, varsayılan ifadelerin metni): aynı adla ikinci bir
    // bildirimin BİREBİR aynı olup olmadığını karşılaştırmak için. Ayrıştırıcı doldurur.
    std::string signature;
    int decl_line = 0;
};

// Kullanici{ad: "Ali", yas: 30}
struct StructLiteralExpression final : ExprNode<NodeKind::StructLiteralExpression> {
    std::string struct_name;
    std::vector<std::pair<std::string, std::unique_ptr<Expression>>> fields;
};

// const { X = iota, Y, Z = 200 }
struct ConstItem {
    std::string name;
    std::unique_ptr<Expression> value; // nullable = implicit iota
};

struct ConstBlock final : StmtNode<NodeKind::ConstBlock> {
    std::vector<ConstItem> items;
};

struct FunctionDeclaration final : StmtNode<NodeKind::FunctionDeclaration> {
    std::string name;
    std::vector<std::string> parameters;
    std::vector<std::unique_ptr<Expression>> defaults;  // paralel; nullptr = varsayılan yok
    bool is_variadic = false;            // son param ...$args ise true
    std::unique_ptr<BlockStatement> body;
    // LOOK 2: yalnız İÇ (başka bir fonksiyonun içinde bildirilen) fonksiyonda dolu — o da bir
    // closure'dır ve saran fonksiyonun yerellerinin DEĞERİNİ alır (look/capture_check.h).
    std::vector<std::string> free_names;
    FunctionDeclaration(std::string name, std::vector<std::string> params, bool variadic, std::unique_ptr<BlockStatement> body)
        : name(std::move(name)), parameters(std::move(params)), is_variadic(variadic), body(std::move(body)) {}
};

struct Program {
    std::vector<std::unique_ptr<Statement>> statements;
};

} // namespace look
