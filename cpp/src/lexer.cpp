#include "look/lexer.h"
#include <cctype>
#include <cstdint>
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
    {"fn",       TokenType::FUNCTION},   // short lambda alias -- exactly the same as function
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
    {"throw",    TokenType::THROW},
    {"switch",   TokenType::SWITCH},
    {"case",     TokenType::CASE},
    {"default",  TokenType::DEFAULT},
    {"struct",   TokenType::STRUCT},
    {"const",    TokenType::CONST_KW},
    {"iota",     TokenType::IOTA},
};

Lexer::Lexer(std::string source)
    : source_(std::move(source)), tokens_(), start_(0), current_(0), line_(1) {
    // Skip the UTF-8 BOM (EF BB BF) if present
    if (source_.size() >= 3 &&
        (unsigned char)source_[0] == 0xEF &&
        (unsigned char)source_[1] == 0xBB &&
        (unsigned char)source_[2] == 0xBF) {
        current_ = 3;
        start_   = 3;
    }
}

std::vector<Token> Lexer::scan_tokens() {
    tokens_.reserve(source_.size() / 4 + 16);   // rough estimate: about one token per 4 source bytes
    while (!is_at_end()) {
        start_ = current_;
        start_col_ = col_;
        scan_token();
    }
    // ASI at EOF -- close the last statement if the file ends without a newline
    // paren_depth==0: normal case. Also trigger when paren_depth>0 but we are inside a closure body.
    // closure body: brace_depth > brace_depth at the time (a snapshot is taken when the relevant paren opens)
    bool _asi_ok_eof = paren_depth_ == 0 ||
        (!paren_brace_stack_.empty() && brace_depth_ > paren_brace_stack_.back());
    if (_asi_ok_eof && bracket_depth_ == 0 &&
        should_insert_semicolon(last_token_type_) &&
        !is_continuation_ahead()) {
        tokens_.push_back(Token{TokenType::SEMICOLON, ";", std::nullopt, line_, col_});
        last_token_type_ = TokenType::SEMICOLON;
    }
    tokens_.push_back(Token{TokenType::EOF_TOKEN, "", std::nullopt, line_, col_});
    return std::move(tokens_);   // member -- `return tokens_` used to COPY the whole vector (with every token's text)
}

// Suppress ASI if the next line starts with a continuation operator.
// Example: "mysql://" . env(...)\n    . "@"  -- the line starting with . continues.
bool Lexer::is_continuation_ahead() const {
    size_t pos = current_;
    // Skip the remaining whitespace on the current line
    while (pos < source_.size() && source_[pos] != '\n' &&
           (source_[pos] == ' ' || source_[pos] == '\t' || source_[pos] == '\r'))
        pos++;
    // We reached the end of the line, move to the start of the next line
    if (pos < source_.size() && source_[pos] == '\n') pos++;
    // Skip the leading whitespace on the new line
    while (pos < source_.size() && (source_[pos] == ' ' || source_[pos] == '\t'))
        pos++;
    if (pos >= source_.size()) return false;
    char c = source_[pos];
    // A line starting with `++$x` / `--$x` is a NEW statement, not a continuation of the previous line.
    // (It used to count as a continuation because it started with `-`/`+` and gave an "Expect ';'" error.)
    if ((c == '+' || c == '-') && pos + 1 < source_.size() && source_[pos + 1] == c) return false;
    // A line starting with these characters continues the expression, it is not a new statement
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
    tokens_.push_back(Token{type, source_.substr(start_, current_ - start_), std::move(literal), line_, start_col_});
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
                int comment_line = line_;
                bool closed = false;
                while (!is_at_end()) {
                    if (peek() == '*' && peek_next() == '/') { advance(); advance(); closed = true; break; }
                    if (peek() == '\n') line_++;
                    advance();
                }
                // Fail-loud: an unclosed `/*` used to be SILENTLY swallowed up to EOF and half of the code
                // was lost (exit 0, no error). If it is not closed, give a clear error.
                if (!closed)
                    throw std::runtime_error("Unterminated block comment starting at line " +
                                             std::to_string(comment_line));
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
            // ASI: end of line + trigger token -> insert a virtual ;
            // closure body: trigger ASI even inside parens, if it is after a { has opened
            // But ASI is suppressed if the next line starts with a continuation operator (. + - * / % & | ^ ?)
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

static int lex_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static void lex_append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0|(cp>>6)); out += (char)(0x80|(cp&0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0|(cp>>12)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
    else { out += (char)(0xF0|(cp>>18)); out += (char)(0x80|((cp>>12)&0x3F)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
}

// value: the string read up to the closing quote; current_ is on the closing quote.
void Lexer::check_quote_inside_placeholder(const std::string& value) {
    // The first '{' that stays open (counting nesting)
    std::vector<size_t> open;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '{') open.push_back(i);
        else if (value[i] == '}' && !open.empty()) open.pop_back();
    }
    if (open.empty()) return;
    size_t start = open.front();
    size_t after = current_ + 1;                       // the character after the closing quote
    if (after >= source_.size()) return;
    char n = source_[after];
    if (!(std::isalnum((unsigned char)n) || n == '_')) return;   // no name after the quote
    std::string head = value.substr(start);           // "{$u["
    // Take from the rest of the source up to the '}' that closes the {...} (at most 80 characters); if none, make no suggestion.
    int depth = (int)open.size();
    std::string tail;
    for (size_t j = after; j < source_.size() && tail.size() < 80; ++j) {
        char c = source_[j];
        if (c == '\n') break;
        tail += c;
        if (c == '{') depth++;
        else if (c == '}' && --depth == 0) break;
    }
    if (depth != 0) tail.clear();
    std::string written = head + "\"" + tail;          // what the user wrote: {$u["name"]}
    std::string msg = "Line " + std::to_string(line_) + ": the \" in " + (tail.empty() ? head + "\"…" : written) +
        " ends the string, so the rest of the line is not part of it.";
    if (head.size() >= 2 && head[1] == '$') {
        if (!tail.empty()) {
            std::string fixed = written;
            for (size_t k = 0; k < fixed.size(); ++k) if (fixed[k] == '"') fixed[k] = '\'';
            msg += " Inside {…} write single quotes: " + fixed;
        } else {
            msg += " Inside {…} write single quotes, e.g. {$u['name']}";
        }
    } else {
        // {cache::get("k")} -- only {$...} is substituted; {...} without a dollar stays as text.
        msg += " Only {$…} is replaced inside a string: put the value in a variable first, then write {$value}";
    }
    throw std::runtime_error(msg);
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
                case 'b':  value += '\b'; break;
                case 'f':  value += '\f'; break;
                case '0':  value += '\0'; break;
                case 'u': {
                    // \uXXXX unicode escape (surrogate pair -> including emoji)
                    uint32_t cp = 0; bool ok = true;
                    for (int k = 0; k < 4; k++) {
                        int h = lex_hex(peek());
                        if (h < 0) { ok = false; break; }
                        cp = (cp << 4) | (uint32_t)h; advance();
                    }
                    if (!ok) { value += "\\u"; break; }
                    if (cp >= 0xD800 && cp <= 0xDBFF && peek() == '\\' && peek_next() == 'u') {
                        advance(); advance(); // '\' 'u'
                        uint32_t lo = 0; bool ok2 = true;
                        for (int k = 0; k < 4; k++) {
                            int h = lex_hex(peek());
                            if (h < 0) { ok2 = false; break; }
                            lo = (lo << 4) | (uint32_t)h; advance();
                        }
                        if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    lex_append_utf8(value, cp);
                    break;
                }
                default:   value += '\\'; value += esc; break;
            }
            continue;
        }

        value += advance();
    }

    if (is_at_end())
        throw std::runtime_error("Unterminated string at line " + std::to_string(line_));

    // "Hi {$u["name"]}" -- the inner double quote ENDS the string; the next word became a meaningless
    // error in the parser ("Expect ')' after print arguments. (got 'name')"). When the string closes
    // with an open '{' and a name continues right after the quote, the quote is mistakenly
    // inside the {...}: state the cause and the correct syntax. (No new syntax -- single quotes already work.)
    if (quote == '"') check_quote_inside_placeholder(value);

    advance(); // closing quote
    add_token(TokenType::STRING, std::move(value));
    // Single quote = RAW: {$x} stays as text (the docs always said so; previously all three
    // quote kinds interpolated -> there was no way to safely write template source).
    if (quote == '\'') tokens_.back().raw = true;
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
    add_token(TokenType::STRING, std::move(value));
}

void Lexer::number() {
    // Hex (0xFF) / binary (0b1010) base -- on entry current_==start_, peek()=first digit.
    // '_' for digit grouping (1_000, 0xFF_FF) is accepted in every base; the parser separates it.
    if (peek() == '0' && (peek_next() == 'x' || peek_next() == 'X')) {
        advance(); advance();  // '0' 'x'
        while (!is_at_end() && (std::isxdigit((unsigned char)peek()) || peek() == '_')) advance();
        add_token(TokenType::NUMBER, source_.substr(start_, current_ - start_));
        return;
    }
    if (peek() == '0' && (peek_next() == 'b' || peek_next() == 'B')) {
        advance(); advance();  // '0' 'b'
        while (!is_at_end() && (peek() == '0' || peek() == '1' || peek() == '_')) advance();
        add_token(TokenType::NUMBER, source_.substr(start_, current_ - start_));
        return;
    }

    while (!is_at_end() && (std::isdigit(peek()) || peek() == '_')) advance();

    bool is_float = false;
    if (peek() == '.' && std::isdigit(peek_next())) {
        is_float = true;
        advance(); // consume '.'
        while (!is_at_end() && (std::isdigit(peek()) || peek() == '_')) advance();
    }

    // Scientific notation: 1e3, 1.5e-2, 2E+10 -- e/E [+/-] digit after the mantissa.
    // (to_chars produces large/small floats in this format; needed for round-trip.)
    if (peek() == 'e' || peek() == 'E') {
        char c1 = peek_next();
        bool valid = std::isdigit((unsigned char)c1) ||
            ((c1 == '+' || c1 == '-') && (current_ + 2 < source_.length()) &&
             std::isdigit((unsigned char)source_[current_ + 2]));
        if (valid) {
            is_float = true;
            advance();                                   // 'e' / 'E'
            if (peek() == '+' || peek() == '-') advance();
            while (!is_at_end() && std::isdigit(peek())) advance();
        }
    }

    add_token(is_float ? TokenType::FLOAT_NUM : TokenType::NUMBER, source_.substr(start_, current_ - start_));
}

void Lexer::identifier() {
    while (!is_at_end() && (std::isalnum(peek()) || peek() == '_')) advance();
    std::string text = source_.substr(start_, current_ - start_);
    // A name with a $ prefix is never a keyword -- only names without the prefix are looked up in the table.
    TokenType type = TokenType::IDENT;
    if (text[0] != '$') {
        auto it = keywords.find(text);
        if (it != keywords.end()) type = it->second;
    }
    tokens_.push_back(Token{type, std::move(text), std::nullopt, line_, start_col_});
    last_token_type_ = type;
}

} // namespace look
