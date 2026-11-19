#include "look/parser.h"
#include "look/capture_check.h"
#include "look/param_write_check.h"
#include "look/stack_guard.h"
#include "look/runtime_host.h"
#include "look/logger.h"
#include <stdexcept>
#include <algorithm>
#include <cstdlib>

namespace look {

void look_throw_nested_too_deep(int line, int col) {
    throw LookParseError("this expression or block is nested too deeply to process on this thread's stack; "
                         "split it into smaller expressions or functions", line, col);
}

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

std::unique_ptr<Program> Parser::parse() {
    auto program = std::make_unique<Program>();
    while (!is_at_end()) {
        while (match(TokenType::SEMICOLON)) {}  // skip empty ; left by ASI
        if (is_at_end()) break;
        program->statements.push_back(statement());
    }
    // LOOK closure rule (look/capture_check.h): writing to a captured variable is an error AT LOAD
    // — before anything runs, in one place. "Stale value" cases are kept
    // as warnings; the caller (CLI, web startup, --check) displays them.
    for (auto& w : check_captures(*program)) {
        if (w.kind == "capture-write") throw LookParseError(w.message, w.line, w.column);
        warnings_.push_back(std::move(w));
    }
    // LOOK parameter-write check (look/param_write_check.h): a write that never reaches the caller.
    for (auto& w : check_param_writes(*program)) warnings_.push_back(std::move(w));
    // Deprecated calls (push / pop / array::pop): one line per file (position of the first call
    // + total) — an app with hundreds of calls must not flood the startup log.
    for (int k = 0; k < DEP_COUNT; ++k)
        if (deps_[k].calls > 1 && deps_[k].warn < warnings_.size())
            warnings_[deps_[k].warn].message = deprecation_message(k, deps_[k].calls);
    // `$l = array::push($l, $x)` / `$l = array::slice($l, 0, -1)` is compiled in place and skips
    // the call: it worked without `use array`, yet the path form (`$m["k"] = array::push(...)`)
    // gives "Module 'array' not loaded." — a hidden rule. One release of warning (lk --check; the VM
    // warns at run time), then an error together with push/pop. An included file may take the module from the parent file;
    // so it is reported only in --check, not at load (kind "use-array-missing").
    if (in_place_array_.calls > 0 && !has_use_array_) {
        CaptureWarning w;
        w.line = in_place_array_.line;
        w.column = in_place_array_.column;
        w.kind = "use-array-missing";
        w.message = "array:: is used without `use array`" +
            (in_place_array_.calls > 1 ? " (" + std::to_string(in_place_array_.calls) + " places in this file, the first here)"
                                       : std::string()) +
            "; it runs only because $l = array::push(...) and $l = array::slice($l, 0, -1) skip the module "
            "check (add `use array`; this will be an error in a later release; lk fix adds it)";
        warnings_.push_back(std::move(w));
    }
    return program;
}

// ── Statements ────────────────────────────────────────────────────────────────

std::unique_ptr<Statement> Parser::statement() {
    look_ast_guard(peek().line, peek().column);   // actual remaining stack (the counter alone is not enough)
    // Recursion guard: deeply nested block/if/while ({{{...}}}) overflows the parser stack —
    // same rationale as the expression guard. The counter is decremented via RAII.
    static constexpr int MAX_STMT_DEPTH = 150;
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& x) : d(x) { ++d; }
        ~DepthGuard() { --d; }
    } guard(stmt_depth_);
    if (stmt_depth_ > MAX_STMT_DEPTH)
        throw look::LookParseError("Expression/block nested too deep (max " +
                                   std::to_string(MAX_STMT_DEPTH) + ")",
                                   peek().line, peek().column);
    auto loc = cur_loc();
    auto set_loc = [&](std::unique_ptr<Statement> s) -> std::unique_ptr<Statement> {
        if (s) s->loc = loc;
        return s;
    };
    if (match(TokenType::USE))      return set_loc(use_statement());
    if (match(TokenType::PRINT))    return set_loc(print_statement());
    if (match(TokenType::WRITE))    return set_loc(write_statement());
    if (match(TokenType::IF))       return set_loc(if_statement());
    if (match(TokenType::WHILE))    return set_loc(while_statement());
    if (match(TokenType::FOR))      return set_loc(for_statement());
    if (match(TokenType::FOREACH))  return set_loc(foreach_statement());
    if (match(TokenType::RETURN))   return set_loc(return_statement());
    if (match(TokenType::THROW))    return set_loc(throw_statement());
    if (match(TokenType::FUNCTION)) return set_loc(function_declaration());
    if (match(TokenType::TRY))      return set_loc(try_statement());
    if (match(TokenType::SWITCH))   return set_loc(switch_statement());
    if (match(TokenType::STRUCT))   return set_loc(struct_declaration());
    if (match(TokenType::CONST_KW)) return set_loc(const_block());
    if (match(TokenType::BREAK))    { consume_semi(); auto s = std::make_unique<BreakStatement>(); s->loc = loc; return s; }
    if (match(TokenType::CONTINUE)) { consume_semi(); auto s = std::make_unique<ContinueStatement>(); s->loc = loc; return s; }
    if (match(TokenType::LBRACE))   return set_loc(block());
    return set_loc(expression_statement());
}

// switch($x) { case 1: ... case 2,3: ... default: ... }
// Go style: no break, automatic exit
std::unique_ptr<Statement> Parser::switch_statement() {
    consume(TokenType::LPAREN, "Expect '(' after 'switch'.");
    auto subject = expression();
    consume(TokenType::RPAREN, "Expect ')' after switch value.");
    consume(TokenType::LBRACE, "Expect '{' after switch(...).");

    auto stmt = std::make_unique<SwitchStatement>();
    stmt->subject = std::move(subject);

    while (!check(TokenType::RBRACE) && !is_at_end()) {
        SwitchCase sc;

        if (match(TokenType::DEFAULT)) {
            // default: — the values list stays empty
            consume(TokenType::COLON, "Expect ':' after 'default'.");
        } else {
            consume(TokenType::CASE, "Expect 'case' or 'default' in switch.");
            // case 1, 2, 3:
            sc.values.push_back(expression());
            while (match(TokenType::COMMA))
                sc.values.push_back(expression());
            consume(TokenType::COLON, "Expect ':' after case value.");
        }

        // case body — until the next case/default/} arrives
        while (!check(TokenType::CASE) && !check(TokenType::DEFAULT) &&
               !check(TokenType::RBRACE) && !is_at_end()) {
            sc.body.push_back(statement());
        }

        stmt->cases.push_back(std::move(sc));
    }

    consume(TokenType::RBRACE, "Expect '}' after switch body.");
    return stmt;
}

// print(expr, ...)  — parens is required
std::unique_ptr<Statement> Parser::try_statement() {
    auto stmt = std::make_unique<TryCatchStatement>();

    // try { }
    consume(TokenType::LBRACE, "Expect '{' after 'try'.");
    stmt->try_block = block();

    // ASI: a newline after } does not block catch/finally
    while (check(TokenType::SEMICOLON)) advance();

    // catch ($e) { }
    if (match(TokenType::CATCH)) {
        consume(TokenType::LPAREN, "Expect '(' after 'catch'.");
        auto var = consume(TokenType::IDENT, "Expect variable name in catch.");
        stmt->catch_var = var.lexeme;
        consume(TokenType::RPAREN, "Expect ')' after catch variable.");
        consume(TokenType::LBRACE, "Expect '{' after catch(...).");
        stmt->catch_block = block();
    }

    // ASI: a newline after } does not block finally
    while (check(TokenType::SEMICOLON)) advance();

    // finally { }
    if (match(TokenType::FINALLY)) {
        consume(TokenType::LBRACE, "Expect '{' after 'finally'.");
        stmt->finally_block = block();
    }

    return stmt;
}

std::unique_ptr<Statement> Parser::print_statement() {
    consume(TokenType::LPAREN, "Expect '(' after 'print'.");
    auto exprs = argument_list();
    consume(TokenType::RPAREN, "Expect ')' after print arguments.");
    consume_semi();
    return std::make_unique<PrintStatement>(std::move(exprs));
}

// write(expr, ...)  — parens is required, no newline
std::unique_ptr<Statement> Parser::write_statement() {
    consume(TokenType::LPAREN, "Expect '(' after 'write'.");
    auto exprs = argument_list();
    consume(TokenType::RPAREN, "Expect ')' after write arguments.");
    consume_semi();
    return std::make_unique<WriteStatement>(std::move(exprs));
}

std::unique_ptr<Statement> Parser::expression_statement() {
    auto expr = expression();
    consume_semi();
    return std::make_unique<ExpressionStatement>(std::move(expr));
}

std::unique_ptr<Statement> Parser::use_statement() {
    // use "routes/auth.lk"; — file module system
    if (check(TokenType::STRING)) {
        if (scope_depth_ > 0) {
            auto& t = peek();
            throw LookParseError(
                "use \"file\" is only allowed at top-level scope, not inside a function or closure",
                t.line, t.column);
        }
        auto tok = advance();
        std::string path = tok.literal.value_or(tok.lexeme);
        consume_semi();
        return std::make_unique<UseFileStatement>(std::move(path));
    }
    // use module; or use module as alias;
    auto name = consume(TokenType::IDENT, "Expect module name after 'use'.");
    std::string alias;
    if (match(TokenType::AS)) {
        auto al = consume(TokenType::IDENT, "Expect alias name after 'as'.");
        alias = al.lexeme;
    }
    consume_semi();
    if (name.lexeme == "array" && alias.empty()) has_use_array_ = true;
    return std::make_unique<UseStatement>(name.lexeme, alias);
}

std::unique_ptr<Statement> Parser::if_statement() {
    consume(TokenType::LPAREN, "Expect '(' after 'if'.");
    auto cond = expression();
    consume(TokenType::RPAREN, "Expect ')' after if condition.");
    consume(TokenType::LBRACE, "Expect '{' after if condition.");
    auto then_b = block();

    std::unique_ptr<BlockStatement> else_b = nullptr;

    // skip the ; coming from ASI — a newline after } does not block elseif/else
    while (check(TokenType::SEMICOLON)) advance();

    if (match(TokenType::ELSEIF)) {
        // elseif → wrap recursive if_statement in a block
        auto elif_stmt = if_statement();
        else_b = std::make_unique<BlockStatement>();
        else_b->statements.push_back(std::move(elif_stmt));
    } else if (match(TokenType::ELSE)) {
        if (match(TokenType::IF)) {
            // else if (two tokens) — same handling
            auto elif_stmt = if_statement();
            else_b = std::make_unique<BlockStatement>();
            else_b->statements.push_back(std::move(elif_stmt));
        } else {
            consume(TokenType::LBRACE, "Expect '{' after else.");
            else_b = block();
        }
    }
    return std::make_unique<IfStatement>(std::move(cond), std::move(then_b), std::move(else_b));
}

std::unique_ptr<Statement> Parser::while_statement() {
    consume(TokenType::LPAREN, "Expect '(' after 'while'.");
    auto cond = expression();
    consume(TokenType::RPAREN, "Expect ')' after while condition.");
    consume(TokenType::LBRACE, "Expect '{' after while condition.");
    auto body = block();
    return std::make_unique<WhileStatement>(std::move(cond), std::move(body));
}

std::unique_ptr<Statement> Parser::for_statement() {
    consume(TokenType::LPAREN, "Expect '(' after 'for'.");
    auto stmt = std::make_unique<ForStatement>();

    if (!check(TokenType::SEMICOLON))
        stmt->init = expression_statement();
    else
        consume(TokenType::SEMICOLON, "Expect ';'.");

    if (!check(TokenType::SEMICOLON))
        stmt->condition = expression();
    consume(TokenType::SEMICOLON, "Expect ';' after for condition.");

    if (!check(TokenType::RPAREN))
        stmt->post = expression();
    consume(TokenType::RPAREN, "Expect ')' after for clauses.");
    consume(TokenType::LBRACE, "Expect '{' after for.");
    stmt->body = block();
    return stmt;
}

// foreach ($arr as $val)
// foreach ($arr as $key => $val)
std::unique_ptr<Statement> Parser::foreach_statement() {
    consume(TokenType::LPAREN, "Expect '(' after 'foreach'.");
    auto iterable = expression();
    consume(TokenType::AS, "Expect 'as' in foreach.");

    auto stmt = std::make_unique<ForeachStatement>();
    stmt->iterable = std::move(iterable);

    auto first = consume(TokenType::IDENT, "Expect variable name in foreach.");

    if (match(TokenType::FAT_ARROW)) {
        // foreach ($arr as $key => $val)
        stmt->key_var = first.lexeme;
        auto val = consume(TokenType::IDENT, "Expect value variable after '=>'.");
        stmt->value_var = val.lexeme;
    } else {
        // foreach ($arr as $val)
        stmt->value_var = first.lexeme;
    }

    consume(TokenType::RPAREN, "Expect ')' after foreach.");
    consume(TokenType::LBRACE, "Expect '{' after foreach.");
    stmt->body = block();
    return stmt;
}

// struct Kullanici { ad, yas, aktif }
// struct Urun { ad, fiyat: 0.0, stok: 0 }
std::unique_ptr<Statement> Parser::struct_declaration() {
    auto name = consume(TokenType::IDENT, "Expect struct name after 'struct'.");
    // LOOK: a struct is declared only at TOP LEVEL (not inside a function, closure or block).
    // A declaration has a single place → a second definition with the same name but different content is always caught at load.
    if (scope_depth_ > 0 || stmt_depth_ > 1)
        throw LookParseError("struct '" + name.lexeme + "' must be declared at the top level of a "
            "file, not inside a function or a block", name.line, name.column);
    const size_t sig_from = current_;
    consume(TokenType::LBRACE, "Expect '{' after struct name.");

    auto stmt = std::make_unique<StructDeclaration>();
    stmt->name = name.lexeme;

    while (!check(TokenType::RBRACE) && !is_at_end()) {
        while (match(TokenType::SEMICOLON)) {}  // skip empty ; left by ASI
        if (check(TokenType::RBRACE) || is_at_end()) break;
        auto field_tok = consume(TokenType::IDENT, "Expect field name.");
        StructField sf;
        sf.name = field_tok.lexeme;
        // LOOK: a field has ONE spelling:  name type [= default]
        //   Type: [?]name  (int, float, string, bool, array, map, fn, any, StructName).
        // The two v1 spellings were removed (three ways for the same job violates the single-way rule): untyped `name`
        // and `name: default`. Both are an error that says what to write; "accepts any value"
        // is now said explicitly by writing `any`.
        // `fn` is a keyword (FUNCTION token) but is also valid as a type name.
        if (check(TokenType::COLON))
            throw LookParseError("struct '" + stmt->name + "': field '" + sf.name
                + "' uses the old 'name: default' form. Write '" + sf.name + " <type> = <default>' "
                  "(for example '" + sf.name + " any = ...').", field_tok.line, field_tok.column);
        if (!(check(TokenType::QUESTION) || check(TokenType::IDENT) || check(TokenType::FUNCTION)))
            throw LookParseError("struct '" + stmt->name + "': field '" + sf.name
                + "' needs a type. Write '" + sf.name + " <type>' — use 'any' to accept every value.",
                field_tok.line, field_tok.column);
        if (match(TokenType::QUESTION)) sf.type = "?";
        if (match(TokenType::FUNCTION)) sf.type += "fn";
        else sf.type += consume(TokenType::IDENT, "Expect a type name after '?'.").lexeme;
        if (match(TokenType::ASSIGN)) {
            sf.default_expr = expression();
        }
        stmt->fields.push_back(std::move(sf));
        match(TokenType::COMMA);  // optional comma separator
    }

    consume(TokenType::RBRACE, "Expect '}' after struct fields.");
    // Same name in the same file: fine if the definition is identical, an error if different (cross-file
    // cases are caught in the compiler — compiler.cpp compile_struct_decl).
    {
        std::string sig;
        for (size_t i = sig_from; i < current_; ++i)
            if (tokens_[i].type != TokenType::SEMICOLON && tokens_[i].type != TokenType::COMMA) {
                sig += tokens_[i].literal.value_or(tokens_[i].lexeme); sig += '\x1f';
            }
        // Error is built in: error::new() builds it, error::is recognizes only it (by the definition's identity).
        // A user struct with the same name would look the same but be a second meaning that is not an error.
        if (stmt->name == "Error")
            throw LookParseError("struct 'Error' is built in — error::new(type, message, code) makes one; "
                "give your struct another name", name.line, name.column);
        auto it = struct_seen_.find(stmt->name);
        if (it != struct_seen_.end() && it->second.first != sig)
            throw LookParseError("struct '" + stmt->name + "' is already declared at line "
                + std::to_string(it->second.second) + " with different fields", name.line, name.column);
        stmt->signature = sig;
        stmt->decl_line = name.line;
        struct_seen_.emplace(stmt->name, std::make_pair(std::move(sig), name.line));
    }
    return stmt;
}

// const { X = iota, Y, Z = 200 }
std::unique_ptr<Statement> Parser::const_block() {
    consume(TokenType::LBRACE, "Expect '{' after 'const'.");

    auto stmt = std::make_unique<ConstBlock>();

    while (!check(TokenType::RBRACE) && !is_at_end()) {
        while (match(TokenType::SEMICOLON)) {}  // skip empty ; left by ASI
        if (check(TokenType::RBRACE) || is_at_end()) break;
        auto name_tok = consume(TokenType::IDENT, "Expect constant name.");
        ConstItem item;
        item.name = name_tok.lexeme;
        if (match(TokenType::ASSIGN)) {
            item.value = expression();
        }
        stmt->items.push_back(std::move(item));
        match(TokenType::COMMA);  // optional comma separator
    }

    consume(TokenType::RBRACE, "Expect '}' after const block.");
    return stmt;
}

std::unique_ptr<Statement> Parser::function_declaration() {
    auto name_tok = consume(TokenType::IDENT, "Expect function name.");
    consume(TokenType::LPAREN, "Expect '(' after function name.");

    std::vector<std::string> params;
    std::vector<std::unique_ptr<Expression>> defaults;
    bool is_variadic = false;
    if (!check(TokenType::RPAREN)) {
        do {
            if (match(TokenType::ELLIPSIS)) {
                auto p = consume(TokenType::IDENT, "Expect parameter name after '...'.");
                params.push_back(p.lexeme);
                defaults.push_back(nullptr);
                is_variadic = true;
                break; // variadic must be the last parameter
            }
            auto p = consume(TokenType::IDENT, "Expect parameter name.");
            params.push_back(p.lexeme);
            // Default value: $param = <expr>
            if (match(TokenType::ASSIGN)) defaults.push_back(assignment());
            else                          defaults.push_back(nullptr);
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "Expect ')' after parameters.");
    consume(TokenType::LBRACE, "Expect '{' before function body.");
    scope_depth_++;
    auto body = block();
    scope_depth_--;
    auto fn = std::make_unique<FunctionDeclaration>(name_tok.lexeme, std::move(params), is_variadic, std::move(body));
    fn->defaults = std::move(defaults);
    // LOOK: a named function declared inside another function is also a closure — same rule.
    if (scope_depth_ > 0) fn->free_names = capture_free_names(fn->parameters, fn->body.get());
    return fn;
}

std::unique_ptr<Statement> Parser::return_statement() {
    std::unique_ptr<Expression> val = nullptr;
    if (!check(TokenType::SEMICOLON) && !check(TokenType::RBRACE) && !is_at_end())
        val = expression();
    consume_semi();
    return std::make_unique<ReturnStatement>(std::move(val));
}

// throw <expr>  — throws a user business-logic error; try/catch catches it.
//   throw "Unauthorized"        → catch($e) { $e == "Unauthorized" }
//   throw error::new("x","msg") → catch($e) { error::is($e, "x") }
std::unique_ptr<Statement> Parser::throw_statement() {
    if (check(TokenType::SEMICOLON) || check(TokenType::RBRACE) || is_at_end())
        throw look::LookParseError("an expression is required after throw",
                                   peek().line, peek().column);
    auto val = expression();
    consume_semi();
    return std::make_unique<ThrowStatement>(std::move(val));
}

std::unique_ptr<BlockStatement> Parser::block() {
    auto blk = std::make_unique<BlockStatement>();
    while (!check(TokenType::RBRACE) && !is_at_end()) {
        while (match(TokenType::SEMICOLON)) {}  // skip empty ; left by ASI
        if (check(TokenType::RBRACE) || is_at_end()) break;
        blk->statements.push_back(statement());
    }
    consume(TokenType::RBRACE, "Expect '}' after block.");
    return blk;
}

// ── Expressions ───────────────────────────────────────────────────────────────

std::unique_ptr<Expression> Parser::expression() {
    look_ast_guard(peek().line, peek().column);   // actual remaining stack (the counter alone is not enough)
    // Recursion guard: deeply nested expressions (((((...))))) or [[[[...]]]]
    // overflow the recursive-descent stack and cause SIGSEGV/worker crash. Code
    // generators (Looky AI), user-uploaded .lk and use "file.lk" all go through
    // this path. Each expression() level is ~14 C++ frames; on Windows' 1MB
    // stack a physical overflow happens at ~250 levels. 150 is more than
    // enough for real code (deepest nesting ~10-20) and cuts off before the
    // physical stack runs out on all platforms.
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& x) : d(x) { ++d; }
        ~DepthGuard() { --d; }
    } guard(expr_depth_);
    if (expr_depth_ > MAX_EXPR_DEPTH)
        throw look::LookParseError("Expression nested too deep (max " +
                                   std::to_string(MAX_EXPR_DEPTH) + ")",
                                   peek().line, peek().column);
    // At the outermost expression entry reset the binary-chain counter (each top-level expression
    // starts fresh; nested parens accumulate and cover the total left-spine depth).
    if (expr_depth_ == 1) binop_depth_ = 0;
    auto loc = cur_loc();
    auto e = assignment();
    if (e) e->loc = loc;
    return e;
}

void Parser::check_binop_chain() {
    if (++binop_depth_ > MAX_BINOP_CHAIN)
        throw look::LookParseError("Expression chain too long (max " +
                                   std::to_string(MAX_BINOP_CHAIN) +
                                   " consecutive binary operators)",
                                   peek().line, peek().column);
}

// `$a[k]++`, `++$a[k]`, `$s.n--`: ++/-- on an element or field. Converted to a compound assignment
// (`$a[k] += 1`) — the compiler already writes it as a path; keys are evaluated once.
// In postfix form the value of the expression is the old value (yield_old). Null if the target is not an element/field.
static std::unique_ptr<Expression> element_step(std::unique_ptr<Expression>& target, const char* op, bool postfix) {
    std::unique_ptr<AssignmentExpression> a;
    auto one = [] { return std::make_unique<NumberLiteral>(1); };
    if (auto* idx = node_as<IndexExpression>(target.get())) {
        auto index = std::move(const_cast<std::unique_ptr<Expression>&>(idx->index));
        if (auto* var = node_as<Variable>(idx->object.get()))
            a = std::make_unique<AssignmentExpression>(var->name, op, one(), std::move(index));
        else {
            auto obj = std::move(const_cast<std::unique_ptr<Expression>&>(idx->object));
            a = std::make_unique<AssignmentExpression>("", op, one(), std::move(index), std::move(obj));
        }
    } else if (auto* ma = node_as<MemberAccessExpression>(target.get())) {
        auto index = std::make_unique<StringLiteral>(ma->field);
        if (auto* var = node_as<Variable>(ma->object.get()))
            a = std::make_unique<AssignmentExpression>(var->name, op, one(), std::move(index));
        else {
            auto obj = std::move(const_cast<std::unique_ptr<Expression>&>(ma->object));
            a = std::make_unique<AssignmentExpression>("", op, one(), std::move(index), std::move(obj));
        }
    } else return nullptr;
    a->yield_old = postfix;
    a->loc = target->loc;
    return a;
}

std::unique_ptr<Expression> Parser::assignment() {
    auto expr = ternary();

    static const std::vector<TokenType> assign_ops = {
        TokenType::ASSIGN,
        TokenType::PLUS_ASSIGN, TokenType::MINUS_ASSIGN,
        TokenType::STAR_ASSIGN, TokenType::SLASH_ASSIGN,
        TokenType::PERCENT_ASSIGN, TokenType::DOT_ASSIGN,
        TokenType::AMP_ASSIGN, TokenType::PIPE_ASSIGN, TokenType::CARET_ASSIGN,
    };

    for (auto op_type : assign_ops) {
        if (match(op_type)) {
            std::string op = previous().lexeme;
            auto val = assignment();

            // $arr[i] = val   (i is a simple variable OR a chain: the base of $a[0][1], $m[k].x)
            if (auto* idx = node_as<IndexExpression>(expr.get())) {
                auto index = std::move(const_cast<std::unique_ptr<Expression>&>(idx->index));
                if (auto* var = node_as<Variable>(idx->object.get()))
                    return std::make_unique<AssignmentExpression>(var->name, op, std::move(val), std::move(index));
                // Chained: object is an expression ($arr[0], $l.s, ...) — in-place mutation
                auto obj = std::move(const_cast<std::unique_ptr<Expression>&>(idx->object));
                return std::make_unique<AssignmentExpression>("", op, std::move(val), std::move(index), std::move(obj));
            }
            // $obj.field = val  → $obj["field"] = val  (obj is a simple variable OR a chain)
            if (auto* ma = node_as<MemberAccessExpression>(expr.get())) {
                auto index = std::make_unique<StringLiteral>(ma->field);
                if (auto* var = node_as<Variable>(ma->object.get()))
                    return std::make_unique<AssignmentExpression>(var->name, op, std::move(val), std::move(index));
                // Chained: $l.s.x = 9, $arr[0].x = 7, $map["k"].x = 3
                auto obj = std::move(const_cast<std::unique_ptr<Expression>&>(ma->object));
                return std::make_unique<AssignmentExpression>("", op, std::move(val), std::move(index), std::move(obj));
            }
            // $var = val
            if (auto* var = node_as<Variable>(expr.get())) {
                if (op == "=" && is_in_place_array_call(var->name, val.get()) && in_place_array_.calls++ == 0) {
                    in_place_array_.line = var->loc.line > 0 ? var->loc.line : previous().line;
                    in_place_array_.column = var->loc.line > 0 ? var->loc.column : previous().column;
                }
                return std::make_unique<AssignmentExpression>(var->name, op, std::move(val));
            }

            throw look::LookParseError("Invalid assignment target.", previous().line, previous().column);
        }
    }
    return expr;
}

std::unique_ptr<Expression> Parser::ternary() {
    auto cond = null_coalescing();
    if (match(TokenType::QUESTION)) {
        auto then_e = ternary();
        consume(TokenType::COLON, "':' expected in the ternary operator");
        auto else_e = ternary();
        return std::make_unique<TernaryExpression>(std::move(cond), std::move(then_e), std::move(else_e));
    }
    return cond;
}

std::unique_ptr<Expression> Parser::null_coalescing() {
    auto expr = logical_or();
    while (match(TokenType::QUESTION_QUESTION)) {
        check_binop_chain();
        expr = std::make_unique<BinaryExpression>(std::move(expr), "??", logical_or());
    }
    return expr;
}

std::unique_ptr<Expression> Parser::logical_or() {
    auto expr = logical_and();
    while (match(TokenType::PIPE_PIPE)) {
        check_binop_chain();
        expr = std::make_unique<BinaryExpression>(std::move(expr), "||", logical_and());
    }
    return expr;
}

std::unique_ptr<Expression> Parser::logical_and() {
    auto expr = bitwise_or();
    while (match(TokenType::AMP_AMP)) {
        check_binop_chain();
        expr = std::make_unique<BinaryExpression>(std::move(expr), "&&", bitwise_or());
    }
    return expr;
}

std::unique_ptr<Expression> Parser::bitwise_or() {
    auto expr = bitwise_xor();
    while (match(TokenType::PIPE)) {
        check_binop_chain();
        expr = std::make_unique<BinaryExpression>(std::move(expr), "|", bitwise_xor());
    }
    return expr;
}

std::unique_ptr<Expression> Parser::bitwise_xor() {
    auto expr = bitwise_and();
    while (match(TokenType::CARET)) {
        check_binop_chain();
        expr = std::make_unique<BinaryExpression>(std::move(expr), "^", bitwise_and());
    }
    return expr;
}

std::unique_ptr<Expression> Parser::bitwise_and() {
    auto expr = equality();
    while (match(TokenType::AMP)) {
        check_binop_chain();
        expr = std::make_unique<BinaryExpression>(std::move(expr), "&", equality());
    }
    return expr;
}

std::unique_ptr<Expression> Parser::equality() {
    auto expr = comparison();
    while (match(TokenType::EQUAL_EQUAL) || match(TokenType::BANG_EQUAL)) {
        check_binop_chain();
        std::string op = previous().lexeme;
        expr = std::make_unique<BinaryExpression>(std::move(expr), op, comparison());
    }
    return expr;
}

std::unique_ptr<Expression> Parser::comparison() {
    auto expr = shift();
    while (match(TokenType::SPACESHIP) ||
           match(TokenType::GREATER) || match(TokenType::GREATER_EQUAL) ||
           match(TokenType::LESS)    || match(TokenType::LESS_EQUAL)) {
        check_binop_chain();
        std::string op = previous().lexeme;
        expr = std::make_unique<BinaryExpression>(std::move(expr), op, shift());
    }
    return expr;
}

std::unique_ptr<Expression> Parser::shift() {
    auto expr = concatenation();
    while (match(TokenType::LESS_LESS) || match(TokenType::GREATER_GREATER)) {
        check_binop_chain();
        std::string op = previous().lexeme;
        expr = std::make_unique<BinaryExpression>(std::move(expr), op, concatenation());
    }
    return expr;
}

std::unique_ptr<Expression> Parser::concatenation() {
    auto expr = addition();
    while (match(TokenType::DOT)) {
        check_binop_chain();
        expr = std::make_unique<BinaryExpression>(std::move(expr), ".", addition());
    }
    return expr;
}

std::unique_ptr<Expression> Parser::addition() {
    auto expr = multiplication();
    while (match(TokenType::PLUS) || match(TokenType::MINUS)) {
        check_binop_chain();
        std::string op = previous().lexeme;
        expr = std::make_unique<BinaryExpression>(std::move(expr), op, multiplication());
    }
    return expr;
}

std::unique_ptr<Expression> Parser::multiplication() {
    auto expr = power();
    while (match(TokenType::STAR) || match(TokenType::SLASH) || match(TokenType::PERCENT)) {
        check_binop_chain();
        std::string op = previous().lexeme;
        expr = std::make_unique<BinaryExpression>(std::move(expr), op, power());
    }
    return expr;
}

std::unique_ptr<Expression> Parser::power() {
    // DEPTH GUARD — power() is right-recursive for `**`; unary()'s guard
    // does not bound the power chain because it is released via RAII BEFORE power()
    // recurses. "2**2**…**2" therefore needs its own guard (otherwise stack overflow).
    struct DepthGuard { int& d; explicit DepthGuard(int& x):d(x){++d;} ~DepthGuard(){--d;} } guard(expr_depth_);
    if (expr_depth_ > MAX_EXPR_DEPTH)
        throw look::LookParseError("Expression nested too deep (max " +
                                   std::to_string(MAX_EXPR_DEPTH) + ")",
                                   peek().line, peek().column);
    auto expr = unary();
    if (match(TokenType::STAR_STAR))
        return std::make_unique<BinaryExpression>(std::move(expr), "**", power());
    return expr;
}

std::unique_ptr<Expression> Parser::unary() {
    // DEPTH GUARD — unary() is right-recursive (!/~/- call unary() on themselves),
    // and it SKIPPED the guard in expression() → "~"×N / "!"×N physically
    // overflowed the C++ stack (ASan stack-overflow, parse-time DoS). Same expr_depth_ counter.
    struct DepthGuard { int& d; explicit DepthGuard(int& x):d(x){++d;} ~DepthGuard(){--d;} } guard(expr_depth_);
    if (expr_depth_ > MAX_EXPR_DEPTH)
        throw look::LookParseError("Expression nested too deep (max " +
                                   std::to_string(MAX_EXPR_DEPTH) + ")",
                                   peek().line, peek().column);
    // Prefix ++/--: the target is a variable or an element/field of it (`++$a["n"]`, `--$s.count`),
    // so it is read together with the index/field chain (call()).
    if (match(TokenType::PLUS_PLUS)) {
        auto right = call();
        if (auto step = element_step(right, "+=", false)) return step;
        return std::make_unique<UnaryExpression>("++", std::move(right), true);
    }
    if (match(TokenType::MINUS_MINUS)) {
        auto right = call();
        if (auto step = element_step(right, "-=", false)) return step;
        return std::make_unique<UnaryExpression>("--", std::move(right), true);
    }
    if (match(TokenType::BANG))  { return std::make_unique<UnaryExpression>("!", std::move(unary()),  true); }
    if (match(TokenType::TILDE)) { return std::make_unique<UnaryExpression>("~", std::move(unary()),  true); }
    if (match(TokenType::MINUS)) {
        // -9223372036854775808 = INT64_MIN: 9223372036854775808 alone does not fit in int64, with the minus it does
        // (Java/Go recognize the same exception). The literal becomes INT64_MIN directly, no overflow error.
        if (check(TokenType::NUMBER)) {
            std::string lit = peek().literal.value_or("");
            lit.erase(std::remove(lit.begin(), lit.end(), '_'), lit.end());
            if (lit == "9223372036854775808") { advance(); return std::make_unique<NumberLiteral>(INT64_MIN); }
        }
        return std::make_unique<UnaryExpression>("-", std::move(unary()),  true);
    }
    return postfix();
}

std::unique_ptr<Expression> Parser::postfix() {
    auto expr = call();
    if (match(TokenType::PLUS_PLUS)) {
        if (auto step = element_step(expr, "+=", true)) return step;
        return std::make_unique<UnaryExpression>("++", std::move(expr), false);
    }
    if (match(TokenType::MINUS_MINUS)) {
        if (auto step = element_step(expr, "-=", true)) return step;
        return std::make_unique<UnaryExpression>("--", std::move(expr), false);
    }
    return expr;
}

std::unique_ptr<Expression> Parser::call() {
    auto expr = primary();
    while (true) {
        if (match(TokenType::LPAREN)) {
            expr = finish_call(std::move(expr));
        } else if (match(TokenType::LBRACKET)) {
            // $arr[index]
            auto index = expression();
            consume(TokenType::RBRACKET, "Expect ']' after index.");
            expr = std::make_unique<IndexExpression>(std::move(expr), std::move(index));
        } else if (check(TokenType::DOT) &&
                   current_ + 1 < tokens_.size() &&
                   tokens_[current_ + 1].type == TokenType::IDENT &&
                   !tokens_[current_ + 1].lexeme.empty() &&
                   tokens_[current_ + 1].lexeme[0] != '$' &&
                   // BUG FIX ('.' concat/member-access ambiguity): if the IDENT after '.'
                   // continues with '(' or '::' this is NOT MEMBER ACCESS — it is string concat +
                   // a function call ($out . helper(...)) or a module reference
                   // ($out . html::escape(...)). Previously these were swallowed as "$out.helper" / "$out.html"
                   // member access and gave a parse error (the idiomatic
                   // "$x = $x . fn(...)" string-building pattern was BROKEN). LOOK has no method
                   // call ($obj.method()) → this form is always concat, so excluding it is
                   // safe. Leave the concat to the top-level '.' operator (binary).
                   !(current_ + 2 < tokens_.size() &&
                     (tokens_[current_ + 2].type == TokenType::LPAREN ||
                      tokens_[current_ + 2].type == TokenType::COLON_COLON)) &&
                   // Only member access on expressions that can be containers
                   // (Variable, IndexExpression, CallExpression, MemberAccessExpression)
                   // NOT on literals — those use '.' for string concat
                   (node_as<Variable>(expr.get()) ||
                    node_as<IndexExpression>(expr.get()) ||
                    node_as<CallExpression>(expr.get()) ||
                    node_as<MemberAccessExpression>(expr.get()))) {
            // $obj.field — member access (dot followed by bare identifier, not $var)
            advance(); // consume '.'
            auto field = consume(TokenType::IDENT, "Expect field name after '.'.");
            expr = std::make_unique<MemberAccessExpression>(std::move(expr), field.lexeme);
        } else {
            break;
        }
    }
    return expr;
}

// Is it `$name = array::push($name, v)` or `$name = array::slice($name, 0, -1)`? (the compiler's
// in-place path: compiler.cpp try_compile_push_assign — that path skips the module check.)
static bool parser_int_literal(const Expression* x, int64_t v) {
    if (!x) return false;
    if (auto* n = node_as<NumberLiteral>(x)) return n->value == v;
    if (auto* u = node_as<UnaryExpression>(x); u && u->op == "-" && u->prefix && u->right)
        if (auto* n = node_as<NumberLiteral>(u->right.get())) return v != INT64_MIN && n->value == -v;
    return false;
}
bool Parser::is_in_place_array_call(const std::string& name, const Expression* val) {
    auto* call = node_as<CallExpression>(val);
    if (!call) return false;
    auto* sr = node_as<ScopeResolution>(call->callee.get());
    if (!sr || sr->module_name != "array") return false;
    const bool push = sr->member_name == "push" && call->arguments.size() == 2;
    const bool shrink = sr->member_name == "slice" && call->arguments.size() == 3 &&
                        parser_int_literal(call->arguments[1].get(), 0) &&
                        parser_int_literal(call->arguments[2].get(), -1);
    if (!push && !shrink) return false;
    auto* first = node_as<Variable>(call->arguments[0].get());
    return first && first->name == name;
}

// Warning text for a deprecated call (count > 1: the total in the file is also written).
std::string Parser::deprecation_message(int dep, int count) {
    static const char* const head[DEP_COUNT] = {
        "push($list, $x) changes the variable it is given",
        "pop($list) changes the variable it is given",
        "array::pop($list) does not remove anything, it returns the last element" };
    static const char* const repl[DEP_COUNT] = {
        "$list = array::push($list, $x)",
        "$x = $list[-1] and $list = array::slice($list, 0, -1)",
        "$list[-1]" };
    static const char* const name[DEP_COUNT] = { "push()", "pop()", "array::pop()" };
    std::string m = head[dep];
    if (count > 1) m += " (" + std::to_string(count) + " calls in this file, the first here)";
    m += std::string("; write ") + repl[dep] + " (" + name[dep] +
         " will be an error in a later release; lk fix rewrites it)";
    return m;
}

std::unique_ptr<Expression> Parser::finish_call(std::unique_ptr<Expression> callee) {
    // Bare push()/pop() and array::pop(): one release of warning, then an error.
    // The single spelling is Go's `a = append(a, x)` model: `$l = array::push($l, $x)`, last element `$l[-1]`,
    // removal `$l = array::slice($l, 0, -1)` — consistent with the value rule, no hidden mutation.
    // The name array::pop is misleading: it returns the last element WITHOUT removing it. Position is the call start (name token).
    int dep = -1;
    if (auto* v = node_as<Variable>(callee.get())) {
        if (v->name == "push") dep = DEP_PUSH;
        else if (v->name == "pop") dep = DEP_POP;
    } else if (auto* sr = node_as<ScopeResolution>(callee.get())) {
        if (sr->module_name == "array" && sr->member_name == "pop") dep = DEP_ARRAY_POP;
    }
    if (dep >= 0 && deps_[dep].calls++ == 0) {
        deps_[dep].warn = warnings_.size();
        CaptureWarning w;
        w.line = callee->loc.line > 0 ? callee->loc.line : previous().line;
        w.column = callee->loc.line > 0 ? callee->loc.column : previous().column;
        w.kind = dep == DEP_PUSH ? "push-deprecated" : dep == DEP_POP ? "pop-deprecated" : "array-pop-deprecated";
        w.message = deprecation_message(dep, 1);
        warnings_.push_back(std::move(w));
    }
    auto args = argument_list();
    consume(TokenType::RPAREN, "Expect ')' after arguments.");
    return std::make_unique<CallExpression>(std::move(callee), std::move(args));
}

std::unique_ptr<Expression> Parser::primary() {
    if (match(TokenType::IOTA))           return std::make_unique<IotaExpression>();
    if (match(TokenType::TRUE_KW))       return std::make_unique<BooleanLiteral>(true);
    if (match(TokenType::FALSE_KW))      return std::make_unique<BooleanLiteral>(false);
    if (match(TokenType::NULL_TOKEN)) return std::make_unique<NullLiteral>();
    if (match(TokenType::NUMBER)) {
        // Integer literal — drop underscore separators (1_000), resolve hex/binary base.
        // If it exceeds the int64 range (99999999999999999999) std::stoll throws
        // out_of_range → promote to float (only for decimals above ~9.2e18).
        std::string lit = previous().literal.value();
        lit.erase(std::remove(lit.begin(), lit.end(), '_'), lit.end());
        // Base prefix without digits ("0x", "0b"): it used to fall to the decimal branch and stoll("0x") = 0 —
        // a silently wrong value. Go: "hexadecimal literal has no digits".
        if (lit.size() == 2 && lit[0] == '0' &&
            (lit[1] == 'x' || lit[1] == 'X' || lit[1] == 'b' || lit[1] == 'B')) {
            throw LookParseError(std::string(lit[1] == 'x' || lit[1] == 'X' ? "hexadecimal" : "binary") +
                " literal " + lit + " has no digits", previous().line, previous().column);
        }
        try {
            int64_t v;
            if (lit.size() > 2 && lit[0] == '0' && (lit[1] == 'x' || lit[1] == 'X'))
                v = (int64_t)std::stoull(lit.substr(2), nullptr, 16);   // hex
            else if (lit.size() > 2 && lit[0] == '0' && (lit[1] == 'b' || lit[1] == 'B'))
                v = (int64_t)std::stoull(lit.substr(2), nullptr, 2);    // binary
            else
                v = (int64_t)std::stoll(lit);                           // decimal
            return std::make_unique<NumberLiteral>(v);
        } catch (...) {
            // An integer literal that does not fit in 64 bits is a COMPILE ERROR (Go: "constant overflows int64").
            // It used to fall to float with a warning: a large number such as an ID/phone silently
            // lost precision. Whoever wants a decimal value writes it explicitly (…0.0), an identifier is carried as text.
            throw LookParseError("integer literal " + lit + " does not fit in 64 bits (the largest is "
                "9223372036854775807): write it as a float (" + lit + ".0) if an approximate value is "
                "fine, or as a string for an ID", previous().line, previous().column);
        }
    }
    if (match(TokenType::FLOAT_NUM)) {
        std::string lit = previous().literal.value();
        lit.erase(std::remove(lit.begin(), lit.end(), '_'), lit.end());  // 1_000.5 → 1000.5
        // `strtod`: an overflowing literal such as `1e309` → ±Infinity (stod out_of_range
        // made it 0.0 → a silently wrong/dangerous value). IEEE-754 correct.
        return std::make_unique<FloatLiteral>(std::strtod(lit.c_str(), nullptr));
    }
    if (match(TokenType::STRING))     return std::make_unique<StringLiteral>(previous().literal.value(), previous().raw);

    // Anonymous function expression: function($a, $b) { ... }
    if (match(TokenType::FUNCTION)) {
        consume(TokenType::LPAREN, "Expect '(' after 'function'.");
        std::vector<std::string> params;
        std::vector<std::unique_ptr<Expression>> defaults;
        bool is_variadic = false;
        if (!check(TokenType::RPAREN)) {
            do {
                if (match(TokenType::ELLIPSIS)) {
                    auto p = consume(TokenType::IDENT, "Expect parameter name after '...'.");
                    params.push_back(p.lexeme);
                    defaults.push_back(nullptr);
                    is_variadic = true;
                    break;
                }
                auto p = consume(TokenType::IDENT, "Expect parameter name.");
                params.push_back(p.lexeme);
                if (match(TokenType::ASSIGN)) defaults.push_back(assignment());
                else                          defaults.push_back(nullptr);
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "Expect ')' after parameters.");

        // use ($conn, $db) — explicit capture list
        std::vector<std::string> captures;
        if (match(TokenType::USE)) {
            consume(TokenType::LPAREN, "Expect '(' after 'use'.");
            if (!check(TokenType::RPAREN)) {
                do {
                    auto cap = consume(TokenType::IDENT, "Expect variable name in use(...).");
                    captures.push_back(cap.lexeme);
                } while (match(TokenType::COMMA));
            }
            consume(TokenType::RPAREN, "Expect ')' after use(...).");
        }

        // Body: either a block `{ ... }` or an expression-bodied arrow `=> expr`
        //   fn($x) => $x * 2   ≡   function($x) { return $x * 2 }
        std::unique_ptr<BlockStatement> body;
        if (match(TokenType::FAT_ARROW)) {
            auto blk = std::make_unique<BlockStatement>();
            blk->statements.push_back(std::make_unique<ReturnStatement>(expression()));
            body = std::move(blk);
        } else {
            consume(TokenType::LBRACE, "Expect '{' or '=>' before function body.");
            scope_depth_++;
            body = block();
            scope_depth_--;
        }
        auto fn = std::make_unique<FunctionExpression>();
        fn->parameters  = std::move(params);
        fn->defaults    = std::move(defaults);
        fn->captures    = std::move(captures);
        fn->is_variadic = is_variadic;
        fn->body = std::move(body);
        fn->free_names = capture_free_names(*fn);   // those of inner closures are already filled (inside out)
        return fn;
    }

    // Array literal [1, 2, 3] or assoc ["key" => val]
    if (match(TokenType::LBRACKET)) {
        if (check(TokenType::RBRACKET)) {
            advance(); // empty array
            return std::make_unique<ArrayLiteral>();
        }
        // Peek ahead to check if first element has => (assoc array)
        auto first = expression();
        if (match(TokenType::FAT_ARROW)) {
            // Associative array
            auto assoc = std::make_unique<AssocArrayLiteral>();
            auto val = expression();
            assoc->pairs.push_back({std::move(first), std::move(val)});
            while (match(TokenType::COMMA)) {
                if (check(TokenType::RBRACKET)) break;
                auto k = expression();
                consume(TokenType::FAT_ARROW, "Expect '=>' in associative array.");
                auto v = expression();
                assoc->pairs.push_back({std::move(k), std::move(v)});
            }
            consume(TokenType::RBRACKET, "Expect ']' after array.");
            return assoc;
        }
        // Regular array
        auto arr = std::make_unique<ArrayLiteral>();
        arr->elements.push_back(std::move(first));
        while (match(TokenType::COMMA)) {
            if (check(TokenType::RBRACKET)) break;
            arr->elements.push_back(expression());
        }
        consume(TokenType::RBRACKET, "Expect ']' after array elements.");
        return arr;
    }

    if (match(TokenType::IDENT)) {
        Token id_tok = previous();
        std::string name = id_tok.lexeme;
        // Struct literal: BareName{field: val, ...}  — bare ident (no $) followed by {
        if (name[0] != '$' && check(TokenType::LBRACE)) {
            advance(); // consume '{'
            auto lit = std::make_unique<StructLiteralExpression>();
            lit->struct_name = name;
            while (!check(TokenType::RBRACE) && !is_at_end()) {
                auto field_tok = consume(TokenType::IDENT, "Expect field name in struct literal.");
                consume(TokenType::COLON, "Expect ':' after field name in struct literal.");
                auto val = expression();
                lit->fields.push_back({field_tok.lexeme, std::move(val)});
                match(TokenType::COMMA);
            }
            consume(TokenType::RBRACE, "Expect '}' after struct literal.");
            return lit;
        }
        if (match(TokenType::COLON_COLON)) {
            auto member = consume(TokenType::IDENT, "Expect member name after '::'.");
            auto sr = std::make_unique<ScopeResolution>(name, member.lexeme);
            sr->loc.line = id_tok.line; sr->loc.column = id_tok.column;   // warning position (array::pop)
            return sr;
        }
        auto var = std::make_unique<Variable>(name);
        var->loc.line = id_tok.line; var->loc.column = id_tok.column;
        return var;
    }

    if (match(TokenType::LPAREN)) {
        auto expr = expression();
        consume(TokenType::RPAREN, "Expect ')' after expression.");
        return expr;
    }

    throw look::LookParseError("Unexpected token: '" + peek().lexeme + "'",
                              peek().line, peek().column);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

std::vector<std::unique_ptr<Expression>> Parser::argument_list() {
    std::vector<std::unique_ptr<Expression>> args;
    if (!check(TokenType::RPAREN)) {
        do { args.push_back(expression()); } while (match(TokenType::COMMA));
    }
    return args;
}

bool          Parser::match(TokenType t)       { if (check(t)) { advance(); return true; } return false; }

// consume ';' — if ASI already inserted it, consume it; otherwise pass silently at } or EOF.
// Not used where an explicit ';' is required, including inside for(;;).
void          Parser::consume_semi() {
    if (check(TokenType::SEMICOLON)) { advance(); return; }
    // Implicit terminator: } or EOF — ASI did its job or we are at the end of an inline block
    if (check(TokenType::RBRACE) || is_at_end()) return;
    throw look::LookParseError("Expect ';'. (got '" + peek().lexeme + "')",
                               peek().line, peek().column);
}
const Token&  Parser::consume(TokenType t, const std::string& msg) {
    if (check(t)) return advance();
    throw look::LookParseError(msg + " (got '" + peek().lexeme + "')",
                              peek().line, peek().column);
}
bool          Parser::check(TokenType t)  const { return !is_at_end() && peek().type == t; }
bool          Parser::is_at_end()         const { return peek().type == TokenType::EOF_TOKEN; }
const Token&  Parser::advance()                 { if (!is_at_end()) current_++; return previous(); }
const Token&  Parser::peek()              const { return tokens_[current_]; }
const Token&  Parser::previous()          const { return tokens_[current_ - 1]; }

} // namespace look
