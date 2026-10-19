#include "slash/exporter/xml.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace slash::exporter {
namespace {

// Deeper documents are refused rather than recursed into.
constexpr int kMaxDepth = 256;

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == ':' || c == '-' || c == '.' || static_cast<unsigned char>(c) >= 0x80;
}

std::string trimmed(std::string_view text) {
    const auto begin = std::ranges::find_if_not(text, space);
    const auto end = std::find_if_not(text.rbegin(), text.rend(), space).base();
    return begin < end ? std::string(begin, end) : std::string();
}

void append_utf8(std::uint32_t code, std::string& out) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0U | (code >> 6U));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0U | (code >> 12U));
        out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    } else {
        out += static_cast<char>(0xF0U | (code >> 18U));
        out += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
        out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    }
}

// A character reference's code point: "#65" or "#x41".
std::optional<std::uint32_t> character_reference(std::string_view entity) {
    const bool hex = entity.starts_with("#x");
    std::string_view digits = entity.substr(hex ? 2 : 1);
    if (digits.empty() || digits.size() > 8) {
        return std::nullopt;
    }
    std::uint32_t code = 0;
    for (const char c : digits) {
        std::uint32_t value = 0;
        if (c >= '0' && c <= '9') {
            value = static_cast<std::uint32_t>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
            value = static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (hex && c >= 'A' && c <= 'F') {
            value = static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            return std::nullopt;
        }
        code = code * (hex ? 16U : 10U) + value;
    }
    if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        return std::nullopt;
    }
    return code;
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Result<XmlElement> document() {
        if (text_.starts_with("\xEF\xBB\xBF")) {
            at_ = 3;
        }
        if (!skip_misc()) {
            return failed();
        }
        if (!starts_with("<")) {
            return fail("expected the root element");
        }
        XmlElement root;
        if (!element(root, 0) || !skip_misc()) {
            return failed();
        }
        if (at_ != text_.size()) {
            return fail("unexpected content after the root element");
        }
        return root;
    }

private:
    [[nodiscard]] bool starts_with(std::string_view prefix) const {
        return text_.substr(at_).starts_with(prefix);
    }

    void skip_space() {
        while (at_ < text_.size() && space(text_[at_])) {
            ++at_;
        }
    }

    [[nodiscard]] std::size_t line() const {
        return 1 + static_cast<std::size_t>(std::count(
                       text_.begin(), text_.begin() + static_cast<std::ptrdiff_t>(at_), '\n'));
    }

    Error fail(std::string message) {
        if (message_.empty()) {
            message_ = "line " + std::to_string(line()) + ": " + std::move(message);
        }
        return failed();
    }

    [[nodiscard]] Error failed() const { return make_error(ErrorCode::InvalidArgument, message_); }

    bool fail_bool(std::string message) {
        (void)fail(std::move(message));
        return false;
    }

    // Moves past end, which must come after the current position.
    bool skip_past(std::string_view end, std::string_view what) {
        const std::size_t found = text_.find(end, at_);
        if (found == std::string_view::npos) {
            return fail_bool("unterminated " + std::string(what));
        }
        at_ = found + end.size();
        return true;
    }

    // White space, comments, processing instructions and a DOCTYPE between elements.
    bool skip_misc() {
        while (true) {
            skip_space();
            if (starts_with("<!--")) {
                if (!skip_past("-->", "comment")) {
                    return false;
                }
            } else if (starts_with("<?")) {
                if (!skip_past("?>", "processing instruction")) {
                    return false;
                }
            } else if (starts_with("<!DOCTYPE")) {
                const std::size_t end = text_.find_first_of("[>", at_);
                if (end == std::string_view::npos || text_[end] == '[') {
                    return fail_bool("DOCTYPEs with an internal subset are not supported");
                }
                at_ = end + 1;
            } else {
                return true;
            }
        }
    }

    std::string name() {
        const std::size_t start = at_;
        while (at_ < text_.size() && name_char(text_[at_])) {
            ++at_;
        }
        return std::string(text_.substr(start, at_ - start));
    }

    // Character data at start with its entities replaced. An error is reported where it is.
    bool decode(std::size_t start, std::size_t end, std::string& out) {
        const std::string_view raw = text_.substr(start, end - start);
        const auto fail_at = [&](std::size_t offset, std::string message) {
            at_ = start + offset;
            return fail_bool(std::move(message));
        };
        std::size_t i = 0;
        while (i < raw.size()) {
            const std::size_t amp = raw.find('&', i);
            out.append(raw.substr(i, amp == std::string_view::npos ? raw.size() - i : amp - i));
            if (amp == std::string_view::npos) {
                break;
            }
            const std::size_t semicolon = raw.find(';', amp);
            if (semicolon == std::string_view::npos) {
                return fail_at(amp, "an '&' that starts no entity; write &amp;");
            }
            const std::string_view entity = raw.substr(amp + 1, semicolon - amp - 1);
            if (entity == "lt") {
                out += '<';
            } else if (entity == "gt") {
                out += '>';
            } else if (entity == "amp") {
                out += '&';
            } else if (entity == "quot") {
                out += '"';
            } else if (entity == "apos") {
                out += '\'';
            } else if (entity.starts_with('#')) {
                const std::optional<std::uint32_t> code = character_reference(entity);
                if (!code) {
                    return fail_at(amp, "bad character reference &" + std::string(entity) + ";");
                }
                append_utf8(*code, out);
            } else {
                return fail_at(amp, "unknown entity &" + std::string(entity) + ";");
            }
            i = semicolon + 1;
        }
        return true;
    }

    bool element(XmlElement& out, int depth) {
        if (depth > kMaxDepth) {
            return fail_bool("elements are nested more than " + std::to_string(kMaxDepth) +
                             " deep");
        }
        ++at_; // '<'
        out.name = name();
        if (out.name.empty()) {
            return fail_bool("expected an element name after '<'");
        }
        while (true) {
            skip_space();
            if (starts_with("/>")) {
                at_ += 2;
                return true;
            }
            if (starts_with(">")) {
                ++at_;
                break;
            }
            std::string attribute = name();
            if (attribute.empty()) {
                return fail_bool("expected an attribute, '>' or '/>' in <" + out.name + ">");
            }
            skip_space();
            if (!starts_with("=")) {
                return fail_bool("expected '=' after the attribute " + attribute);
            }
            ++at_;
            skip_space();
            if (at_ >= text_.size() || (text_[at_] != '"' && text_[at_] != '\'')) {
                return fail_bool("expected a quoted value for the attribute " + attribute);
            }
            const char quote = text_[at_++];
            const std::size_t end = text_.find(quote, at_);
            if (end == std::string_view::npos) {
                return fail_bool("unterminated value of the attribute " + attribute);
            }
            std::string value;
            if (!decode(at_, end, value)) {
                return false;
            }
            at_ = end + 1;
            if (out.attribute(attribute) != nullptr) {
                return fail_bool("<" + out.name + "> has the attribute " + attribute + " twice");
            }
            out.attributes.emplace_back(std::move(attribute), std::move(value));
        }
        std::string text;
        while (true) {
            if (at_ >= text_.size()) {
                return fail_bool("<" + out.name + "> is never closed");
            }
            if (starts_with("</")) {
                at_ += 2;
                const std::string closing = name();
                if (closing != out.name) {
                    return fail_bool("</" + closing + "> closes <" + out.name + ">");
                }
                skip_space();
                if (!starts_with(">")) {
                    return fail_bool("expected '>' after </" + closing);
                }
                ++at_;
                out.text = trimmed(text);
                return true;
            }
            if (starts_with("<!--")) {
                if (!skip_past("-->", "comment")) {
                    return false;
                }
            } else if (starts_with("<![CDATA[")) {
                at_ += 9;
                const std::size_t end = text_.find("]]>", at_);
                if (end == std::string_view::npos) {
                    return fail_bool("unterminated CDATA section");
                }
                text.append(text_.substr(at_, end - at_));
                at_ = end + 3;
            } else if (starts_with("<?")) {
                if (!skip_past("?>", "processing instruction")) {
                    return false;
                }
            } else if (starts_with("<")) {
                XmlElement child;
                if (!element(child, depth + 1)) {
                    return false;
                }
                out.children.push_back(std::move(child));
            } else {
                const std::size_t end = std::min(text_.find('<', at_), text_.size());
                if (!decode(at_, end, text)) {
                    return false;
                }
                at_ = end;
            }
        }
    }

    std::string_view text_;
    std::size_t at_ = 0;
    std::string message_;
};

void write_element(const XmlElement& element, int depth, std::string& out) {
    const std::string pad(static_cast<std::size_t>(depth) * 4, ' ');
    out += pad + "<" + element.name;
    for (const auto& [name, value] : element.attributes) {
        // Elements with several attributes put each on its own line, as Android Studio does.
        if (element.attributes.size() > 1) {
            out += "\n" + pad + "    ";
        } else {
            out += ' ';
        }
        out += name + "=\"" + xml_escaped(value) + "\"";
    }
    if (element.children.empty() && element.text.empty()) {
        out += " />\n";
        return;
    }
    out += '>';
    if (element.children.empty()) {
        out += xml_escaped(element.text) + "</" + element.name + ">\n";
        return;
    }
    out += '\n';
    if (!element.text.empty()) {
        out += pad + "    " + xml_escaped(element.text) + "\n";
    }
    for (const XmlElement& child : element.children) {
        write_element(child, depth + 1, out);
    }
    out += pad + "</" + element.name + ">\n";
}

} // namespace

const std::string* XmlElement::attribute(std::string_view attribute_name) const {
    const auto found =
        std::ranges::find(attributes, attribute_name, &std::pair<std::string, std::string>::first);
    return found == attributes.end() ? nullptr : &found->second;
}

void XmlElement::set_attribute(std::string_view attribute_name, std::string value) {
    const auto found =
        std::ranges::find(attributes, attribute_name, &std::pair<std::string, std::string>::first);
    if (found != attributes.end()) {
        found->second = std::move(value);
    } else {
        attributes.emplace_back(std::string(attribute_name), std::move(value));
    }
}

void XmlElement::remove_attribute(std::string_view attribute_name) {
    std::erase_if(attributes, [&](const std::pair<std::string, std::string>& attribute) {
        return attribute.first == attribute_name;
    });
}

const XmlElement* XmlElement::child(std::string_view child_name) const {
    const auto found = std::ranges::find(children, child_name, &XmlElement::name);
    return found == children.end() ? nullptr : &*found;
}

std::string XmlElement::child_text(std::string_view child_name) const {
    const XmlElement* found = child(child_name);
    return found == nullptr ? std::string() : found->text;
}

Result<XmlElement> parse_xml(std::string_view text) {
    return Parser(text).document();
}

std::string write_xml(const XmlElement& root) {
    std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    write_element(root, 0, out);
    return out;
}

std::string xml_escaped(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

} // namespace slash::exporter
