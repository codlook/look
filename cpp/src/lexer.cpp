#include "look/lexer.h"
#include <cctype>
#include <unordered_map>
#include <stdexcept>

namespace look {

static const std::unordered_map<std::string, TokenType> keywords = {
    {"if",       TokenType::IF},
    {"else",     TokenType::ELSE},
    {"while",    TokenType::WHILE},
    {"for",      TokenType::FOR},
    {"foreach",  TokenType::FOREACH},
    {"as",       TokenType::AS},
    {"break",    TokenType::BREAK},
    {"continue", TokenType::CONTINUE},
    {"function", TokenType::FUNCTION},
    {"return",   TokenType::RETURN},
    {"print",    TokenType::PRINT},
    {"write",    TokenType::WRITE},
    {"elseif",   TokenType::ELSEIF},
    {"true",     TokenType::TRUE_KW},
    {"false",    TokenType::FALSE_KW},
    {"null",     TokenType::NULL_TOKEN},
    {"use",      TokenType::USE},
    {"global",   TokenType::GLOBAL},
    {"try",      TokenType::TRY},
    {"catch",    TokenType::CATCH},
    {"finally",  TokenType::FINALLY},
    {"switch",   TokenType::SWITCH},
    {"case",     TokenType::CASE},
    {"default",  TokenType::DEFAULT},
    {"struct",   TokenType::STRUCT},
    {"const",    TokenType::CONST_KW},
    {"iota",     TokenType::IOTA},
};

Lexer::Lexer(std::string source)
    : source_(std::move(source)), tokens_(), start_(0), current_(0), line_(1) {
    // UTF-8 BOM (EF BB BF) varsa atla
    if (source_.size() >= 3 &&
        (unsigned char)source_[0] == 0xEF &&
        (unsigned char)source_[1] == 0xBB &&
        (unsigned char)source_[2] == 0xBF) {
        current_ = 3;
        start_   = 3;
    }
}

std::vector<Token> Lexer::scan_tokens() {
    while (!is_at_end()) {
        start_ = current_;
        start_col_ = col_;
        scan_token();
    }
    // ASI at EOF — dosya newline olmadan bitiyorsa son ifadeyi kapat
    // paren_depth==0: normal durum. paren_depth>0 ama closure body içindeyse de tetikle.
    // closure body: brace_depth > brace_depth'te (ilgili paren açıldığında snapshot alınır)
    bool _asi_ok_eof = paren_depth_ == 0 ||
        (!paren_brace_stack_.empty() && brace_depth_ > paren_brace_stack_.back());
    if (_asi_ok_eof && bracket_depth_ == 0 &&
        should_insert_semicolon(last_token_type_) &&
        !is_continuation_ahead()) {
        tokens_.push_back(Token{TokenType::SEMICOLON, ";", std::nullopt, line_, col_});
        last_token_type_ = TokenType::SEMICOLON;
    }
    tokens_.push_back(Token{TokenType::EOF_TOKEN, "", std::nullopt, line_, col_});
    return tokens_;
}

// Sonraki satır bir devam operatörüyle başlıyorsa ASI bastır.
// Örnek: "mysql://" . env(...)\n    . "@"  — . ile başlayan satır devam eder.
bool Lexer::is_continuation_ahead() const {
    size_t pos = current_;
    // Mevcut satırdaki kalan boşlukları atla
    while (pos < source_.size() && source_[pos] != '\n' &&
           (source_[pos] == ' ' || source_[pos] == '\t' || source_[pos] == '\r'))
        pos++;
    // Satır sonuna geldik, bir sonraki satırın başına geç
    if (pos < source_.size() && source_[pos] == '\n') pos++;
    // Yeni satırdaki leading boşlukları atla
    while (pos < source_.size() && (source_[pos] == ' ' || source_[pos] == '\t'))
        pos++;
    if (pos >= source_.size()) return false;
    char c = source_[pos];
    // Bu karakterlerle başlayan satır ifade devamıdır, yeni statement değil
    return (c == '.' || c == '+' || c == '-' || c == '*' || c == '/' ||
            c == '%' || c == '&' || c == '|' || c == '^' || c == '?');
}

bool Lexer::should_insert_semicolon(TokenType t) {
    switch (t) {
        case TokenType::IDENT:
        case TokenType::NUMBER:
        case TokenType::FLOAT_NUM:
        case TokenType::STRING:
        case TokenType::TRUE_KW:
        case TokenType::FALSE_KW:
        case TokenType::NULL_TOKEN:
        case TokenType::RPAREN:
        case TokenType::RBRACKET:
        case TokenType::RBRACE:
        case TokenType::PLUS_PLUS:
        case TokenType::MINUS_MINUS:
        case TokenType::BREAK:
        case TokenType::CONTINUE:
        case TokenType::RETURN:
            return true;
        default:
            return false;
    }
}

bool Lexer::is_at_end() const { return current_ >= source_.length(); }
char Lexer::advance()         { col_++; return source_[current_++]; }
char Lexer::peek() const      { return is_at_end() ? '\0' : source_[current_]; }
char Lexer::peek_next() const { return (current_ + 1 >= source_.length()) ? '\0' : source_[current_ + 1]; }

void Lexer::add_token(TokenType type, std::optional<std::string> literal) {
    std::string text = source_.substr(start_, current_ - start_);
    tokens_.push_back(Token{type, text, literal, line_, start_col_});
    last_token_type_ = type;
}

void Lexer::scan_token() {
    char c = advance();
    switch (c) {
        // :: scope resolution
        case ':':
            if (peek() == ':') { advance(); add_token(TokenType::COLON_COLON); }
            else add_token(TokenType::COLON);
            break;

        // Single char
        case '(':
            paren_brace_stack_.push_back(brace_depth_);
            paren_depth_++;
            add_token(TokenType::LPAREN);
            break;
        case ')':
            if (!paren_brace_stack_.empty()) paren_brace_stack_.pop_back();
            paren_depth_--;
            add_token(TokenType::RPAREN);
            break;
        case '{': brace_depth_++;   add_token(TokenType::LBRACE);    break;
        case '}': brace_depth_--;   add_token(TokenType::RBRACE);    break;
        case '[': bracket_depth_++; add_token(TokenType::LBRACKET);  break;
        case ']': bracket_depth_--; add_token(TokenType::RBRACKET);  break;
        case ',': add_token(TokenType::COMMA);     break;
        case ';': add_token(TokenType::SEMICOLON); break;
        case '~': add_token(TokenType::TILDE);     break;

        // + / += / ++
        case '+':
            if (peek() == '=')      { advance(); add_token(TokenType::PLUS_ASSIGN); }
            else if (peek() == '+') { advance(); add_token(TokenType::PLUS_PLUS);   }
            else add_token(TokenType::PLUS);
            break;

        // - / -= / --
        case '-':
            if (peek() == '=')      { advance(); add_token(TokenType::MINUS_ASSIGN); }
            else if (peek() == '-') { advance(); add_token(TokenType::MINUS_MINUS);  }
            else add_token(TokenType::MINUS);
            break;

        // * / *= / **
        case '*':
            if (peek() == '=')      { advance(); add_token(TokenType::STAR_ASSIGN); }
            else if (peek() == '*') { advance(); add_token(TokenType::STAR_STAR);   }
            else add_token(TokenType::STAR);
            break;

        // / / /= / comment
        case '/':
            if (peek() == '=') {
                advance(); add_token(TokenType::SLASH_ASSIGN);
            } else if (peek() == '/') {
                while (!is_at_end() && peek() != '\n') advance();
            } else if (peek() == '*') {
                advance(); // consume *
                while (!is_at_end()) {
                    if (peek() == '*' && peek_next() == '/') { advance(); advance(); break; }
                    if (peek() == '\n') line_++;
                    advance();
                }
            } else {
                add_token(TokenType::SLASH);
            }
            break;

        // % / %=
        case '%':
            if (peek() == '=') { advance(); add_token(TokenType::PERCENT_ASSIGN); }
            else add_token(TokenType::PERCENT);
            break;

        // & / &= / &&
        case '&':
            if (peek() == '&')      { advance(); add_token(TokenType::AMP_AMP);    }
            else if (peek() == '=') { advance(); add_token(TokenType::AMP_ASSIGN); }
            else add_token(TokenType::AMP);
            break;

        // | / |= / ||
        case '|':
            if (peek() == '|')      { advance(); add_token(TokenType::PIPE_PIPE);   }
            else if (peek() == '=') { advance(); add_token(TokenType::PIPE_ASSIGN); }
            else add_token(TokenType::PIPE);
            break;

        // ^ / ^=
        case '^':
            if (peek() == '=') { advance(); add_token(TokenType::CARET_ASSIGN); }
            else add_token(TokenType::CARET);
            break;

        // = / == / =>
        case '=':
            if (peek() == '=')      { advance(); add_token(TokenType::EQUAL_EQUAL); }
            else if (peek() == '>') { advance(); add_token(TokenType::FAT_ARROW);   }
            else add_token(TokenType::ASSIGN);
            break;

        // ! / !=
        case '!':
            if (peek() == '=') { advance(); add_token(TokenType::BANG_EQUAL); }
            else add_token(TokenType::BANG);
            break;

        // < / <= / <<  / <=>
        case '<':
            if (peek() == '=' && peek_next() == '>') {
                advance(); advance(); add_token(TokenType::SPACESHIP);
            } else if (peek() == '=') {
                advance(); add_token(TokenType::LESS_EQUAL);
            } else if (peek() == '<') {
                advance(); add_token(TokenType::LESS_LESS);
            } else {
                add_token(TokenType::LESS);
            }
            break;

        // > / >= / >>
        case '>':
            if (peek() == '=')      { advance(); add_token(TokenType::GREATER_EQUAL);    }
            else if (peek() == '>') { advance(); add_token(TokenType::GREATER_GREATER); }
            else add_token(TokenType::GREATER);
            break;

        // ?? null coalescing
        case '?':
            if (peek() == '?') { advance(); add_token(TokenType::QUESTION_QUESTION); }
            else add_token(TokenType::QUESTION);
            break;

        // ... variadic
        case '.':
            if (peek() == '.' && peek_next() == '.') {
                advance(); advance(); add_token(TokenType::ELLIPSIS);
            } else if (peek() == '=') {
                advance(); add_token(TokenType::DOT_ASSIGN);
            } else {
                add_token(TokenType::DOT);
            }
            break;

        // Backtick raw string — no escape processing
        case '`': raw_string(); break;

        // Whitespace
        case ' ': case '\r': case '\t': break;
        case '\n':
            // ASI: satır sonu + trigger token → sanal ; ekle
            // closure body: paren içinde olsak bile { açıldıktan sonraysa ASI tetikle
            // Ancak sonraki satır devam operatörüyle başlıyorsa (. + - * / % & | ^ ?) ASI bastırılır
            {
                bool _asi_ok = paren_depth_ == 0 ||
                    (!paren_brace_stack_.empty() && brace_depth_ > paren_brace_stack_.back());
                if (_asi_ok && bracket_depth_ == 0 &&
                    should_insert_semicolon(last_token_type_) &&
                    !is_continuation_ahead()) {
                    tokens_.push_back(Token{TokenType::SEMICOLON, ";", std::nullopt, line_, col_});
                    last_token_type_ = TokenType::SEMICOLON;
                }
            }
            line_++; col_ = 1;
            break;

        // Comments
        case '#':
            while (!is_at_end() && peek() != '\n') advance();
            break;

        // Strings
        case '"':  string('"');  break;
        case '\'': string('\''); break;

        // $ prefix variables
        case '$': identifier(); break;

        default:
            if (std::isdigit(c)) {
                current_--;
                number();
            } else if (std::isalpha(c) || c == '_') {
                current_--;
                identifier();
            } else {
                throw std::runtime_error(std::string("Unexpected character: '") + c + "'");
            }
            break;
    }
}

void Lexer::string(char quote) {
    std::string value;
    while (!is_at_end() && peek() != quote) {
        char c = peek();
        if (c == '\n') line_++;

        // Escape sequences
        if (c == '\\') {
            advance(); // consume backslash
            char esc = advance();
            switch (esc) {
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                case 'r':  value += '\r'; break;
                case '\\': value += '\\'; break;
                case '"':  value += '"';  break;
                case '\'': value += '\''; break;
                case '$':  value += '$';  break;
                default:   value += '\\'; value += esc; break;
            }
            continue;
        }

        value += advance();
    }

    if (is_at_end())
        throw std::runtime_error("Unterminated string at line " + std::to_string(line_));

    advance(); // closing quote
    add_token(TokenType::STRING, value);
}

void Lexer::raw_string() {
    std::string value;
    while (!is_at_end() && peek() != '`') {
        if (peek() == '\n') line_++;
        value += advance();
    }
    if (is_at_end())
        throw std::runtime_error("Unterminated raw string at line " + std::to_string(line_));
    advance(); // closing backtick
    add_token(TokenType::STRING, value);
}

void Lexer::number() {
    while (!is_at_end() && std::isdigit(peek())) advance();

    bool is_float = false;
    if (peek() == '.' && std::isdigit(peek_next())) {
        is_float = true;
        advance(); // consume '.'
        while (!is_at_end() && std::isdigit(peek())) advance();
    }

    std::string text = source_.substr(start_, current_ - start_);
    add_token(is_float ? TokenType::FLOAT_NUM : TokenType::NUMBER, text);
}

void Lexer::identifier() {
    while (!is_at_end() && (std::isalnum(peek()) || peek() == '_')) advance();
    std::string text = source_.substr(start_, current_ - start_);
    // Strip leading $ for keyword lookup
    std::string lookup = (text[0] == '$') ? text.substr(1) : text;
    auto it = keywords.find(lookup);
    // Only treat as keyword if there's no $ prefix
    TokenType type = (text[0] != '$' && it != keywords.end()) ? it->second : TokenType::IDENT;
    add_token(type);
}

} // namespace look
