#pragma once

#include <memory>
#include <string>
#include <vector>

namespace look {

struct SourceLocation {
    std::string file;
    int line   = 0;
    int column = 0;
};

struct Expression {
    SourceLocation loc;
    virtual ~Expression() = default;
};

struct Statement {
    SourceLocation loc;
    virtual ~Statement() = default;
};

// ── Literals ──────────────────────────────────────────────────────────────────

struct StringLiteral final : Expression {
    std::string value;
    explicit StringLiteral(std::string v) : value(std::move(v)) {}
};

struct NumberLiteral final : Expression {
    int value;
    explicit NumberLiteral(int v) : value(v) {}
};

struct FloatLiteral final : Expression {
    double value;
    explicit FloatLiteral(double v) : value(v) {}
};

struct BooleanLiteral final : Expression {
    bool value;
    explicit BooleanLiteral(bool v) : value(v) {}
};

struct NullLiteral final : Expression {};

// $arr = [1, 2, "hello"]
struct ArrayLiteral final : Expression {
    std::vector<std::unique_ptr<Expression>> elements;
};

// $map = ["key" => "val", "x" => 1]
struct AssocArrayLiteral final : Expression {
    std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> pairs;
};

// Forward declare BlockStatement for FunctionExpression
struct BlockStatement;

// function($a, $b) use ($conn, $db) { ... }  — closure with explicit capture
struct FunctionExpression final : Expression {
    std::vector<std::string> parameters;
    std::vector<std::string> captures;   // use ($conn, $db) listesi
    bool is_variadic = false;            // son param ...$args ise true
    std::unique_ptr<BlockStatement> body;
};

// ── Expressions ───────────────────────────────────────────────────────────────

struct Variable final : Expression {
    std::string name;
    explicit Variable(std::string n) : name(std::move(n)) {}
};

// $arr[0]  /  $arr[$i]
struct IndexExpression final : Expression {
    std::unique_ptr<Expression> object;
    std::unique_ptr<Expression> index;
    IndexExpression(std::unique_ptr<Expression> obj, std::unique_ptr<Expression> idx)
        : object(std::move(obj)), index(std::move(idx)) {}
};

struct UnaryExpression final : Expression {
    std::string op;
    std::unique_ptr<Expression> right;
    bool prefix;
    UnaryExpression(std::string op, std::unique_ptr<Expression> right, bool prefix = true)
        : op(std::move(op)), right(std::move(right)), prefix(prefix) {}
};

struct BinaryExpression final : Expression {
    std::unique_ptr<Expression> left;
    std::string op;
    std::unique_ptr<Expression> right;
    BinaryExpression(std::unique_ptr<Expression> l, std::string op, std::unique_ptr<Expression> r)
        : left(std::move(l)), op(std::move(op)), right(std::move(r)) {}
};

// $x = expr  /  $x += expr  /  $arr[i] = expr
struct AssignmentExpression final : Expression {
    std::string name;
    std::string op;
    std::unique_ptr<Expression> index;   // nullable — set for $arr[i] = ...
    std::unique_ptr<Expression> value;
    AssignmentExpression(std::string name, std::string op,
                         std::unique_ptr<Expression> val,
                         std::unique_ptr<Expression> idx = nullptr)
        : name(std::move(name)), op(std::move(op)),
          index(std::move(idx)), value(std::move(val)) {}
};

struct CallExpression final : Expression {
    std::unique_ptr<Expression> callee;
    std::vector<std::unique_ptr<Expression>> arguments;
    CallExpression(std::unique_ptr<Expression> callee, std::vector<std::unique_ptr<Expression>> args)
        : callee(std::move(callee)), arguments(std::move(args)) {}
};

// $x ? $a : $b
struct TernaryExpression final : Expression {
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> then_expr;
    std::unique_ptr<Expression> else_expr;
    TernaryExpression(std::unique_ptr<Expression> cond,
                      std::unique_ptr<Expression> then_e,
                      std::unique_ptr<Expression> else_e)
        : condition(std::move(cond)), then_expr(std::move(then_e)), else_expr(std::move(else_e)) {}
};

struct ScopeResolution final : Expression {
    std::string module_name;
    std::string member_name;
    ScopeResolution(std::string mod, std::string mem)
        : module_name(std::move(mod)), member_name(std::move(mem)) {}
};

// ── Statements ────────────────────────────────────────────────────────────────

struct ExpressionStatement final : Statement {
    std::unique_ptr<Expression> expression;
    explicit ExpressionStatement(std::unique_ptr<Expression> e) : expression(std::move(e)) {}
};

// print(expr, ...)  → output + newline
struct PrintStatement final : Statement {
    std::vector<std::unique_ptr<Expression>> expressions;
    explicit PrintStatement(std::vector<std::unique_ptr<Expression>> exprs)
        : expressions(std::move(exprs)) {}
};

// write(expr, ...)  → output, no newline
struct WriteStatement final : Statement {
    std::vector<std::unique_ptr<Expression>> expressions;
    explicit WriteStatement(std::vector<std::unique_ptr<Expression>> exprs)
        : expressions(std::move(exprs)) {}
};

struct ReturnStatement final : Statement {
    std::unique_ptr<Expression> expression;
    explicit ReturnStatement(std::unique_ptr<Expression> e) : expression(std::move(e)) {}
};

struct BreakStatement    final : Statement {};
struct ContinueStatement final : Statement {};

struct UseStatement final : Statement {
    std::string module_name;
    std::string alias;  // empty = no alias
    explicit UseStatement(std::string name, std::string alias = "")
        : module_name(std::move(name)), alias(std::move(alias)) {}
};

// use "routes/auth.lk"; — dosya modül sistemi (Phase 18.5)
// Sadece function ve const tanımları caller scope'a aktarılır; $var paylaşılmaz.
struct UseFileStatement final : Statement {
    std::string path;   // "routes/auth.lk" gibi — tırnak içindeki değer
    explicit UseFileStatement(std::string p) : path(std::move(p)) {}
};

struct BlockStatement final : Statement {
    std::vector<std::unique_ptr<Statement>> statements;
};

struct IfStatement final : Statement {
    std::unique_ptr<Expression> condition;
    std::unique_ptr<BlockStatement> then_branch;
    std::unique_ptr<BlockStatement> else_branch; // nullable
    IfStatement(std::unique_ptr<Expression> cond,
                std::unique_ptr<BlockStatement> then_b,
                std::unique_ptr<BlockStatement> else_b)
        : condition(std::move(cond)), then_branch(std::move(then_b)), else_branch(std::move(else_b)) {}
};

struct WhileStatement final : Statement {
    std::unique_ptr<Expression> condition;
    std::unique_ptr<BlockStatement> body;
    WhileStatement(std::unique_ptr<Expression> cond, std::unique_ptr<BlockStatement> body)
        : condition(std::move(cond)), body(std::move(body)) {}
};

struct ForStatement final : Statement {
    std::unique_ptr<Statement>  init;
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> post;
    std::unique_ptr<BlockStatement> body;
};

// foreach ($arr as $val)
// foreach ($arr as $key => $val)
struct ForeachStatement final : Statement {
    std::unique_ptr<Expression> iterable;
    std::string key_var;   // empty = no key
    std::string value_var;
    std::unique_ptr<BlockStatement> body;
};

// try { } catch ($e) { } finally { }
struct TryCatchStatement final : Statement {
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

struct SwitchStatement final : Statement {
    std::unique_ptr<Expression> subject;
    std::vector<SwitchCase> cases;
};

// iota keyword — only meaningful inside const block
struct IotaExpression final : Expression {};

// $obj.field  — member access on struct/assoc array
struct MemberAccessExpression final : Expression {
    std::unique_ptr<Expression> object;
    std::string field;
    MemberAccessExpression(std::unique_ptr<Expression> obj, std::string f)
        : object(std::move(obj)), field(std::move(f)) {}
};

// struct Kullanici { ad, yas, aktif }
// struct Urun { ad, fiyat: 0.0, stok: 0 }
struct StructField {
    std::string name;
    std::unique_ptr<Expression> default_expr; // nullable
};

struct StructDeclaration final : Statement {
    std::string name;
    std::vector<StructField> fields;
};

// Kullanici{ad: "Ali", yas: 30}
struct StructLiteralExpression final : Expression {
    std::string struct_name;
    std::vector<std::pair<std::string, std::unique_ptr<Expression>>> fields;
};

// const { X = iota, Y, Z = 200 }
struct ConstItem {
    std::string name;
    std::unique_ptr<Expression> value; // nullable = implicit iota
};

struct ConstBlock final : Statement {
    std::vector<ConstItem> items;
};

struct FunctionDeclaration final : Statement {
    std::string name;
    std::vector<std::string> parameters;
    bool is_variadic = false;            // son param ...$args ise true
    std::unique_ptr<BlockStatement> body;
    FunctionDeclaration(std::string name, std::vector<std::string> params, bool variadic, std::unique_ptr<BlockStatement> body)
        : name(std::move(name)), parameters(std::move(params)), is_variadic(variadic), body(std::move(body)) {}
};

struct Program {
    std::vector<std::unique_ptr<Statement>> statements;
};

} // namespace look
