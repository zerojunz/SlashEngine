#include "slash/importer/unity_yaml.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>

namespace slash::importer::unity {

// --- Nodes -------------------------------------------------------------------------------------

const Node* Node::get(std::string_view key) const {
    if (kind != Kind::Map) {
        return nullptr;
    }
    for (const auto& [name, value] : entries) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

Node* Node::get(std::string_view key) {
    return const_cast<Node*>(std::as_const(*this).get(key));
}

const Node* Node::path(std::string_view dotted) const {
    const Node* node = this;
    std::size_t start = 0;
    while (node != nullptr && start <= dotted.size()) {
        const std::size_t end = std::min(dotted.find('.', start), dotted.size());
        const std::string_view part = dotted.substr(start, end - start);
        if (node->kind == Kind::Seq) {
            std::size_t index = 0;
            const auto [last, error] =
                std::from_chars(part.data(), part.data() + part.size(), index);
            node = error == std::errc() && last == part.data() + part.size() &&
                           index < node->items.size()
                       ? &node->items[index]
                       : nullptr;
        } else {
            node = node->get(part);
        }
        start = end + 1;
    }
    return node;
}

std::optional<double> Node::number() const {
    if (kind != Kind::Scalar || text.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::int64_t> Node::integer() const {
    if (kind != Kind::Scalar || text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error == std::errc() && end == text.data() + text.size()) {
        return value;
    }
    // Unsigned values past the signed range, such as layer masks of 4294967295 written as
    // 64-bit numbers, keep their bits.
    std::uint64_t bits = 0;
    const auto [unsigned_end, unsigned_error] =
        std::from_chars(text.data(), text.data() + text.size(), bits);
    if (unsigned_error == std::errc() && unsigned_end == text.data() + text.size()) {
        return static_cast<std::int64_t>(bits);
    }
    if (const auto real = number()) {
        return static_cast<std::int64_t>(*real);
    }
    return std::nullopt;
}

std::string_view Node::string() const {
    return kind == Kind::Scalar ? std::string_view(text) : std::string_view();
}

double Node::number_at(std::string_view dotted, double fallback) const {
    const Node* node = path(dotted);
    const auto value = node != nullptr ? node->number() : std::nullopt;
    return value.value_or(fallback);
}

std::int64_t Node::integer_at(std::string_view dotted, std::int64_t fallback) const {
    const Node* node = path(dotted);
    const auto value = node != nullptr ? node->integer() : std::nullopt;
    return value.value_or(fallback);
}

bool Node::flag_at(std::string_view dotted, bool fallback) const {
    const Node* node = path(dotted);
    if (node == nullptr || node->kind != Kind::Scalar) {
        return fallback;
    }
    if (node->text == "true") {
        return true;
    }
    if (node->text == "false") {
        return false;
    }
    const auto value = node->integer();
    return value ? *value != 0 : fallback;
}

std::string Node::string_at(std::string_view dotted) const {
    const Node* node = path(dotted);
    return node != nullptr ? std::string(node->string()) : std::string();
}

std::optional<ObjectRef> object_ref(const Node& node) {
    const Node* file_id = node.get("fileID");
    if (file_id == nullptr) {
        return std::nullopt;
    }
    ObjectRef ref;
    ref.file_id = file_id->integer().value_or(0);
    if (const Node* guid = node.get("guid")) {
        ref.guid = guid->string();
    }
    if (const Node* type = node.get("type")) {
        ref.type = static_cast<int>(type->integer().value_or(0));
    }
    return ref;
}

std::optional<ObjectRef> object_ref_at(const Node& node, std::string_view dotted) {
    const Node* value = node.path(dotted);
    return value != nullptr ? object_ref(*value) : std::nullopt;
}

namespace {

// --- Lines -------------------------------------------------------------------------------------

struct Line {
    int indent = 0;
    std::string_view content; // after the indentation, without trailing white space
    int number = 0;           // 1-based
};

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

Line make_line(std::string_view raw, int number) {
    Line line;
    line.number = number;
    std::size_t at = 0;
    while (at < raw.size() && raw[at] == ' ') {
        ++at;
    }
    line.indent = static_cast<int>(at);
    std::string_view content = raw.substr(at);
    while (!content.empty() && is_space(content.back())) {
        content.remove_suffix(1);
    }
    line.content = content;
    return line;
}

Error parse_error(int line, const std::string& message) {
    return Error{.code = ErrorCode::InvalidArgument,
                 .message = "line " + std::to_string(line) + ": " + message};
}

bool is_seq_line(const Line& line) {
    return line.content == "-" || line.content.starts_with("- ");
}

// Where a "key: value" line's key ends, or npos when the line isn't one.
std::size_t key_end(std::string_view content) {
    if (content.empty() || content.front() == '{' || content.front() == '[' ||
        content.front() == '\'' || content.front() == '"' || content == "-" ||
        content.starts_with("- ")) {
        return std::string_view::npos;
    }
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == ':' && (i + 1 == content.size() || content[i + 1] == ' ')) {
            return i;
        }
    }
    return std::string_view::npos;
}

// --- Flow collections --------------------------------------------------------------------------

class FlowParser {
public:
    FlowParser(std::string_view text, int line) : text_(text), line_(line) {}

    Result<Node> parse() {
        auto node = value(false);
        if (!node) {
            return node;
        }
        skip();
        if (at_ != text_.size()) {
            return parse_error(line_, "unexpected text after a flow collection");
        }
        return node;
    }

private:
    void skip() {
        while (at_ < text_.size() && is_space(text_[at_])) {
            ++at_;
        }
    }

    Result<Node> value(bool in_map_key) {
        skip();
        Node node;
        node.line = line_;
        if (at_ >= text_.size()) {
            return node;
        }
        const char c = text_[at_];
        if (c == '{') {
            return map();
        }
        if (c == '[') {
            return seq();
        }
        if (c == '\'' || c == '"') {
            auto text = quoted();
            if (!text) {
                return text.error();
            }
            node.text = std::move(text.value());
            return node;
        }
        const std::size_t start = at_;
        while (at_ < text_.size() && text_[at_] != ',' && text_[at_] != '}' && text_[at_] != ']' &&
               (!in_map_key || text_[at_] != ':')) {
            ++at_;
        }
        node.text = std::string(trim(text_.substr(start, at_ - start)));
        return node;
    }

    Result<std::string> quoted() {
        const char quote = text_[at_++];
        std::string out;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == quote) {
                if (quote == '\'' && at_ < text_.size() && text_[at_] == '\'') {
                    out += '\'';
                    ++at_;
                    continue;
                }
                return out;
            }
            if (quote == '"' && c == '\\' && at_ < text_.size()) {
                out += escape(text_[at_++]);
                continue;
            }
            out += c;
        }
        return parse_error(line_, "a quoted string with no end");
    }

    static char escape(char c) {
        switch (c) {
        case 'n':
            return '\n';
        case 't':
            return '\t';
        case 'r':
            return '\r';
        case '0':
            return '\0';
        default:
            return c;
        }
    }

    Result<Node> map() {
        Node node;
        node.kind = Node::Kind::Map;
        node.line = line_;
        ++at_; // '{'
        while (true) {
            skip();
            if (at_ >= text_.size()) {
                return parse_error(line_, "a flow mapping with no '}'");
            }
            if (text_[at_] == '}') {
                ++at_;
                return node;
            }
            auto key = value(true);
            if (!key) {
                return key;
            }
            skip();
            Node entry;
            entry.line = line_;
            if (at_ < text_.size() && text_[at_] == ':') {
                ++at_;
                auto parsed = value(false);
                if (!parsed) {
                    return parsed;
                }
                entry = std::move(parsed.value());
            }
            node.entries.push_back({std::move(key.value().text), std::move(entry)});
            skip();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
            } else if (at_ >= text_.size() || text_[at_] != '}') {
                return parse_error(line_, "expected ',' or '}' in a flow mapping");
            }
        }
    }

    Result<Node> seq() {
        Node node;
        node.kind = Node::Kind::Seq;
        node.line = line_;
        ++at_; // '['
        while (true) {
            skip();
            if (at_ >= text_.size()) {
                return parse_error(line_, "a flow sequence with no ']'");
            }
            if (text_[at_] == ']') {
                ++at_;
                return node;
            }
            auto item = value(false);
            if (!item) {
                return item;
            }
            node.items.push_back(std::move(item.value()));
            skip();
            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
            } else if (at_ >= text_.size() || text_[at_] != ']') {
                return parse_error(line_, "expected ',' or ']' in a flow sequence");
            }
        }
    }

    std::string_view text_;
    int line_ = 0;
    std::size_t at_ = 0;
};

// Whether a flow collection's text closes every bracket it opens, outside quotes.
bool balanced(std::string_view text) {
    int depth = 0;
    char quote = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != 0) {
            if (quote == '"' && c == '\\') {
                ++i;
            } else if (c == quote) {
                if (quote == '\'' && i + 1 < text.size() && text[i + 1] == '\'') {
                    ++i;
                } else {
                    quote = 0;
                }
            }
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
    return depth <= 0 && quote == 0;
}

// --- Block structure ---------------------------------------------------------------------------

class BlockParser {
public:
    explicit BlockParser(std::vector<Line> lines) : lines_(std::move(lines)) {}

    Result<Node> parse() {
        skip_blank();
        if (at_ >= lines_.size()) {
            return Node{};
        }
        auto node = parse_node(lines_[at_].indent);
        if (!node) {
            return node;
        }
        skip_blank();
        if (at_ < lines_.size()) {
            return parse_error(lines_[at_].number, "unexpected indentation");
        }
        return node;
    }

private:
    void skip_blank() {
        while (at_ < lines_.size() && lines_[at_].content.empty()) {
            ++at_;
        }
    }

    // The node starting at the current line, at the line's indentation.
    Result<Node> parse_node(int indent) {
        skip_blank();
        if (at_ >= lines_.size() || lines_[at_].indent < indent) {
            return Node{};
        }
        const Line& line = lines_[at_];
        if (is_seq_line(line)) {
            return parse_seq(line.indent);
        }
        if (key_end(line.content) != std::string_view::npos) {
            return parse_map(line.indent);
        }
        ++at_;
        return parse_inline(line.content, line.indent - 1, line.number);
    }

    Result<Node> parse_map(int indent) {
        Node map;
        map.kind = Node::Kind::Map;
        map.line = lines_[at_].number;
        while (true) {
            skip_blank();
            if (at_ >= lines_.size() || lines_[at_].indent != indent || is_seq_line(lines_[at_])) {
                if (at_ < lines_.size() && lines_[at_].indent > indent) {
                    return parse_error(lines_[at_].number, "unexpected indentation");
                }
                return map;
            }
            const Line line = lines_[at_];
            const std::size_t end = key_end(line.content);
            if (end == std::string_view::npos) {
                return parse_error(line.number, "expected \"key: value\"");
            }
            std::string key(trim(line.content.substr(0, end)));
            if (key.size() >= 2 && (key.front() == '\'' || key.front() == '"') &&
                key.back() == key.front()) {
                key = key.substr(1, key.size() - 2);
            }
            const std::string_view rest = trim(line.content.substr(end + 1));
            ++at_;
            Result<Node> value = Node{};
            if (rest.empty()) {
                skip_blank();
                if (at_ < lines_.size() && lines_[at_].indent > indent) {
                    value = parse_node(lines_[at_].indent);
                } else if (at_ < lines_.size() && lines_[at_].indent == indent &&
                           is_seq_line(lines_[at_])) {
                    value = parse_seq(indent); // Unity's "key:" then "- item" at its indent
                } else {
                    Node empty;
                    empty.line = line.number;
                    value = std::move(empty);
                }
            } else {
                value = parse_inline(rest, indent, line.number);
            }
            if (!value) {
                return value;
            }
            map.entries.push_back({std::move(key), std::move(value.value())});
        }
    }

    Result<Node> parse_seq(int indent) {
        Node seq;
        seq.kind = Node::Kind::Seq;
        seq.line = lines_[at_].number;
        while (true) {
            skip_blank();
            if (at_ >= lines_.size() || lines_[at_].indent != indent || !is_seq_line(lines_[at_])) {
                return seq;
            }
            Line& line = lines_[at_];
            std::size_t offset = 1;
            while (offset < line.content.size() && line.content[offset] == ' ') {
                ++offset;
            }
            const std::string_view rest = line.content.substr(offset);
            Result<Node> item = Node{};
            if (rest.empty()) {
                const int number = line.number;
                ++at_;
                skip_blank();
                if (at_ < lines_.size() && lines_[at_].indent > indent) {
                    item = parse_node(lines_[at_].indent);
                } else {
                    Node empty;
                    empty.line = number;
                    item = std::move(empty);
                }
            } else if (rest == "-" || rest.starts_with("- ") ||
                       key_end(rest) != std::string_view::npos) {
                // The item is a mapping or a sequence that starts on this line: read the line
                // as if the item's first entry stood alone at its column.
                line.indent = indent + static_cast<int>(offset);
                line.content = rest;
                item = parse_node(line.indent);
            } else {
                const int number = line.number;
                ++at_;
                item = parse_inline(rest, indent, number);
            }
            if (!item) {
                return item;
            }
            seq.items.push_back(std::move(item.value()));
        }
    }

    // A value written after "key: " or "- ": a flow collection, a quoted or plain scalar, which
    // may go on over the lines after it, indented past owner.
    Result<Node> parse_inline(std::string_view rest, int owner, int number) {
        const char first = rest.front();
        if (first == '{' || first == '[') {
            std::string text(rest);
            while (!balanced(text) && at_ < lines_.size()) {
                text += ' ';
                text += lines_[at_++].content;
            }
            return FlowParser(text, number).parse();
        }
        Node node;
        node.line = number;
        if (first == '\'' || first == '"') {
            auto text = quoted_scalar(rest, number);
            if (!text) {
                return text.error();
            }
            node.text = std::move(text.value());
            return node;
        }
        if (first == '|' || first == '>') {
            node.text = block_scalar(owner, first == '|');
            return node;
        }
        std::string text(rest);
        while (true) {
            std::size_t next = at_;
            int blanks = 0;
            while (next < lines_.size() && lines_[next].content.empty()) {
                ++next;
                ++blanks;
            }
            if (next >= lines_.size() || lines_[next].indent <= owner) {
                break;
            }
            text += blanks > 0 ? std::string(static_cast<std::size_t>(blanks), '\n') : " ";
            text += lines_[next].content;
            at_ = next + 1;
        }
        node.text = std::move(text);
        return node;
    }

    // A quoted scalar that may go on over the following lines: a line break folds to a space,
    // and each blank line in it to a newline.
    Result<std::string> quoted_scalar(std::string_view first, int number) {
        const char quote = first.front();
        std::string out;
        std::string_view line = first.substr(1);
        while (true) {
            bool escaped_break = false; // a '\\' ending the line joins it to the next
            for (std::size_t i = 0; i < line.size(); ++i) {
                const char c = line[i];
                if (c == quote) {
                    if (quote == '\'' && i + 1 < line.size() && line[i + 1] == '\'') {
                        out += '\'';
                        ++i;
                        continue;
                    }
                    if (!trim(line.substr(i + 1)).empty()) {
                        return parse_error(number, "unexpected text after a quoted string");
                    }
                    return out;
                }
                if (quote == '"' && c == '\\') {
                    if (i + 1 == line.size()) {
                        escaped_break = true;
                        break;
                    }
                    out += unescape(line, i);
                    continue;
                }
                out += c;
            }
            if (!escaped_break) {
                while (!out.empty() && is_space(out.back())) {
                    out.pop_back();
                }
            }
            std::size_t breaks = 0;
            while (at_ < lines_.size() && lines_[at_].content.empty()) {
                ++breaks;
                ++at_;
            }
            if (at_ >= lines_.size()) {
                return parse_error(number, "a quoted string with no end");
            }
            if (!escaped_break) {
                out += breaks > 0 ? std::string(breaks, '\n') : std::string(" ");
            }
            line = lines_[at_++].content;
        }
    }

    static std::string unescape(std::string_view line, std::size_t& i) {
        const char c = line[++i];
        const auto hex = [&](std::size_t digits) {
            std::uint32_t code = 0;
            const std::string_view part = line.substr(i + 1, digits);
            const auto [end, error] =
                std::from_chars(part.data(), part.data() + part.size(), code, 16);
            if (error != std::errc() || end != part.data() + part.size() || part.size() < digits) {
                return std::string(1, c);
            }
            i += digits;
            return utf8(code);
        };
        switch (c) {
        case 'n':
            return "\n";
        case 't':
            return "\t";
        case 'r':
            return "\r";
        case '0':
            return {"\0", 1};
        case 'x':
            return hex(2);
        case 'u':
            return hex(4);
        case 'U':
            return hex(8);
        default:
            return {&c, 1};
        }
    }

    static std::string utf8(std::uint32_t code) {
        std::string out;
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
        return out;
    }

    // A literal (|) or folded (>) block scalar: the lines after it indented past owner.
    std::string block_scalar(int owner, bool literal) {
        std::string out;
        int indent = -1;
        while (at_ < lines_.size() && (lines_[at_].content.empty() || lines_[at_].indent > owner)) {
            const Line& line = lines_[at_++];
            if (indent < 0 && !line.content.empty()) {
                indent = line.indent;
            }
            if (!out.empty()) {
                out += literal || line.content.empty() ? '\n' : ' ';
            }
            if (!line.content.empty()) {
                out +=
                    std::string(static_cast<std::size_t>(std::max(0, line.indent - indent)), ' ');
                out += line.content;
            }
        }
        while (!out.empty() && out.back() == '\n') {
            out.pop_back();
        }
        return out;
    }

    std::vector<Line> lines_;
    std::size_t at_ = 0;
};

std::vector<Line> split_lines(std::string_view text, int first_number) {
    std::vector<Line> lines;
    std::size_t start = 0;
    int number = first_number;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        lines.push_back(make_line(text.substr(start, end - start), number++));
        start = end + 1;
    }
    return lines;
}

// Reads "--- !u!<class> &<id>" and an optional "stripped".
std::optional<Object> document_header(std::string_view line, int number) {
    Object object;
    object.line = number;
    std::string_view rest = trim(line.substr(3));
    if (!rest.starts_with("!u!")) {
        return std::nullopt;
    }
    rest.remove_prefix(3);
    const auto [class_end, class_error] =
        std::from_chars(rest.data(), rest.data() + rest.size(), object.class_id);
    if (class_error != std::errc()) {
        return std::nullopt;
    }
    rest = trim(rest.substr(static_cast<std::size_t>(class_end - rest.data())));
    if (!rest.starts_with("&")) {
        return std::nullopt;
    }
    rest.remove_prefix(1);
    const auto [id_end, id_error] =
        std::from_chars(rest.data(), rest.data() + rest.size(), object.file_id);
    if (id_error != std::errc()) {
        return std::nullopt;
    }
    rest = trim(rest.substr(static_cast<std::size_t>(id_end - rest.data())));
    object.stripped = rest == "stripped";
    return object;
}

} // namespace

Result<std::vector<Object>> parse_file(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    std::vector<Object> objects;
    std::optional<Object> current;
    std::size_t body_start = 0;
    int body_line = 1;
    const auto finish = [&](std::size_t end) -> Result<void> {
        if (!current) {
            return {};
        }
        BlockParser parser(split_lines(text.substr(body_start, end - body_start), body_line));
        auto doc = parser.parse();
        if (!doc) {
            return doc.error();
        }
        if (!doc.value().is_map() || doc.value().entries.size() != 1) {
            return parse_error(current->line, "expected one object type under \"---\"");
        }
        current->type = std::move(doc.value().entries.front().key);
        current->body = std::move(doc.value().entries.front().value);
        objects.push_back(std::move(*current));
        current.reset();
        return {};
    };
    std::size_t start = 0;
    int number = 1;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view line = text.substr(start, end - start);
        if (line.starts_with("---")) {
            if (auto finished = finish(start); !finished) {
                return finished.error();
            }
            current = document_header(line, number);
            if (!current) {
                return parse_error(number, "expected \"--- !u!<class> &<file ID>\"");
            }
            body_start = end + 1;
            body_line = number + 1;
        } else if (!current && !line.starts_with("%") && !trim(line).empty()) {
            return parse_error(number, "text before the first \"---\"");
        }
        start = end + 1;
        ++number;
    }
    if (auto finished = finish(std::min(text.size(), start)); !finished) {
        return finished.error();
    }
    return objects;
}

Result<Node> parse_document(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    std::vector<Line> lines = split_lines(text, 1);
    std::erase_if(lines, [](const Line& line) {
        return line.indent == 0 && (line.content.starts_with("%") || line.content == "---");
    });
    return BlockParser(std::move(lines)).parse();
}

} // namespace slash::importer::unity
