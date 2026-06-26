#pragma once

#include <optional>
#include <string>

namespace look {

enum class TokenType {
    // Literals
    EOF_TOKEN,
    IDENT,
    STRING,
    NUMBER,
    FLOAT_NUM,

    // Delimiters
    SEMICOLON,
    LPAREN,
    RPAREN,
    LBRACE,
    RBRACE,
    LBRACKET,
    RBRACKET,
    COMMA,
    FAT_ARROW,      // =>

    // Scope resolution
    COLON_COLON,    // ::

    // Assignment
    ASSIGN,
    PLUS_ASSIGN,
    MINUS_ASSIGN,
    STAR_ASSIGN,
    SLASH_ASSIGN,
    PERCENT_ASSIGN,
    DOT_ASSIGN,
    AMP_ASSIGN,
    PIPE_ASSIGN,
    CARET_ASSIGN,

    // Arithmetic
    PLUS,
    MINUS,
    STAR,
    SLASH,
    PERCENT,
    STAR_STAR,

    // String
    DOT,

    // Increment / Decrement
    PLUS_PLUS,
    MINUS_MINUS,

    // Bitwise
    AMP,
    PIPE,
    CARET,
    TILDE,
    LESS_LESS,
    GREATER_GREATER,

    // Logical
    BANG,
    AMP_AMP,
    PIPE_PIPE,

    // Comparison
    EQUAL_EQUAL,
    BANG_EQUAL,
    GREATER,
    GREATER_EQUAL,
    LESS,
    LESS_EQUAL,
    SPACESHIP,

    // Null coalescing
    QUESTION_QUESTION,  // ??

    // Ternary
    QUESTION,           // ?
    COLON,              // :

    // Variadic
    ELLIPSIS,           // ...

    // Keywords
    RETURN,
    FUNCTION,
    PRINT,
    WRITE,
    IF,
    ELSEIF,
    ELSE,
    WHILE,
    FOR,
    FOREACH,
    AS,
    BREAK,
    CONTINUE,
    TRUE_KW,   // "true" — TRUE/FALSE windows.h macro'larıyla çakışır
    FALSE_KW,  // "false"
    NULL_TOKEN,
    USE,
    TRY,
    CATCH,
    FINALLY,
    GLOBAL,
    SWITCH,
    CASE,
    DEFAULT,

    // Phase 11
    STRUCT,
    CONST_KW,   // "const" — CONST is a Windows macro in windef.h
    IOTA,
};

struct Token {
    TokenType type;
    std::string lexeme;
    std::optional<std::string> literal;
    int line;
    int column = 1;
};

} // namespace look
