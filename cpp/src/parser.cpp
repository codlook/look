#include "look/parser.h"
#include "look/interpreter.h"
#include <stdexcept>
#include <algorithm>
#include <cstdlib>

namespace look {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

std::unique_ptr<Program> Parser::parse() {
    auto program = std::make_unique<Program>();
    while (!is_at_end()) {
        while (match(TokenType::SEMICOLON)) {}  // ASI'den gelen boş ; atla
        if (is_at_end()) break;
        program->statements.push_back(statement());
    }
    return program;
}

// ── Statements ────────────────────────────────────────────────────────────────

std::unique_ptr<Statement> Parser::statement() {
    // Recursion guard: derin iç içe blok/if/while ({{{...}}}) parser yığınını
    // taşırır — ifade guard'ıyla aynı gerekçe. RAII ile sayaç azaltılır.
    static constexpr int MAX_STMT_DEPTH = 150;
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& x) : d(x) { ++d; }
        ~DepthGuard() { --d; }
    } guard(stmt_depth_);
    if (stmt_depth_ > MAX_STMT_DEPTH)
        throw look::LookParseError("İfade/blok çok derin iç içe (max " +
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
// Go stili: break yok, otomatik çıkış
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
            // default: — values listesi boş kalır
            consume(TokenType::COLON, "Expect ':' after 'default'.");
        } else {
            consume(TokenType::CASE, "Expect 'case' or 'default' in switch.");
            // case 1, 2, 3:
            sc.values.push_back(expression());
            while (match(TokenType::COMMA))
                sc.values.push_back(expression());
            consume(TokenType::COLON, "Expect ':' after case value.");
        }

        // case body — sonraki case/default/} gelene kadar
        while (!check(TokenType::CASE) && !check(TokenType::DEFAULT) &&
               !check(TokenType::RBRACE) && !is_at_end()) {
            sc.body.push_back(statement());
        }

        stmt->cases.push_back(std::move(sc));
    }

    consume(TokenType::RBRACE, "Expect '}' after switch body.");
    return stmt;
}

// print(expr, ...)  — parens zorunlu
std::unique_ptr<Statement> Parser::try_statement() {
    auto stmt = std::make_unique<TryCatchStatement>();

    // try { }
    consume(TokenType::LBRACE, "Expect '{' after 'try'.");
    stmt->try_block = block();

    // ASI: } sonrası newline catch/finally'yi engellemez
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

    // ASI: } sonrası newline finally'yi engellemez
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

// write(expr, ...)  — parens zorunlu, newline yok
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
    // use "routes/auth.lk"; — dosya modül sistemi
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
    // use module; veya use module as alias;
    auto name = consume(TokenType::IDENT, "Expect module name after 'use'.");
    std::string alias;
    if (match(TokenType::AS)) {
        auto al = consume(TokenType::IDENT, "Expect alias name after 'as'.");
        alias = al.lexeme;
    }
    consume_semi();
    return std::make_unique<UseStatement>(name.lexeme, alias);
}

std::unique_ptr<Statement> Parser::if_statement() {
    consume(TokenType::LPAREN, "Expect '(' after 'if'.");
    auto cond = expression();
    consume(TokenType::RPAREN, "Expect ')' after if condition.");
    consume(TokenType::LBRACE, "Expect '{' after if condition.");
    auto then_b = block();

    std::unique_ptr<BlockStatement> else_b = nullptr;

    // ASI'den gelen ; atla — } sonrası newline elseif/else'yi engellemez
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
    consume(TokenType::LBRACE, "Expect '{' after struct name.");

    auto stmt = std::make_unique<StructDeclaration>();
    stmt->name = name.lexeme;

    while (!check(TokenType::RBRACE) && !is_at_end()) {
        while (match(TokenType::SEMICOLON)) {}  // ASI'den gelen boş ; atla
        if (check(TokenType::RBRACE) || is_at_end()) break;
        auto field_tok = consume(TokenType::IDENT, "Expect field name.");
        StructField sf;
        sf.name = field_tok.lexeme;
        if (match(TokenType::COLON)) {
            sf.default_expr = expression();
        }
        stmt->fields.push_back(std::move(sf));
        match(TokenType::COMMA);  // optional comma separator
    }

    consume(TokenType::RBRACE, "Expect '}' after struct fields.");
    return stmt;
}

// const { X = iota, Y, Z = 200 }
std::unique_ptr<Statement> Parser::const_block() {
    consume(TokenType::LBRACE, "Expect '{' after 'const'.");

    auto stmt = std::make_unique<ConstBlock>();

    while (!check(TokenType::RBRACE) && !is_at_end()) {
        while (match(TokenType::SEMICOLON)) {}  // ASI'den gelen boş ; atla
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
                break; // variadic son parametre olmak zorunda
            }
            auto p = consume(TokenType::IDENT, "Expect parameter name.");
            params.push_back(p.lexeme);
            // Varsayılan değer: $param = <ifade>
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
    return fn;
}

std::unique_ptr<Statement> Parser::return_statement() {
    std::unique_ptr<Expression> val = nullptr;
    if (!check(TokenType::SEMICOLON) && !check(TokenType::RBRACE) && !is_at_end())
        val = expression();
    consume_semi();
    return std::make_unique<ReturnStatement>(std::move(val));
}

// throw <ifade>  — kullanıcı iş-mantığı hatası fırlatır; try/catch yakalar.
//   throw "Yetkisiz"            → catch($e) { $e == "Yetkisiz" }
//   throw error::new("x","msg") → catch($e) { error::is($e, "x") }
std::unique_ptr<Statement> Parser::throw_statement() {
    if (check(TokenType::SEMICOLON) || check(TokenType::RBRACE) || is_at_end())
        throw look::LookParseError("throw sonrası bir ifade gerekli",
                                   peek().line, peek().column);
    auto val = expression();
    consume_semi();
    return std::make_unique<ThrowStatement>(std::move(val));
}

std::unique_ptr<BlockStatement> Parser::block() {
    auto blk = std::make_unique<BlockStatement>();
    while (!check(TokenType::RBRACE) && !is_at_end()) {
        while (match(TokenType::SEMICOLON)) {}  // ASI'den gelen boş ; atla
        if (check(TokenType::RBRACE) || is_at_end()) break;
        blk->statements.push_back(statement());
    }
    consume(TokenType::RBRACE, "Expect '}' after block.");
    return blk;
}

// ── Expressions ───────────────────────────────────────────────────────────────

std::unique_ptr<Expression> Parser::expression() {
    // Recursion guard: derin iç içe ifade (((((...))))) veya [[[[...]]]]
    // recursive-descent yığınını taşırıp SIGSEGV/worker crash yapar. Kod
    // üretici (Looky AI), kullanıcı-yüklenen .lk veya use "dosya.lk" bu yoldan
    // geçer. Her expression() seviyesi ~14 C++ frame demek; Windows'un 1MB
    // stack'inde ~250 seviyede fiziksel taşma olur. 150 gerçek kod için
    // fazlasıyla yeterli (en derin iç içe ifade ~10-20) ve tüm platformlarda
    // fiziksel stack tükenmeden önce keser.
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& x) : d(x) { ++d; }
        ~DepthGuard() { --d; }
    } guard(expr_depth_);
    if (expr_depth_ > MAX_EXPR_DEPTH)
        throw look::LookParseError("İfade çok derin iç içe (max " +
                                   std::to_string(MAX_EXPR_DEPTH) + ")",
                                   peek().line, peek().column);
    // En dıştaki ifade girişinde ikili-zincir sayacını sıfırla (her top-level ifade
    // taze başlar; iç içe paren'lar accumulate ederek toplam sol-spine derinliğini kapsar).
    if (expr_depth_ == 1) binop_depth_ = 0;
    auto loc = cur_loc();
    auto e = assignment();
    if (e) e->loc = loc;
    return e;
}

void Parser::check_binop_chain() {
    if (++binop_depth_ > MAX_BINOP_CHAIN)
        throw look::LookParseError("İfade zinciri çok uzun (max " +
                                   std::to_string(MAX_BINOP_CHAIN) +
                                   " ardışık ikili operatör)",
                                   peek().line, peek().column);
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

            // $arr[i] = val   (i basit değişken VEYA zincir: $a[0][1], $m[k].x'in tabanı)
            if (auto* idx = dynamic_cast<IndexExpression*>(expr.get())) {
                auto index = std::move(const_cast<std::unique_ptr<Expression>&>(idx->index));
                if (auto* var = dynamic_cast<Variable*>(idx->object.get()))
                    return std::make_unique<AssignmentExpression>(var->name, op, std::move(val), std::move(index));
                // Zincirli: object bir ifade ($arr[0], $l.s, ...) — in-place mutasyon
                auto obj = std::move(const_cast<std::unique_ptr<Expression>&>(idx->object));
                return std::make_unique<AssignmentExpression>("", op, std::move(val), std::move(index), std::move(obj));
            }
            // $obj.field = val  → $obj["field"] = val  (obj basit değişken VEYA zincir)
            if (auto* ma = dynamic_cast<MemberAccessExpression*>(expr.get())) {
                auto index = std::make_unique<StringLiteral>(ma->field);
                if (auto* var = dynamic_cast<Variable*>(ma->object.get()))
                    return std::make_unique<AssignmentExpression>(var->name, op, std::move(val), std::move(index));
                // Zincirli: $l.s.x = 9, $arr[0].x = 7, $map["k"].x = 3
                auto obj = std::move(const_cast<std::unique_ptr<Expression>&>(ma->object));
                return std::make_unique<AssignmentExpression>("", op, std::move(val), std::move(index), std::move(obj));
            }
            // $var = val
            if (auto* var = dynamic_cast<Variable*>(expr.get()))
                return std::make_unique<AssignmentExpression>(var->name, op, std::move(val));

            throw look::LookParseError("Invalid assignment target.", previous().line, previous().column);
        }
    }
    return expr;
}

std::unique_ptr<Expression> Parser::ternary() {
    auto cond = null_coalescing();
    if (match(TokenType::QUESTION)) {
        auto then_e = ternary();
        consume(TokenType::COLON, "Ternary operatöründe ':' bekleniyor");
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
    // DERİNLİK GUARD'I — power() `**` için sağ-özyinelemeli; unary()'nin guard'ı
    // power() recurse'den ÖNCE RAII ile çözüldüğünden power zincirini sınırlamaz.
    // "2**2**…**2" bu yüzden ayrı guard ister (aksi halde stack-overflow).
    struct DepthGuard { int& d; explicit DepthGuard(int& x):d(x){++d;} ~DepthGuard(){--d;} } guard(expr_depth_);
    if (expr_depth_ > MAX_EXPR_DEPTH)
        throw look::LookParseError("İfade çok derin iç içe (max " +
                                   std::to_string(MAX_EXPR_DEPTH) + ")",
                                   peek().line, peek().column);
    auto expr = unary();
    if (match(TokenType::STAR_STAR))
        return std::make_unique<BinaryExpression>(std::move(expr), "**", power());
    return expr;
}

std::unique_ptr<Expression> Parser::unary() {
    // DERİNLİK GUARD'I — unary() sağ-özyinelemeli (!/~/- kendine unary() çağırır),
    // expression()'daki guard'ı ATLIYORDU → "~"×N / "!"×N C++ stack'ini fiziksel
    // taşırıyordu (ASan stack-overflow, parse-zamanı DoS). Aynı expr_depth_ sayacı.
    struct DepthGuard { int& d; explicit DepthGuard(int& x):d(x){++d;} ~DepthGuard(){--d;} } guard(expr_depth_);
    if (expr_depth_ > MAX_EXPR_DEPTH)
        throw look::LookParseError("İfade çok derin iç içe (max " +
                                   std::to_string(MAX_EXPR_DEPTH) + ")",
                                   peek().line, peek().column);
    if (match(TokenType::PLUS_PLUS)) {
        auto right = primary();
        return std::make_unique<UnaryExpression>("++", std::move(right), true);
    }
    if (match(TokenType::MINUS_MINUS)) {
        auto right = primary();
        return std::make_unique<UnaryExpression>("--", std::move(right), true);
    }
    if (match(TokenType::BANG))  { return std::make_unique<UnaryExpression>("!", std::move(unary()),  true); }
    if (match(TokenType::TILDE)) { return std::make_unique<UnaryExpression>("~", std::move(unary()),  true); }
    if (match(TokenType::MINUS)) { return std::make_unique<UnaryExpression>("-", std::move(unary()),  true); }
    return postfix();
}

std::unique_ptr<Expression> Parser::postfix() {
    auto expr = call();
    if (match(TokenType::PLUS_PLUS))   return std::make_unique<UnaryExpression>("++", std::move(expr), false);
    if (match(TokenType::MINUS_MINUS)) return std::make_unique<UnaryExpression>("--", std::move(expr), false);
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
                   // BUG FIX ('.' concat/üye-erişim ikililiği): '.' sonrası gelen IDENT
                   // '(' veya '::' ile devam ediyorsa bu ÜYE ERİŞİMİ DEĞİL — string concat +
                   // fonksiyon çağrısı ($out . helper(...)) ya da modül referansı
                   // ($out . html::escape(...)). Eskiden bunlar "$out.helper" / "$out.html"
                   // üye erişimi olarak yutulup parse hatası veriyordu (idiomatik
                   // "$x = $x . fn(...)" string-building kalıbı BOZUKTU). LOOK'ta metot
                   // çağrısı ($obj.method()) yok → bu form her zaman concat'tır, dışlamak
                   // güvenli. Concat'ı üst seviyedeki '.' operatörüne (binary) bırak.
                   !(current_ + 2 < tokens_.size() &&
                     (tokens_[current_ + 2].type == TokenType::LPAREN ||
                      tokens_[current_ + 2].type == TokenType::COLON_COLON)) &&
                   // Only member access on expressions that can be containers
                   // (Variable, IndexExpression, CallExpression, MemberAccessExpression)
                   // NOT on literals — those use '.' for string concat
                   (dynamic_cast<Variable*>(expr.get()) ||
                    dynamic_cast<IndexExpression*>(expr.get()) ||
                    dynamic_cast<CallExpression*>(expr.get()) ||
                    dynamic_cast<MemberAccessExpression*>(expr.get()))) {
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

std::unique_ptr<Expression> Parser::finish_call(std::unique_ptr<Expression> callee) {
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
        // Integer literal — underscore ayırıcıları (1_000) at, hex/binary tabanı çöz.
        // int64 aralığını aşarsa (99999999999999999999) std::stoll out_of_range
        // fırlatır → float'a promote (yalnızca ~9.2e18 üstü decimal için).
        std::string lit = previous().literal.value();
        lit.erase(std::remove(lit.begin(), lit.end(), '_'), lit.end());
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
            // int64'ü aşan tamsayı → float. `strtod`: taşma→±inf, underflow→0,
            // işaret korunur (stod out_of_range'i 0'a çevirip devasa değeri sessizce
            // sıfırlıyordu → limit/eşik bypass'ı).
            return std::make_unique<FloatLiteral>(std::strtod(lit.c_str(), nullptr));
        }
    }
    if (match(TokenType::FLOAT_NUM)) {
        std::string lit = previous().literal.value();
        lit.erase(std::remove(lit.begin(), lit.end(), '_'), lit.end());  // 1_000.5 → 1000.5
        // `strtod`: `1e309` gibi taşan literal → ±Infinity (stod out_of_range'i
        // 0.0 yapıyordu → sessizce yanlış/tehlikeli değer). IEEE-754 doğru.
        return std::make_unique<FloatLiteral>(std::strtod(lit.c_str(), nullptr));
    }
    if (match(TokenType::STRING))     return std::make_unique<StringLiteral>(previous().literal.value());

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

        // use ($conn, $db) — explicit capture listesi
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

        // Gövde: ya blok `{ ... }` ya da ifade-gövdeli ok `=> ifade`
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
            return std::make_unique<ScopeResolution>(name, member.lexeme);
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

// ';' tüket — ASI zaten eklediyse consume eder, yoksa } veya EOF'ta sessiz geçer.
// for(;;) içi dahil olmak üzere explicit ';' gerektiren yerlerde kullanılmaz.
void          Parser::consume_semi() {
    if (check(TokenType::SEMICOLON)) { advance(); return; }
    // Implicit terminator: } veya EOF — ASI iş yapmışsa veya inline blok sonundayız
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
