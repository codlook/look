#pragma once

#include "look/ast.h"
#include "look/token.h"
#include "look/capture_check.h"
#include "look/param_write_check.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace look {

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    std::unique_ptr<Program> parse();
    // After parse() -- places that create a silent behavior difference (capture-stale).
    const std::vector<CaptureWarning>& warnings() const { return warnings_; }

private:
    std::vector<Token> tokens_;
    size_t current_ = 0;
    int    scope_depth_ = 0;  // function nesting -- for the `use "file"` top-level guard
    std::map<std::string, std::pair<std::string, int>> struct_seen_;  // name -> (definition signature, line)
    std::vector<CaptureWarning> warnings_;   // warnings to show at load time (capture-stale)
    // Deprecated calls -- ONE warning per file for each kind (first location + total)
    enum { DEP_PUSH, DEP_POP, DEP_ARRAY_POP, DEP_COUNT };
    struct Deprecated { int calls = 0; size_t warn = (size_t)-1; };   // warn: the warning's position in warnings_
    Deprecated deps_[DEP_COUNT];
    static std::string deprecation_message(int dep, int count);
    // array::push/slice in place without `use array` (hidden rule; the --check warning is "use-array-missing")
    struct InPlaceArray { int calls = 0, line = 0, column = 0; };
    InPlaceArray in_place_array_;
    bool has_use_array_ = false;
    static bool is_in_place_array_call(const std::string& name, const Expression* val);
    static constexpr int MAX_EXPR_DEPTH = 150;  // expression recursion cap (shared by expression+unary+power)
    int    expr_depth_  = 0;  // expression recursion depth -- stops deeply nested paren/array
                              // from overflowing the parser's recursive-descent stack with a SIGSEGV
                              // (like the guard in the JSON parser).
    int    stmt_depth_  = 0;  // statement/block nesting -- deeply nested {}/if/while
                              // carry the same stack-overflow risk; guarded by a separate counter.
    // A left-leaning binary chain ('1+1+1+...+1' with N terms) is built ITERATIVELY in the parser
    // (a while loop) -- expr_depth_ does NOT grow, so the paren/array guard does NOT CATCH it.
    // But the resulting AST is left-nested N deep; the VM compiler walks this skeleton
    // RECURSIVELY and overflows the stack (SIGABRT) at ~7000 terms
    // -> attacker-controlled deep input = DoS. One term ~ 1 chain link; real
    // code never has a thousand consecutive binary operators. When the limit is exceeded
    // a clean parse error is raised WITHOUT building the AST at all -> the compiler never sees a deep tree.
    static constexpr int MAX_BINOP_CHAIN = 1000;  // cap on binary operators in a single expression
    int    binop_depth_ = 0;  // counter of binary-operator nodes built (reset at the start of expression())
    void   check_binop_chain();  // called before every binary node; clean error on overflow

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
    void          consume_semi();  // consume ';' -- if absent, pass silently at } or EOF (ASI optional)
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
