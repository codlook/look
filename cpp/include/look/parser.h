#pragma once

#include "look/ast.h"
#include "look/token.h"
#include <memory>
#include <vector>

namespace look {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    std::unique_ptr<Program> parse();

private:
    std::vector<Token> tokens_;
    size_t current_ = 0;
    int    scope_depth_ = 0;  // fonksiyon iç içeliği — use "file" top-level guard için
    static constexpr int MAX_EXPR_DEPTH = 150;  // ifade özyineleme tavanı (expression+unary+power ortak)
    int    expr_depth_  = 0;  // ifade özyineleme derinliği — derin iç içe paren/array
                              // parser'ın recursive-descent yığınını taşırıp SIGSEGV
                              // yapmasını engeller (JSON parser'daki koruma gibi).
    int    stmt_depth_  = 0;  // statement/blok iç içeliği — derin iç içe {}/if/while
                              // aynı stack-taşma riskini taşır; ayrı sayaçla korunur.
    // Sol-yaslı ikili zincir ('1+1+1+...+1' N terim) parse'ta İTERATİF kurulur
    // (while döngüsü) — expr_depth_ ARTMAZ, yani paren/array guard'ı bunu YAKALAMAZ.
    // Ama üretilen AST N derinliğinde sola-nested olur; interpreter evaluate() ve
    // VM compiler bu iskeleti ÖZYİNELİ gezerek ~7000 terimde stack-overflow (SIGABRT)
    // yapar → saldırgan-kontrollü derin girdi = DoS. Tek terim ~1 zincir demek; gerçek
    // kodda bin+ ardışık ikili operatör görülmez. Sınır aşılınca AST HİÇ kurulmadan
    // temiz parse hatası atılır → ne interpreter ne VM derin ağacı görür.
    static constexpr int MAX_BINOP_CHAIN = 1000;  // tek ifadedeki ikili-operatör tavanı
    int    binop_depth_ = 0;  // kurulan ikili-operatör düğüm sayacı (expression() başında sıfırlanır)
    void   check_binop_chain();  // her ikili düğümden önce çağrılır; aşımda temiz hata

    // Statements
    std::unique_ptr<Statement>      statement();
    std::unique_ptr<BlockStatement> block();
    std::unique_ptr<Statement>      print_statement();
    std::unique_ptr<Statement>      write_statement();
    std::unique_ptr<Statement>      if_statement();
    std::unique_ptr<Statement>      while_statement();
    std::unique_ptr<Statement>      for_statement();
    std::unique_ptr<Statement>      foreach_statement();
    std::unique_ptr<Statement>      function_declaration();
    std::unique_ptr<Statement>      return_statement();
    std::unique_ptr<Statement>      throw_statement();
    std::unique_ptr<Statement>      use_statement();
    std::unique_ptr<Statement>      try_statement();
    std::unique_ptr<Statement>      switch_statement();
    std::unique_ptr<Statement>      struct_declaration();
    std::unique_ptr<Statement>      const_block();
    std::unique_ptr<Statement>      expression_statement();

    // Expressions
    std::unique_ptr<Expression> expression();
    std::unique_ptr<Expression> assignment();
    std::unique_ptr<Expression> ternary();
    std::unique_ptr<Expression> null_coalescing();
    std::unique_ptr<Expression> logical_or();
    std::unique_ptr<Expression> logical_and();
    std::unique_ptr<Expression> bitwise_or();
    std::unique_ptr<Expression> bitwise_xor();
    std::unique_ptr<Expression> bitwise_and();
    std::unique_ptr<Expression> equality();
    std::unique_ptr<Expression> comparison();
    std::unique_ptr<Expression> shift();
    std::unique_ptr<Expression> concatenation();
    std::unique_ptr<Expression> addition();
    std::unique_ptr<Expression> multiplication();
    std::unique_ptr<Expression> power();
    std::unique_ptr<Expression> unary();
    std::unique_ptr<Expression> postfix();
    std::unique_ptr<Expression> call();
    std::unique_ptr<Expression> finish_call(std::unique_ptr<Expression> callee);
    std::unique_ptr<Expression> primary();

    // Helpers
    std::vector<std::unique_ptr<Expression>> argument_list();

    bool          match(TokenType type);
    const Token&  consume(TokenType type, const std::string& message);
    void          consume_semi();  // ';' tüket — yoksa } veya EOF'ta sessiz geç (ASI opsiyonel)
    bool          check(TokenType type) const;
    bool          is_at_end() const;
    const Token&  advance();
    const Token&  peek() const;
    const Token&  previous() const;

    SourceLocation cur_loc() const {
        if (current_ < tokens_.size())
            return {"", tokens_[current_].line, tokens_[current_].column};
        return {};
    }
    SourceLocation prev_loc() const {
        if (current_ > 0)
            return {"", tokens_[current_-1].line, tokens_[current_-1].column};
        return {};
    }
};

} // namespace look
