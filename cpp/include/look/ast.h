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

// Node kind tag. The compiler and load-time checks ask "which kind?" at every node;
// a tag comparison instead of a dynamic_cast chain (10–20 attempts): node_as<T>(p).
// Every concrete node is `final` and derives directly from Expression/Statement → tag equality
// gives the same result as dynamic_cast.
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

// Base of a concrete node: sets the tag in the constructor, carries the type's tag as KIND.
template <NodeKind K> struct ExprNode : Expression { static constexpr NodeKind KIND = K; ExprNode() : Expression(K) {} };
template <NodeKind K> struct StmtNode : Statement  { static constexpr NodeKind KIND = K; StmtNode() : Statement(K) {} };

// T* if p is of type T, nullptr otherwise (or if p is null) — same as dynamic_cast<const T*>(p).
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
    std::vector<std::unique_ptr<Expression>> defaults;  // parallel; nullptr = no default
    std::vector<std::string> captures;   // use ($conn, $db) listesi
    bool is_variadic = false;            // true if the last param is ...$args
    std::unique_ptr<BlockStatement> body;
    // Names the closure needs from outside (filled by the parser — look/capture_check.h).
    // Read by the capture checks (capture_check.h, param_write_check.h).
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
    std::unique_ptr<Expression> object;  // nullable — chained lvalue container
                                         // ($l.s.x → object=$l.s, index="x").
                                         // if set, `name` is not used.
    std::unique_ptr<Expression> index;   // nullable — set for $arr[i] = ...
    std::unique_ptr<Expression> value;
    // `$a[k]++` / `$a[k]--` (postfix): the value of the expression is the value BEFORE the write (like ++ on a variable).
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
    std::unique_ptr<Expression> expression;   // thrown value (string, error::new, etc.)
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

// use "routes/auth.lk"; — file module system (Phase 18.5)
// Only function and const definitions are carried into the caller scope; $var is not shared.
struct UseFileStatement final : StmtNode<NodeKind::UseFileStatement> {
    std::string path;   // like "routes/auth.lk" — the value inside the quotes
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

// struct User { name, age, active }
// struct Product { name, price: 0.0, stock: 0 }
struct StructField {
    std::string name;
    std::string type;                         // "" = untyped, "int", "?string", "User"
    std::unique_ptr<Expression> default_expr; // nullable
};

struct StructDeclaration final : StmtNode<NodeKind::StructDeclaration> {
    std::string name;
    std::vector<StructField> fields;
    // Definition signature (field names, types, text of the default expressions): to compare whether a second
    // declaration with the same name is EXACTLY the same. Filled by the parser.
    std::string signature;
    int decl_line = 0;
};

// User{name: "Ali", age: 30}
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
    std::vector<std::unique_ptr<Expression>> defaults;  // parallel; nullptr = no default
    bool is_variadic = false;            // true if the last param is ...$args
    std::unique_ptr<BlockStatement> body;
    // Filled ONLY for an INNER function (declared inside another function) — that is also a
    // closure and takes the VALUE of the enclosing function's locals (look/capture_check.h).
    std::vector<std::string> free_names;
    FunctionDeclaration(std::string name, std::vector<std::string> params, bool variadic, std::unique_ptr<BlockStatement> body)
        : name(std::move(name)), parameters(std::move(params)), is_variadic(variadic), body(std::move(body)) {}
};

struct Program {
    std::vector<std::unique_ptr<Statement>> statements;
};

} // namespace look
