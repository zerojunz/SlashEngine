#include "slash/importer/godot_text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>

namespace slash::importer::godot {

double Value::number() const noexcept {
    if (kind == Kind::Int) {
        return static_cast<double>(integer);
    }
    return kind == Kind::Float ? real : 0.0;
}

const Value* Value::get(std::string_view key) const {
    if (kind != Kind::Dictionary && !is_call("Object")) {
        return nullptr;
    }
    for (const auto& [name, value] : entries) {
        if (name.is_string() && name.text == key) {
            return &value;
        }
    }
    return nullptr;
}

std::optional<std::string> Value::string_argument() const {
    if (kind == Kind::Call && !items.empty() && items.front().is_string()) {
        return items.front().text;
    }
    return std::nullopt;
}

std::optional<std::vector<double>> Value::numbers() const {
    if (kind != Kind::Call && kind != Kind::Array) {
        return std::nullopt;
    }
    std::vector<double> out;
    out.reserve(items.size());
    for (const Value& item : items) {
        if (!item.is_number()) {
            return std::nullopt;
        }
        out.push_back(item.number());
    }
    return out;
}

const Value* Section::field(std::string_view name) const {
    const auto it = std::ranges::find(fields, name, &std::pair<std::string, Value>::first);
    return it != fields.end() ? &it->second : nullptr;
}

const Value* Section::property(std::string_view name) const {
    const auto it = std::ranges::find(properties, name, &std::pair<std::string, Value>::first);
    return it != properties.end() ? &it->second : nullptr;
}

std::string Section::field_string(std::string_view name) const {
    const Value* value = field(name);
    if (value == nullptr) {
        value = property(name);
    }
    return value != nullptr && value->is_string() ? value->text : std::string();
}

namespace {

enum class TokenType : std::uint8_t {
    CurlyOpen,
    CurlyClose,
    BracketOpen,
    BracketClose,
    ParenOpen,
    ParenClose,
    Identifier,
    String,
    StringName,
    Number,
    Color,
    Colon,
    Comma,
    Period,
    Equal,
    Eof,
};

struct Token {
    TokenType type = TokenType::Eof;
    std::string text; // identifier, string or number text
    Value value;      // a color's value
};

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}
bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_value(char c) {
    if (is_digit(c)) {
        return c - '0';
    }
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
}

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

Value make_float(double number) {
    Value value;
    value.kind = Value::Kind::Float;
    value.real = number;
    return value;
}

// Color::html: "#rgb", "#rgba", "#rrggbb" or "#rrggbbaa".
Value html_color(std::string_view digits) {
    Value color;
    color.kind = Value::Kind::Call;
    color.text = "Color";
    std::vector<double> channels{0.0, 0.0, 0.0, 1.0};
    const bool short_form = digits.size() == 3 || digits.size() == 4;
    const std::size_t width = short_form ? 1 : 2;
    for (std::size_t i = 0; i < 4 && (i + 1) * width <= digits.size(); ++i) {
        int value = 0;
        for (std::size_t j = 0; j < width; ++j) {
            value = value * 16 + hex_value(digits[(i * width) + j]);
        }
        channels[i] = short_form ? value / 15.0 : value / 255.0;
    }
    for (const double channel : channels) {
        color.items.push_back(make_float(channel));
    }
    return color;
}

// Godot's VariantParser over UTF-8 text.
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    [[nodiscard]] const Error& error() const { return error_; }
    [[nodiscard]] bool failed() const { return failed_; }

    bool fail(std::string message) {
        if (!failed_) {
            failed_ = true;
            error_ = Error{.code = ErrorCode::InvalidArgument,
                           .message = "line " + std::to_string(line_) + ": " + std::move(message)};
        }
        return false;
    }

    bool get_token(Token& token) {
        token = Token{};
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            switch (c) {
            case '\n':
                ++line_;
                continue;
            case '{':
                token.type = TokenType::CurlyOpen;
                return true;
            case '}':
                token.type = TokenType::CurlyClose;
                return true;
            case '[':
                token.type = TokenType::BracketOpen;
                return true;
            case ']':
                token.type = TokenType::BracketClose;
                return true;
            case '(':
                token.type = TokenType::ParenOpen;
                return true;
            case ')':
                token.type = TokenType::ParenClose;
                return true;
            case ':':
                token.type = TokenType::Colon;
                return true;
            case ',':
                token.type = TokenType::Comma;
                return true;
            case '.':
                token.type = TokenType::Period;
                return true;
            case '=':
                token.type = TokenType::Equal;
                return true;
            case ';':
                skip_line();
                continue;
            case '#': {
                const std::size_t start = pos_;
                while (pos_ < text_.size() && is_hex(text_[pos_])) {
                    ++pos_;
                }
                token.type = TokenType::Color;
                token.value = html_color(text_.substr(start, pos_ - start));
                return true;
            }
            case '&':
            case '@':
                if (pos_ >= text_.size() || text_[pos_] != '"') {
                    return fail("expected '\"' after '" + std::string(1, c) + "'");
                }
                ++pos_;
                token.type = TokenType::StringName;
                return read_string(token.text);
            case '"':
                token.type = TokenType::String;
                return read_string(token.text);
            default:
                break;
            }
            if (static_cast<unsigned char>(c) <= 32) {
                continue;
            }
            std::string word;
            char next = c;
            if (next == '-') {
                word += '-';
                if (pos_ >= text_.size()) {
                    return fail("unexpected character '-'");
                }
                next = text_[pos_++];
            }
            if (is_digit(next)) {
                word += next;
                read_number(word);
                token.type = TokenType::Number;
                token.text = std::move(word);
                return true;
            }
            if (is_alpha(next)) {
                word += next;
                while (pos_ < text_.size() && (is_alpha(text_[pos_]) || is_digit(text_[pos_]))) {
                    word += text_[pos_++];
                }
                token.type = TokenType::Identifier;
                token.text = std::move(word);
                return true;
            }
            return fail("unexpected character '" + std::string(1, next) + "'");
        }
        token.type = TokenType::Eof;
        return true;
    }

    bool parse_value(const Token& token, Value& value) {
        switch (token.type) {
        case TokenType::CurlyOpen:
            return parse_dictionary(value);
        case TokenType::BracketOpen:
            return parse_array(value, TokenType::BracketClose);
        case TokenType::Number:
            return number_value(token.text, value);
        case TokenType::String:
        case TokenType::StringName:
            value.kind =
                token.type == TokenType::String ? Value::Kind::String : Value::Kind::StringName;
            value.text = token.text;
            return true;
        case TokenType::Color:
            value = token.value;
            return true;
        case TokenType::Identifier:
            return parse_identifier(token.text, value);
        default:
            return fail("expected a value");
        }
    }

    // A tag, after its '[': its name and fields up to the ']'.
    bool parse_tag(Section& section, bool simple) {
        section.line = line_;
        if (simple) {
            std::string name;
            bool escaping = false;
            while (true) {
                if (pos_ >= text_.size()) {
                    return fail("the file ends inside a section name");
                }
                const char c = text_[pos_++];
                if (c == ']' && !escaping) {
                    break;
                }
                escaping = c == '\\';
                name += c;
            }
            const auto first = name.find_first_not_of(" \t");
            const auto last = name.find_last_not_of(" \t");
            section.tag = first == std::string::npos ? "" : name.substr(first, last - first + 1);
            return true;
        }
        Token token;
        if (!get_token(token)) {
            return false;
        }
        if (token.type != TokenType::Identifier) {
            return fail("expected a tag name");
        }
        section.tag = token.text;
        while (true) {
            if (!get_token(token)) {
                return false;
            }
            if (token.type == TokenType::BracketClose) {
                return true;
            }
            if (token.type == TokenType::Eof) {
                return fail("the file ends inside the tag '" + section.tag + "'");
            }
            if (token.type != TokenType::Identifier) {
                return fail("expected a field name in the tag '" + section.tag + "'");
            }
            std::string name = token.text;
            if (!get_token(token)) {
                return false;
            }
            if (token.type != TokenType::Equal) {
                return fail("expected '=' after '" + name + "'");
            }
            if (!get_token(token)) {
                return false;
            }
            Value value;
            if (!parse_value(token, value)) {
                return false;
            }
            section.fields.emplace_back(std::move(name), std::move(value));
        }
    }

    // Godot's parse_tag_assign_eof over a whole file: tags start sections, and "key = value"
    // lines add properties to the latest one.
    bool parse_sections(std::vector<Section>& sections, bool simple_tags) {
        std::string key;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == ';') {
                skip_line();
                continue;
            }
            if (c == '\n') {
                ++line_;
                continue;
            }
            if (c == '[' && key.empty()) {
                Section section;
                if (!parse_tag(section, simple_tags)) {
                    return false;
                }
                sections.push_back(std::move(section));
                continue;
            }
            if (static_cast<unsigned char>(c) <= 32) {
                continue;
            }
            if (c == '"') {
                if (!read_string(key)) {
                    return false;
                }
                continue;
            }
            if (c != '=') {
                key += c;
                continue;
            }
            Token token;
            if (!get_token(token)) {
                return false;
            }
            Value value;
            if (!parse_value(token, value)) {
                return false;
            }
            if (sections.empty()) {
                sections.emplace_back().line = 1;
            }
            sections.back().properties.emplace_back(std::move(key), std::move(value));
            key.clear();
        }
        if (!key.empty()) {
            return fail("expected '=' after '" + key + "'");
        }
        return true;
    }

private:
    void skip_line() {
        while (pos_ < text_.size() && text_[pos_] != '\n') {
            ++pos_;
        }
    }

    void read_number(std::string& word) {
        enum class Part : std::uint8_t { Int, Dec, Exp };
        Part part = Part::Int;
        bool exp_sign = false;
        bool exp_begun = false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (part == Part::Int) {
                if (c == '.') {
                    part = Part::Dec;
                } else if (c == 'e' || c == 'E') {
                    part = Part::Exp;
                } else if (!is_digit(c)) {
                    break;
                }
            } else if (part == Part::Dec) {
                if (c == 'e' || c == 'E') {
                    part = Part::Exp;
                } else if (!is_digit(c)) {
                    break;
                }
            } else if (is_digit(c)) {
                exp_begun = true;
            } else if ((c == '-' || c == '+') && !exp_sign && !exp_begun) {
                exp_sign = true;
            } else {
                break;
            }
            word += c;
            ++pos_;
        }
    }

    bool read_string(std::string& out) {
        out.clear();
        std::uint32_t lead = 0; // a UTF-16 lead surrogate waiting for its trail
        while (true) {
            if (pos_ >= text_.size()) {
                return fail("unterminated string");
            }
            const char c = text_[pos_++];
            if (c == '"') {
                break;
            }
            if (c != '\\') {
                if (lead != 0) {
                    return fail("unpaired UTF-16 surrogate in a string");
                }
                if (c == '\n') {
                    ++line_;
                }
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) {
                return fail("unterminated string");
            }
            const char next = text_[pos_++];
            std::uint32_t code = 0;
            switch (next) {
            case 'b':
                code = 8;
                break;
            case 't':
                code = 9;
                break;
            case 'n':
                code = 10;
                break;
            case 'f':
                code = 12;
                break;
            case 'r':
                code = 13;
                break;
            case 'u':
            case 'U': {
                const int length = next == 'U' ? 6 : 4;
                for (int i = 0; i < length; ++i) {
                    if (pos_ >= text_.size() || !is_hex(text_[pos_])) {
                        return fail("malformed hex constant in a string");
                    }
                    code = (code << 4U) | static_cast<std::uint32_t>(hex_value(text_[pos_++]));
                }
                break;
            }
            default:
                code = static_cast<unsigned char>(next);
                break;
            }
            if ((code & 0xFFFFFC00U) == 0xD800U) {
                if (lead != 0) {
                    return fail("unpaired UTF-16 surrogate in a string");
                }
                lead = code;
                continue;
            }
            if ((code & 0xFFFFFC00U) == 0xDC00U) {
                if (lead == 0) {
                    return fail("unpaired UTF-16 surrogate in a string");
                }
                code = ((lead - 0xD800U) << 10U) + (code - 0xDC00U) + 0x10000U;
                lead = 0;
            } else if (lead != 0) {
                return fail("unpaired UTF-16 surrogate in a string");
            }
            if (code < 0x80 && next != 'u' && next != 'U') {
                out += static_cast<char>(code);
            } else {
                append_utf8(out, code);
            }
        }
        if (lead != 0) {
            return fail("unpaired UTF-16 surrogate in a string");
        }
        return true;
    }

    bool number_value(const std::string& text, Value& value) {
        const bool is_float = text.find_first_of(".eE") != std::string::npos;
        char* end = nullptr;
        if (is_float) {
            value.kind = Value::Kind::Float;
            value.real = std::strtod(text.c_str(), &end);
        } else {
            value.kind = Value::Kind::Int;
            value.integer = std::strtoll(text.c_str(), &end, 10);
        }
        if (end != text.c_str() + text.size()) {
            return fail("malformed number " + text);
        }
        return true;
    }

    bool expect(TokenType type, std::string_view what) {
        Token token;
        if (!get_token(token)) {
            return false;
        }
        if (token.type != type) {
            return fail("expected " + std::string(what));
        }
        return true;
    }

    bool parse_identifier(const std::string& id, Value& value) {
        if (id == "true" || id == "false") {
            value.kind = Value::Kind::Bool;
            value.boolean = id == "true";
            return true;
        }
        if (id == "null" || id == "nil") {
            value.kind = Value::Kind::Nil;
            return true;
        }
        if (id == "inf" || id == "-inf" || id == "inf_neg" || id == "nan") {
            const double infinity = 1e300 * 1e300;
            double number = id == "inf" ? infinity : -infinity;
            if (id == "nan") {
                number = infinity - infinity;
            }
            value = make_float(number);
            return true;
        }
        Token token;
        if (!get_token(token)) {
            return false;
        }
        // Typed containers: Array[int]([1, 2]) and Dictionary[String, int]({"a": 1}).
        if ((id == "Array" || id == "Dictionary") && token.type == TokenType::BracketOpen) {
            int depth = 1;
            while (depth > 0) {
                if (!get_token(token)) {
                    return false;
                }
                if (token.type == TokenType::Eof) {
                    return fail("the file ends inside " + id + "'s type");
                }
                depth += token.type == TokenType::BracketOpen ? 1 : 0;
                depth -= token.type == TokenType::BracketClose ? 1 : 0;
            }
            if (!expect(TokenType::ParenOpen, "'(' after " + id + "'s type") || !get_token(token) ||
                !parse_value(token, value)) {
                return false;
            }
            return expect(TokenType::ParenClose, "')' after " + id + "'s contents");
        }
        if (token.type != TokenType::ParenOpen) {
            return fail("expected '(' after " + id);
        }
        value.kind = Value::Kind::Call;
        value.text = id;
        if (id == "Object") {
            return parse_object(value);
        }
        return parse_array(value, TokenType::ParenClose);
    }

    // Object(Type, "property": value, ...).
    bool parse_object(Value& value) {
        Token token;
        if (!get_token(token)) {
            return false;
        }
        if (token.type != TokenType::Identifier) {
            return fail("expected the Object's type");
        }
        Value type;
        type.kind = Value::Kind::String;
        type.text = token.text;
        value.items.push_back(std::move(type));
        while (true) {
            if (!get_token(token)) {
                return false;
            }
            if (token.type == TokenType::ParenClose) {
                return true;
            }
            if (token.type != TokenType::Comma) {
                return fail("expected ',' or ')' in Object");
            }
            if (!get_token(token)) {
                return false;
            }
            if (token.type != TokenType::String) {
                return fail("expected a property name in Object");
            }
            Value key;
            key.kind = Value::Kind::String;
            key.text = token.text;
            if (!expect(TokenType::Colon, "':' after an Object property")) {
                return false;
            }
            Value item;
            if (!get_token(token) || !parse_value(token, item)) {
                return false;
            }
            value.entries.push_back({std::move(key), std::move(item)});
        }
    }

    // Values separated by commas up to the closing token, into value.items.
    bool parse_array(Value& value, TokenType close) {
        if (value.kind != Value::Kind::Call) {
            value.kind = Value::Kind::Array;
        }
        bool need_comma = false;
        while (true) {
            Token token;
            if (!get_token(token)) {
                return false;
            }
            if (token.type == close) {
                return true;
            }
            if (token.type == TokenType::Eof) {
                return fail("the file ends inside a list");
            }
            if (need_comma) {
                if (token.type != TokenType::Comma) {
                    return fail("expected ','");
                }
                need_comma = false;
                continue;
            }
            Value item;
            if (!parse_value(token, item)) {
                return false;
            }
            value.items.push_back(std::move(item));
            need_comma = true;
        }
    }

    bool parse_dictionary(Value& value) {
        value.kind = Value::Kind::Dictionary;
        bool need_comma = false;
        while (true) {
            Token token;
            if (!get_token(token)) {
                return false;
            }
            if (token.type == TokenType::CurlyClose) {
                return true;
            }
            if (token.type == TokenType::Eof) {
                return fail("the file ends inside a dictionary");
            }
            if (need_comma) {
                if (token.type != TokenType::Comma) {
                    return fail("expected ',' or '}'");
                }
                need_comma = false;
                continue;
            }
            Value key;
            if (!parse_value(token, key)) {
                return false;
            }
            if (!expect(TokenType::Colon, "':' after a dictionary key")) {
                return false;
            }
            Value item;
            if (!get_token(token) || !parse_value(token, item)) {
                return false;
            }
            value.entries.push_back({std::move(key), std::move(item)});
            need_comma = true;
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    int line_ = 1;
    bool failed_ = false;
    Error error_;
};

Result<std::vector<Section>> parse_sections(std::string_view text, bool simple_tags) {
    Parser parser(text);
    std::vector<Section> sections;
    if (!parser.parse_sections(sections, simple_tags)) {
        return parser.error();
    }
    return sections;
}

} // namespace

Result<Value> parse_value(std::string_view text) {
    Parser parser(text);
    Token token;
    Value value;
    if (!parser.get_token(token) || !parser.parse_value(token, value)) {
        return parser.error();
    }
    if (!parser.get_token(token)) {
        return parser.error();
    }
    if (token.type != TokenType::Eof) {
        parser.fail("unexpected text after the value");
        return parser.error();
    }
    return value;
}

Result<std::vector<Section>> parse_resource(std::string_view text) {
    return parse_sections(text, false);
}

Result<std::vector<Section>> parse_project_settings(std::string_view text) {
    return parse_sections(text, true);
}

} // namespace slash::importer::godot
