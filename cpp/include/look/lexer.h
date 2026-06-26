#pragma once

#include "look/token.h"
#include <string>
#include <vector>

namespace look {

class Lexer {
public:
    explicit Lexer(std::string source);
    std::vector<Token> scan_tokens();

private:
    std::string source_;
    std::vector<Token> tokens_;
    size_t start_ = 0;
    size_t current_ = 0;
    int line_ = 1;
    int col_ = 1;        // current column (increments per char)
    int start_col_ = 1;  // column at token start

    // ASI (Automatic Semicolon Insertion) state
    TokenType last_token_type_ = TokenType::EOF_TOKEN;
    int paren_depth_   = 0;  // ( )
    int bracket_depth_ = 0;  // [ ]
    int brace_depth_   = 0;  // { }
    // ( açıldığında o anki brace_depth kaydedilir. ASI: brace_depth > stack.back() ise
    // ( içindeyiz ama bir { bloğunun içindeyiz → closure body → ASI tetikle.
    std::vector<int> paren_brace_stack_;  // her ( için brace_depth snapshot

    bool is_at_end() const;
    char advance();
    char peek() const;
    char peek_next() const;
    void scan_token();
    void add_token(TokenType type, std::optional<std::string> literal = std::nullopt);
    void string(char quote);
    void raw_string();
    void number();
    void identifier();

    static bool should_insert_semicolon(TokenType t);
    bool is_continuation_ahead() const;  // sonraki satır devam operatörüyle başlıyorsa ASI bastır
};

} // namespace look
