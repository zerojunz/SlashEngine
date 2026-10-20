// Reads a C++ file's declarations from its tokens, keeping track only of what encloses them:
// namespaces, classes, enums, function bodies and other braces. A statement outside functions
// that ends in ";" may declare a function or an alias; one that ends in "{" opens a namespace, a
// class, an enum, a function body or an initializer. Calls are followed everywhere, so each
// string literal knows the call it is passed to.
#include "slash/command/cpp_outline.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <map>
#include <ranges>
#include <set>
#include <utility>

namespace slash::command {
namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);
constexpr std::size_t kMaxText = 240; // characters of a signature or an argument

enum class TokenKind : std::uint8_t { Identifier, Number, String, Char, Punct };

struct Token {
    TokenKind kind = TokenKind::Punct;
    std::string_view text; // as written
    std::string value;     // a string literal's characters
    int line = 1;
    bool space_before = false; // whitespace or a comment before it
};

// A comment on lines of its own, by the line it ends on.
struct Comment {
    int first = 0;
    std::string text;
};

struct Lexed {
    std::vector<Token> tokens;
    std::map<int, Comment> comments;
    std::vector<std::string> includes;
    int lines = 0;
};

bool identifier_start(char c) {
    const auto u = static_cast<unsigned char>(c);
    return std::isalpha(u) != 0 || c == '_' || u >= 0x80;
}

bool identifier_char(char c) {
    return identifier_start(c) || std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool is(const Token& token, std::string_view text) {
    return token.kind != TokenKind::String && token.text == text;
}

std::string trimmed(std::string_view text) {
    const std::size_t begin = text.find_first_not_of(" \t\r");
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t end = text.find_last_not_of(" \t\r");
    return std::string(text.substr(begin, end - begin + 1));
}

// A comment's words: "// text", "/// text", or each line of a block comment without its "*".
std::string comment_text(std::string_view body, bool block) {
    if (!block) {
        while (!body.empty() && (body.front() == '/' || body.front() == '!')) {
            body.remove_prefix(1);
        }
        return trimmed(body);
    }
    std::string out;
    while (!body.empty()) {
        const std::size_t end = body.find('\n');
        const std::string_view line = body.substr(0, end);
        std::string text = trimmed(line);
        std::size_t stars = 0;
        while (stars < text.size() && (text[stars] == '*' || text[stars] == '!')) {
            ++stars;
        }
        text = trimmed(std::string_view(text).substr(stars));
        if (!text.empty()) {
            if (!out.empty()) {
                out += ' ';
            }
            out += text;
        }
        if (end == std::string_view::npos) {
            break;
        }
        body.remove_prefix(end + 1);
    }
    return out;
}

// A quoted literal's characters, with the common escapes turned into what they stand for.
std::string unescaped(std::string_view body) {
    std::string out;
    out.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '\\' || i + 1 == body.size()) {
            out += body[i];
            continue;
        }
        const char next = body[++i];
        switch (next) {
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case '0':
            out += '\0';
            break;
        case '\\':
        case '"':
        case '\'':
        case '?':
            out += next;
            break;
        default:
            out += '\\';
            out += next;
        }
    }
    return out;
}

class Lexer {
public:
    explicit Lexer(std::string_view text) : text_(text) {}

    Lexed run() {
        while (i_ < text_.size()) {
            const char c = text_[i_];
            if (c == '\n') {
                new_line();
                space_ = true;
                ++i_;
            } else if (std::isspace(static_cast<unsigned char>(c)) != 0) {
                space_ = true;
                ++i_;
            } else if (c == '/' && peek(1) == '/') {
                line_comment();
            } else if (c == '/' && peek(1) == '*') {
                block_comment();
            } else if (c == '#' && !line_has_token_) {
                directive();
            } else if (c == '"') {
                quoted(i_, i_, '"');
            } else if (c == '\'') {
                quoted(i_, i_, '\'');
            } else if (identifier_start(c)) {
                identifier();
            } else if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
                       (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))) != 0)) {
                number();
            } else {
                punctuation();
            }
        }
        out_.lines = line_ - (text_.empty() || text_.back() == '\n' ? 1 : 0);
        return std::move(out_);
    }

private:
    [[nodiscard]] char peek(std::size_t ahead) const {
        return i_ + ahead < text_.size() ? text_[i_ + ahead] : '\0';
    }

    void new_line() {
        ++line_;
        line_has_token_ = false;
    }

    // Counts the line breaks in text_[from, to).
    void lines_in(std::size_t from, std::size_t to) {
        for (std::size_t k = from; k < to; ++k) {
            if (text_[k] == '\n') {
                new_line();
            }
        }
    }

    void push(TokenKind kind, std::size_t begin, std::size_t end, int line,
              std::string value = {}) {
        out_.tokens.push_back(
            {kind, text_.substr(begin, end - begin), std::move(value), line, space_});
        space_ = false;
        line_has_token_ = true;
    }

    void line_comment() {
        std::size_t end = text_.find('\n', i_);
        if (end == std::string_view::npos) {
            end = text_.size();
        }
        if (!line_has_token_) {
            add_comment(line_, line_, comment_text(text_.substr(i_ + 2, end - i_ - 2), false));
        }
        i_ = end;
        space_ = true;
    }

    void block_comment() {
        const std::size_t end = text_.find("*/", i_ + 2);
        const std::size_t body_end = end == std::string_view::npos ? text_.size() : end;
        const std::size_t stop = end == std::string_view::npos ? text_.size() : end + 2;
        const bool own_line = !line_has_token_;
        const int first = line_;
        lines_in(i_, stop);
        if (own_line) {
            add_comment(first, line_, comment_text(text_.substr(i_ + 2, body_end - i_ - 2), true));
        }
        i_ = stop;
        space_ = true;
    }

    void add_comment(int first, int last, std::string text) {
        // A comment of only punctuation, such as a line of dashes, separates code; it says
        // nothing about what follows.
        if (std::ranges::none_of(text, [](char c) { return identifier_char(c); })) {
            return;
        }
        out_.comments[last] = {first, std::move(text)};
    }

    // A preprocessor line, with its continuations. Only #include "file" is kept.
    void directive() {
        std::size_t end = i_;
        for (;;) {
            end = text_.find('\n', end);
            if (end == std::string_view::npos) {
                end = text_.size();
                break;
            }
            std::size_t back = end;
            if (back > i_ && text_[back - 1] == '\r') {
                --back;
            }
            if (back > i_ && text_[back - 1] == '\\') {
                ++end;
                continue;
            }
            break;
        }
        std::string_view line = text_.substr(i_ + 1, end - i_ - 1);
        line.remove_prefix(std::min(line.find_first_not_of(" \t"), line.size()));
        if (line.starts_with("include")) {
            line.remove_prefix(7);
            line.remove_prefix(std::min(line.find_first_not_of(" \t"), line.size()));
            if (line.starts_with('"')) {
                if (const std::size_t close = line.find('"', 1); close != std::string_view::npos) {
                    out_.includes.emplace_back(line.substr(1, close - 1));
                }
            }
        }
        lines_in(i_, end);
        i_ = end;
        space_ = true;
    }

    // A string or character literal whose quote is at text_[quote], its prefix from begin.
    void quoted(std::size_t begin, std::size_t quote, char mark) {
        const int line = line_;
        std::size_t k = quote + 1;
        while (k < text_.size() && text_[k] != mark && text_[k] != '\n') {
            k += text_[k] == '\\' ? std::size_t{2} : std::size_t{1};
        }
        k = std::min(k, text_.size());
        const std::string_view body = text_.substr(quote + 1, k - quote - 1);
        const std::size_t end = k < text_.size() && text_[k] == mark ? k + 1 : k;
        push(mark == '"' ? TokenKind::String : TokenKind::Char, begin, end, line,
             mark == '"' ? unescaped(body) : std::string());
        i_ = end;
    }

    // R"delimiter( ... )delimiter", its prefix from begin.
    void raw(std::size_t begin, std::size_t quote) {
        const int line = line_;
        const std::size_t open = text_.find('(', quote);
        if (open == std::string_view::npos || open - quote > 17) {
            quoted(begin, quote, '"');
            return;
        }
        const std::string close =
            ")" + std::string(text_.substr(quote + 1, open - quote - 1)) + "\"";
        const std::size_t found = text_.find(close, open + 1);
        const std::size_t body_end = found == std::string_view::npos ? text_.size() : found;
        const std::size_t end =
            found == std::string_view::npos ? text_.size() : found + close.size();
        push(TokenKind::String, begin, end, line,
             std::string(text_.substr(open + 1, body_end - open - 1)));
        lines_in(begin, end);
        line_has_token_ = true;
        i_ = end;
    }

    void identifier() {
        const std::size_t begin = i_;
        while (i_ < text_.size() && identifier_char(text_[i_])) {
            ++i_;
        }
        const std::string_view word = text_.substr(begin, i_ - begin);
        constexpr std::array<std::string_view, 4> kStringPrefixes = {"u8", "u", "U", "L"};
        const bool prefix = std::ranges::find(kStringPrefixes, word) != kStringPrefixes.end();
        const bool raw_prefix =
            word == "R" || (word.size() >= 2 && word.back() == 'R' &&
                            std::ranges::find(kStringPrefixes, word.substr(0, word.size() - 1)) !=
                                kStringPrefixes.end());
        if (i_ < text_.size() && text_[i_] == '"' && raw_prefix) {
            raw(begin, i_);
        } else if (i_ < text_.size() && (text_[i_] == '"' || text_[i_] == '\'') && prefix) {
            quoted(begin, i_, text_[i_]);
        } else {
            push(TokenKind::Identifier, begin, i_, line_);
        }
    }

    void number() {
        const std::size_t begin = i_;
        while (i_ < text_.size()) {
            const char c = text_[i_];
            const char before = i_ > begin ? text_[i_ - 1] : '\0';
            if (identifier_char(c) || c == '.' || (c == '\'' && identifier_char(peek(1))) ||
                ((c == '+' || c == '-') &&
                 (before == 'e' || before == 'E' || before == 'p' || before == 'P'))) {
                ++i_;
            } else {
                break;
            }
        }
        push(TokenKind::Number, begin, i_, line_);
    }

    void punctuation() {
        const std::size_t begin = i_;
        const std::string_view rest = text_.substr(i_);
        std::size_t size = 1;
        if (rest.starts_with("::") || rest.starts_with("->")) {
            size = 2;
        } else if (rest.starts_with("...")) {
            size = 3;
        }
        i_ += size;
        push(TokenKind::Punct, begin, i_, line_);
    }

    std::string_view text_;
    std::size_t i_ = 0;
    int line_ = 1;
    bool space_ = false;
    bool line_has_token_ = false;
    Lexed out_;
};

bool is_keyword(std::string_view word) {
    static const std::set<std::string_view, std::less<>> kKeywords = {"alignas",
                                                                      "alignof",
                                                                      "catch",
                                                                      "co_await",
                                                                      "co_return",
                                                                      "co_yield",
                                                                      "decltype",
                                                                      "delete",
                                                                      "do",
                                                                      "explicit",
                                                                      "for",
                                                                      "if",
                                                                      "new",
                                                                      "noexcept",
                                                                      "requires",
                                                                      "return",
                                                                      "sizeof",
                                                                      "static_assert",
                                                                      "switch",
                                                                      "throw",
                                                                      "typeid",
                                                                      "while",
                                                                      "using",
                                                                      "typedef",
                                                                      "operator",
                                                                      "namespace",
                                                                      "template",
                                                                      "static_cast",
                                                                      "const_cast",
                                                                      "dynamic_cast",
                                                                      "reinterpret_cast"};
    return kKeywords.contains(word);
}

bool is_macro_name(const Token& token) {
    if (token.kind != TokenKind::Identifier || token.text.size() < 2) {
        return false;
    }
    bool letter = false;
    for (const char c : token.text) {
        if (std::islower(static_cast<unsigned char>(c)) != 0) {
            return false;
        }
        letter = letter || std::isupper(static_cast<unsigned char>(c)) != 0;
    }
    return letter;
}

// Tokens joined as written, each run of spaces, line breaks and comments one space.
class TextBuilder {
public:
    explicit TextBuilder(std::size_t limit = kMaxText) : limit_(limit) {}

    void add(const Token& token) {
        if (full_) {
            return;
        }
        const bool closing = is(token, ")") || is(token, ",") || is(token, "]") || is(token, ";");
        if (token.space_before && !text_.empty() && !closing && text_.back() != '(' &&
            text_.back() != '[') {
            text_ += ' ';
        }
        text_ += token.text;
        if (text_.size() > limit_) {
            text_.resize(limit_);
            text_ += "...";
            full_ = true;
        }
    }

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
    void clear() {
        text_.clear();
        full_ = false;
    }

private:
    std::size_t limit_;
    std::string text_;
    bool full_ = false;
};

enum class ScopeKind : std::uint8_t { Namespace, Class, Enum, Function, Block, Init };

struct Scope {
    ScopeKind kind = ScopeKind::Namespace;
    std::string name;                               // a namespace's or class's
    std::size_t symbol = kNone;                     // a function's
    std::string function;                           // a function's name, for the strings in it
    std::set<std::string, std::less<>> identifiers; // a function body's
    std::vector<std::size_t> statement;             // a namespace's or class's statement so far
    int parens = 0;                                 // open in that statement
    std::size_t calls = 0;                          // calls open when it began
};

// A call whose arguments are being read.
struct Call {
    std::string callee;
    std::vector<std::string> args;
    TextBuilder current;
    std::vector<std::size_t> strings; // into CppOutline::strings
    int nesting = 0;                  // braces and brackets open inside it
};

class Parser {
public:
    explicit Parser(Lexed lexed) : lexed_(std::move(lexed)), tokens_(lexed_.tokens) {
        scopes_.push_back({});
    }

    CppOutline run() {
        for (std::size_t t = 0; t < tokens_.size(); ++t) {
            current_ = t;
            step(t);
        }
        while (scopes_.size() > 1) {
            close_scope();
        }
        out_.includes = std::move(lexed_.includes);
        out_.identifiers.assign(identifiers_.begin(), identifiers_.end());
        out_.lines = lexed_.lines;
        return std::move(out_);
    }

private:
    [[nodiscard]] bool statement_scope() const {
        const ScopeKind kind = scopes_.back().kind;
        return kind == ScopeKind::Namespace || kind == ScopeKind::Class;
    }

    // The innermost function the tokens are in, or nullptr.
    Scope* function_scope() {
        for (Scope& scope : std::views::reverse(scopes_)) {
            if (scope.kind == ScopeKind::Function) {
                return &scope;
            }
        }
        return nullptr;
    }

    void step(std::size_t t) {
        const Token& token = tokens_[t];
        Scope* function = function_scope();
        if (token.kind == TokenKind::Identifier) {
            identifiers_.emplace(token.text);
            if (function != nullptr) {
                function->identifiers.emplace(token.text);
            }
        }
        if (token.kind == TokenKind::String) {
            string_literal(t, function);
        }
        if (token.kind == TokenKind::Punct) {
            if (token.text == "(") {
                add_to_calls(token);
                calls_.push_back({callee_before(t), {}, TextBuilder(), {}, 0});
                parenthesis(t, 1);
                return;
            }
            if (token.text == ")") {
                end_call();
                add_to_calls(token);
                parenthesis(t, -1);
                return;
            }
            if (token.text == "," && !calls_.empty() && calls_.back().nesting == 0) {
                Call& call = calls_.back();
                call.args.push_back(call.current.text());
                call.current.clear();
                for (std::size_t k = 0; k + 1 < calls_.size(); ++k) {
                    calls_[k].current.add(token);
                }
                add_to_statement(t);
                return;
            }
            if (token.text == "[" || token.text == "{") {
                add_to_calls(token);
                if (!calls_.empty()) {
                    ++calls_.back().nesting;
                }
                if (token.text == "{") {
                    open_scope(t);
                } else {
                    add_to_statement(t);
                }
                return;
            }
            if (token.text == "]" || token.text == "}") {
                if (!calls_.empty() && calls_.back().nesting > 0) {
                    --calls_.back().nesting;
                }
                add_to_calls(token);
                if (token.text == "}") {
                    if (scopes_.size() > 1) {
                        close_scope();
                    }
                } else {
                    add_to_statement(t);
                }
                return;
            }
            if (token.text == ";" && statement_scope() && scopes_.back().parens == 0) {
                add_to_calls(token);
                end_statement();
                return;
            }
            if (token.text == ":" && statement_scope()) {
                // "public:" and the like begin no declaration.
                std::vector<std::size_t>& statement = scopes_.back().statement;
                if (statement.size() == 1 &&
                    (is(tokens_[statement[0]], "public") || is(tokens_[statement[0]], "private") ||
                     is(tokens_[statement[0]], "protected"))) {
                    statement.clear();
                    return;
                }
            }
        }
        add_to_calls(token);
        add_to_statement(t);
    }

    void add_to_calls(const Token& token) {
        for (Call& call : calls_) {
            call.current.add(token);
        }
    }

    void add_to_statement(std::size_t t) {
        if (statement_scope()) {
            scopes_.back().statement.push_back(t);
        }
    }

    void parenthesis(std::size_t t, int change) {
        if (statement_scope()) {
            Scope& scope = scopes_.back();
            scope.parens = std::max(0, scope.parens + change);
            scope.statement.push_back(t);
        }
    }

    // The function a "(" calls: the name before it, or before its template arguments.
    [[nodiscard]] std::string callee_before(std::size_t t) const {
        if (t == 0) {
            return {};
        }
        std::size_t k = t - 1;
        if (is(tokens_[k], ">")) {
            int depth = 0;
            for (;; --k) {
                if (is(tokens_[k], ">")) {
                    ++depth;
                } else if (is(tokens_[k], "<") && --depth == 0) {
                    break;
                }
                if (k == 0) {
                    return {};
                }
            }
            if (k == 0) {
                return {};
            }
            --k;
        }
        const Token& name = tokens_[k];
        if (name.kind != TokenKind::Identifier || is_keyword(name.text)) {
            return {};
        }
        return std::string(name.text);
    }

    void end_call() {
        if (calls_.empty()) {
            return;
        }
        Call call = std::move(calls_.back());
        calls_.pop_back();
        if (!call.current.empty() || !call.args.empty()) {
            call.args.push_back(call.current.text());
        }
        for (const std::size_t index : call.strings) {
            out_.strings[index].args = call.args;
        }
    }

    void string_literal(std::size_t t, const Scope* function) {
        const Token& token = tokens_[t];
        // "a" "b" is one literal.
        if (t > 0 && tokens_[t - 1].kind == TokenKind::String && !out_.strings.empty()) {
            out_.strings.back().value += token.value;
            return;
        }
        CppString literal;
        literal.value = token.value;
        literal.line = token.line;
        literal.compared = compared_with(t);
        if (function != nullptr) {
            literal.function = function->function;
        }
        if (!calls_.empty()) {
            literal.callee = calls_.back().callee;
            literal.arg = calls_.back().args.size();
            calls_.back().strings.push_back(out_.strings.size());
        }
        out_.strings.push_back(std::move(literal));
    }

    // The name a string literal is compared with by == or !=: the last part of "event.kind" in
    // event.kind == "hit" or "hit" != event.kind.
    [[nodiscard]] std::string compared_with(std::size_t t) const {
        const auto equality = [this](std::size_t first) {
            return first + 1 < tokens_.size() &&
                   (is(tokens_[first], "=") || is(tokens_[first], "!")) &&
                   is(tokens_[first + 1], "=") && !tokens_[first + 1].space_before;
        };
        if (t >= 3 && equality(t - 2) && tokens_[t - 3].kind == TokenKind::Identifier) {
            return std::string(tokens_[t - 3].text);
        }
        if (equality(t + 1)) {
            std::size_t k = t + 3;
            std::string name;
            while (k < tokens_.size() && tokens_[k].kind == TokenKind::Identifier) {
                name = std::string(tokens_[k].text);
                if (k + 2 < tokens_.size() &&
                    (is(tokens_[k + 1], ".") || is(tokens_[k + 1], "->"))) {
                    k += 2;
                } else {
                    break;
                }
            }
            return name;
        }
        return {};
    }

    // Takes the macros a statement starts with out of it, and returns its remaining tokens
    // without attributes ([[nodiscard]]) and a template<...> prefix.
    std::vector<std::size_t> declaration(std::vector<std::size_t>& statement,
                                         std::string* last_macro) {
        while (statement.size() >= 2 && is_macro_name(tokens_[statement[0]]) &&
               is(tokens_[statement[1]], "(")) {
            CppMacro macro;
            macro.name = std::string(tokens_[statement[0]].text);
            macro.line = tokens_[statement[0]].line;
            int depth = 0;
            std::size_t k = 1;
            TextBuilder arg;
            for (; k < statement.size(); ++k) {
                const Token& token = tokens_[statement[k]];
                if (is(token, "(") || is(token, "{") || is(token, "[")) {
                    if (depth++ == 0) {
                        continue;
                    }
                } else if (is(token, ")") || is(token, "}") || is(token, "]")) {
                    if (--depth == 0) {
                        break;
                    }
                } else if (is(token, ",") && depth == 1) {
                    macro.args.push_back(arg.text());
                    arg.clear();
                    continue;
                }
                arg.add(token);
            }
            if (!arg.empty() || !macro.args.empty()) {
                macro.args.push_back(arg.text());
            }
            if (last_macro != nullptr) {
                *last_macro = macro.name + "(";
                for (std::size_t a = 0; a < macro.args.size(); ++a) {
                    *last_macro += (a > 0 ? ", " : "") + macro.args[a];
                }
                *last_macro += ")";
            }
            out_.macros.push_back(std::move(macro));
            statement.erase(statement.begin(),
                            statement.begin() +
                                static_cast<std::ptrdiff_t>(std::min(k + 1, statement.size())));
        }
        std::vector<std::size_t> out;
        std::size_t k = 0;
        for (;;) {
            if (k + 1 < statement.size() && is(tokens_[statement[k]], "[") &&
                is(tokens_[statement[k + 1]], "[")) {
                int depth = 0;
                for (; k < statement.size(); ++k) {
                    if (is(tokens_[statement[k]], "[")) {
                        ++depth;
                    } else if (is(tokens_[statement[k]], "]") && --depth == 0) {
                        ++k;
                        break;
                    }
                }
                continue;
            }
            break;
        }
        out.assign(statement.begin() + static_cast<std::ptrdiff_t>(k), statement.end());
        return out;
    }

    // Where the declaration proper starts: past "template <...>" prefixes.
    [[nodiscard]] std::size_t past_templates(const std::vector<std::size_t>& tokens) const {
        std::size_t k = 0;
        while (k + 1 < tokens.size() && is(tokens_[tokens[k]], "template") &&
               is(tokens_[tokens[k + 1]], "<")) {
            int depth = 0;
            for (++k; k < tokens.size(); ++k) {
                if (is(tokens_[tokens[k]], "<")) {
                    ++depth;
                } else if (is(tokens_[tokens[k]], ">") && --depth == 0) {
                    ++k;
                    break;
                }
            }
        }
        return k;
    }

    // The index of the parameters' "(" from begin, outside template arguments, or kNone.
    [[nodiscard]] std::size_t parameters(const std::vector<std::size_t>& tokens,
                                         std::size_t begin) const {
        int angles = 0;
        for (std::size_t k = begin; k < tokens.size(); ++k) {
            const Token& token = tokens_[tokens[k]];
            if (is(token, "operator")) {
                ++k;
                if (k + 1 < tokens.size() && is(tokens_[tokens[k]], "(") &&
                    is(tokens_[tokens[k + 1]], ")")) {
                    k += 2;
                }
                while (k < tokens.size() && !is(tokens_[tokens[k]], "(")) {
                    ++k;
                }
                return k < tokens.size() ? k : kNone;
            }
            if (is(token, "<") && k > begin &&
                tokens_[tokens[k - 1]].kind == TokenKind::Identifier) {
                ++angles;
            } else if (is(token, ">") && angles > 0) {
                --angles;
            } else if (is(token, "(") && angles == 0) {
                return k;
            }
        }
        return kNone;
    }

    // The name a function's parameters follow, with the classes it is qualified by, or "".
    [[nodiscard]] std::string function_name(const std::vector<std::size_t>& tokens,
                                            std::size_t begin, std::size_t paren) const {
        std::size_t operator_at = kNone;
        for (std::size_t k = begin; k < paren; ++k) {
            if (is(tokens_[tokens[k]], "operator")) {
                operator_at = k;
                break;
            }
        }
        std::string name;
        std::size_t k = 0; // the token before the name
        if (operator_at != kNone) {
            for (std::size_t j = operator_at; j < paren; ++j) {
                const Token& token = tokens_[tokens[j]];
                if (j > operator_at && token.kind == TokenKind::Identifier &&
                    tokens_[tokens[j - 1]].kind == TokenKind::Identifier) {
                    name += ' ';
                }
                name += token.text;
            }
            if (operator_at == begin) {
                return name;
            }
            k = operator_at - 1;
        } else {
            if (paren == begin) {
                return {};
            }
            const Token& last = tokens_[tokens[paren - 1]];
            if (last.kind != TokenKind::Identifier || is_keyword(last.text)) {
                return {};
            }
            name = std::string(last.text);
            if (paren - 1 == begin) {
                return name;
            }
            k = paren - 2;
            if (is(tokens_[tokens[k]], "~")) {
                name.insert(0, "~");
                if (k == begin) {
                    return name;
                }
                --k;
            }
        }
        while (k > begin && is(tokens_[tokens[k]], "::") &&
               tokens_[tokens[k - 1]].kind == TokenKind::Identifier) {
            name.insert(0, std::string(tokens_[tokens[k - 1]].text) + "::");
            if (k - 1 == begin) {
                break;
            }
            k -= 2;
        }
        return name;
    }

    [[nodiscard]] std::string scope_path() const {
        std::string path;
        for (const Scope& scope : scopes_) {
            if ((scope.kind == ScopeKind::Namespace || scope.kind == ScopeKind::Class) &&
                !scope.name.empty()) {
                if (!path.empty()) {
                    path += "::";
                }
                path += scope.name;
            }
        }
        return path;
    }

    [[nodiscard]] std::string member_of() const {
        return scopes_.back().kind == ScopeKind::Class ? scopes_.back().name : std::string();
    }

    [[nodiscard]] std::string doc_above(int line) const {
        std::vector<const std::string*> parts;
        for (int at = line - 1;;) {
            const auto found = lexed_.comments.find(at);
            if (found == lexed_.comments.end()) {
                break;
            }
            parts.push_back(&found->second.text);
            at = found->second.first - 1;
        }
        std::string doc;
        for (const std::string* part : std::views::reverse(parts)) {
            if (!doc.empty()) {
                doc += ' ';
            }
            doc += *part;
        }
        return doc;
    }

    // A function's declaration as written, up to a constructor's member initializers.
    [[nodiscard]] std::string signature(const std::vector<std::size_t>& tokens,
                                        std::size_t paren) const {
        TextBuilder text;
        int depth = 0;
        for (std::size_t k = 0; k < tokens.size(); ++k) {
            const Token& token = tokens_[tokens[k]];
            if (is(token, "(")) {
                ++depth;
            } else if (is(token, ")")) {
                --depth;
            } else if (is(token, ":") && depth == 0 && k > paren) {
                break;
            }
            text.add(token);
        }
        return text.text();
    }

    std::size_t add_function(const std::vector<std::size_t>& tokens, std::size_t paren,
                             std::string name, bool definition) {
        const Token& first = tokens_[tokens.front()];
        CppSymbol symbol;
        symbol.kind = "function";
        symbol.name = std::move(name);
        symbol.scope = scope_path();
        symbol.member_of = member_of();
        symbol.signature = signature(tokens, paren);
        symbol.doc = doc_above(first.line);
        symbol.line = tokens_[tokens[paren > 0 ? paren - 1 : 0]].line;
        symbol.definition = definition;
        out_.symbols.push_back(std::move(symbol));
        return out_.symbols.size() - 1;
    }

    void add_type(std::string kind, std::string name, int line, int first_line) {
        CppSymbol symbol;
        symbol.kind = std::move(kind);
        symbol.name = std::move(name);
        symbol.scope = scope_path();
        symbol.member_of = member_of();
        symbol.doc = doc_above(first_line);
        symbol.line = line;
        symbol.definition = true;
        out_.symbols.push_back(std::move(symbol));
    }

    // A class's or enum's name: the last identifier from begin outside template arguments and
    // parentheses, before a base clause or "final".
    [[nodiscard]] std::pair<std::string, int> type_name(const std::vector<std::size_t>& tokens,
                                                        std::size_t begin) const {
        std::pair<std::string, int> found;
        int depth = 0;
        for (std::size_t k = begin; k < tokens.size(); ++k) {
            const Token& token = tokens_[tokens[k]];
            if (is(token, "<") || is(token, "(")) {
                ++depth;
            } else if (is(token, ">") || is(token, ")")) {
                --depth;
            } else if (depth == 0 && (is(token, ":") || is(token, "final"))) {
                break;
            } else if (depth == 0 && token.kind == TokenKind::Identifier && !is(token, "class") &&
                       !is(token, "struct")) {
                found = {std::string(token.text), token.line};
            }
        }
        return found;
    }

    // The first token of the declaration from begin with one of the class keywords, before
    // any "(" or "=", or kNone.
    [[nodiscard]] std::size_t keyword_at(const std::vector<std::size_t>& tokens, std::size_t begin,
                                         std::initializer_list<std::string_view> words) const {
        for (std::size_t k = begin; k < tokens.size(); ++k) {
            const Token& token = tokens_[tokens[k]];
            if (is(token, "(") || is(token, "=")) {
                return kNone;
            }
            if (std::ranges::any_of(words,
                                    [&](std::string_view word) { return is(token, word); })) {
                return k;
            }
        }
        return kNone;
    }

    [[nodiscard]] bool assigns_before(const std::vector<std::size_t>& tokens, std::size_t begin,
                                      std::size_t end) const {
        for (std::size_t k = begin; k < end && k < tokens.size(); ++k) {
            if (is(tokens_[tokens[k]], "=")) {
                return true;
            }
        }
        return false;
    }

    void open_scope(std::size_t t) {
        Scope next;
        next.calls = calls_.size();
        if (!statement_scope()) {
            next.kind = ScopeKind::Block;
            scopes_.push_back(std::move(next));
            return;
        }
        Scope& scope = scopes_.back();
        if (scope.parens > 0) {
            // A brace inside parentheses, such as a default argument's "{}".
            scope.statement.push_back(t);
            next.kind = ScopeKind::Init;
            scopes_.push_back(std::move(next));
            return;
        }
        std::string macro;
        const std::vector<std::size_t> tokens = declaration(scope.statement, &macro);
        const std::size_t begin = past_templates(tokens);
        next.kind = ScopeKind::Init;
        if (tokens.empty()) {
            next.kind = macro.empty() ? ScopeKind::Block : ScopeKind::Function;
            next.function = macro;
        } else if (is(tokens_[tokens[0]], "namespace") ||
                   (tokens.size() > 1 && is(tokens_[tokens[0]], "inline") &&
                    is(tokens_[tokens[1]], "namespace"))) {
            next.kind = ScopeKind::Namespace;
            for (std::size_t k = is(tokens_[tokens[0]], "inline") ? 2 : 1; k < tokens.size(); ++k) {
                next.name += tokens_[tokens[k]].text;
            }
        } else if (is(tokens_[tokens[0]], "extern") && tokens.size() == 2 &&
                   tokens_[tokens[1]].kind == TokenKind::String) {
            next.kind = ScopeKind::Namespace; // extern "C" { ... }
        } else {
            const std::size_t paren = parameters(tokens, begin);
            const std::string name =
                paren == kNone ? std::string() : function_name(tokens, begin, paren);
            const std::size_t enum_at = keyword_at(tokens, begin, {"enum"});
            const std::size_t class_at = keyword_at(tokens, begin, {"struct", "class", "union"});
            if (enum_at != kNone) {
                next.kind = ScopeKind::Enum;
                const auto [type, line] = type_name(tokens, enum_at + 1);
                if (!type.empty()) {
                    add_type("enum", type, line, tokens_[tokens.front()].line);
                }
            } else if (class_at != kNone && name.empty()) {
                next.kind = ScopeKind::Class;
                const auto [type, line] = type_name(tokens, class_at + 1);
                next.name = type;
                if (!type.empty()) {
                    add_type(std::string(tokens_[tokens[class_at]].text), type, line,
                             tokens_[tokens.front()].line);
                }
            } else if (!name.empty() && (name.starts_with("operator") ||
                                         name.find("::operator") != std::string::npos ||
                                         !assigns_before(tokens, begin, paren))) {
                next.kind = ScopeKind::Function;
                next.function = name;
                next.symbol = add_function(tokens, paren, name, true);
            }
        }
        if (next.kind == ScopeKind::Init) {
            scope.statement.push_back(t);
        } else {
            scope.statement.clear();
            scope.parens = 0;
        }
        scopes_.push_back(std::move(next));
    }

    void close_scope() {
        const Scope closed = std::move(scopes_.back());
        scopes_.pop_back();
        if (closed.kind == ScopeKind::Function && closed.symbol != kNone) {
            out_.symbols[closed.symbol].body_identifiers.assign(closed.identifiers.begin(),
                                                                closed.identifiers.end());
        }
        while (calls_.size() > closed.calls) {
            end_call();
        }
        if (!statement_scope()) {
            return;
        }
        Scope& scope = scopes_.back();
        if (closed.kind == ScopeKind::Init) {
            // The statement goes on: "int values[] = {1, 2};".
            scope.statement.push_back(closing_brace_index());
        } else {
            scope.statement.clear();
            scope.parens = 0;
        }
    }

    // The "}" just read, which step is handling.
    [[nodiscard]] std::size_t closing_brace_index() const { return current_; }

    void end_statement() {
        Scope& scope = scopes_.back();
        const std::vector<std::size_t> tokens = declaration(scope.statement, nullptr);
        scope.statement.clear();
        scope.parens = 0;
        if (tokens.empty()) {
            return;
        }
        const Token& first = tokens_[tokens[0]];
        if (is(first, "using")) {
            if (tokens.size() > 3 && tokens_[tokens[1]].kind == TokenKind::Identifier &&
                !is(tokens_[tokens[1]], "namespace") && is(tokens_[tokens[2]], "=")) {
                add_type("alias", std::string(tokens_[tokens[1]].text), tokens_[tokens[1]].line,
                         first.line);
            }
            return;
        }
        if (is(first, "typedef")) {
            int depth = 0;
            const Token* name = nullptr;
            for (const std::size_t k : tokens) {
                const Token& token = tokens_[k];
                depth += is(token, "(") || is(token, "<") ? 1 : 0;
                depth -= is(token, ")") || is(token, ">") ? 1 : 0;
                if (depth == 0 && token.kind == TokenKind::Identifier) {
                    name = &token;
                }
            }
            if (name != nullptr) {
                add_type("alias", std::string(name->text), name->line, first.line);
            }
            return;
        }
        if (is(first, "friend") || is(first, "static_assert") || is(first, "extern") ||
            is(first, "template")) {
            return;
        }
        const std::size_t begin = past_templates(tokens);
        const std::size_t paren = parameters(tokens, begin);
        if (paren == kNone) {
            return; // a variable, or a class declared ahead
        }
        std::string name = function_name(tokens, begin, paren);
        if (name.empty() ||
            (!name.starts_with("operator") && name.find("::operator") == std::string::npos &&
             assigns_before(tokens, begin, paren))) {
            return;
        }
        add_function(tokens, paren, std::move(name), false);
    }

    Lexed lexed_;
    const std::vector<Token>& tokens_;
    std::vector<Scope> scopes_;
    std::vector<Call> calls_;
    std::set<std::string, std::less<>> identifiers_;
    std::size_t current_ = 0; // the token step is at
    CppOutline out_;
};

} // namespace

CppOutline outline_cpp(std::string_view text) {
    Parser parser(Lexer(text).run());
    return parser.run();
}

} // namespace slash::command
