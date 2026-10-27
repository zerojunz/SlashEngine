#include "slash/l10n/translation.hpp"

#include "slash/l10n/locale.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::l10n {
namespace {

constexpr std::string_view kEnglishRules = "nplurals=2; plural=(n != 1);";

// Text without the characters up to a space at either end, as Godot's String::strip_edges.
std::string_view strip(std::string_view text) {
    while (!text.empty() && static_cast<unsigned char>(text.front()) <= 32) {
        text.remove_prefix(1);
    }
    while (!text.empty() && static_cast<unsigned char>(text.back()) <= 32) {
        text.remove_suffix(1);
    }
    return text;
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    std::size_t at = 0;
    while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
    }
    return text;
}

// C escapes read as what they stand for, one after another as Godot's String::c_unescape.
std::string c_unescape(std::string_view text) {
    std::string out(text);
    out = replace_all(std::move(out), "\\a", "\a");
    out = replace_all(std::move(out), "\\b", "\b");
    out = replace_all(std::move(out), "\\f", "\f");
    out = replace_all(std::move(out), "\\n", "\n");
    out = replace_all(std::move(out), "\\r", "\r");
    out = replace_all(std::move(out), "\\t", "\t");
    out = replace_all(std::move(out), "\\v", "\v");
    out = replace_all(std::move(out), "\\'", "'");
    out = replace_all(std::move(out), "\\\"", "\"");
    out = replace_all(std::move(out), "\\\\", "\\");
    return out;
}

std::string to_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// The text's lines, as Godot's FileAccess::get_line reads them one by one: split at '\n', with
// '\r' left out. The last is what follows the last '\n', reading which reaches the end.
std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    std::string line;
    for (const char c : text) {
        if (c == '\n') {
            lines.push_back(std::move(line));
            line.clear();
        } else if (c != '\r') {
            line += c;
        }
    }
    lines.push_back(std::move(line));
    return lines;
}

// Where the first c outside parentheses is, as PluralRules::_find_unquoted; parentheses don't
// nest.
std::size_t find_unquoted(std::string_view text, char c) {
    bool in_quote = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (in_quote) {
            in_quote = text[i] != ')';
        } else if (text[i] == '(') {
            in_quote = true;
        } else if (text[i] == c) {
            return i;
        }
    }
    return std::string_view::npos;
}

// The tests of a plural rule's expression, as PluralRules::_cache_plural_tests.
Result<std::shared_ptr<const PluralRules::Test>> plural_tests(std::string_view rule) {
    if (rule.starts_with('(') && rule.ends_with(')')) {
        int depth = 0;
        for (std::size_t i = 1; i + 1 < rule.size() && depth >= 0; ++i) {
            if (rule[i] == '(') {
                ++depth;
            } else if (rule[i] == ')') {
                --depth;
            }
        }
        if (depth == 0) {
            rule = rule.substr(1, rule.size() - 2);
        }
    }
    auto test = std::make_shared<PluralRules::Test>();
    const std::size_t question = find_unquoted(rule, '?');
    const std::size_t colon = find_unquoted(rule, ':');
    const std::string_view condition = strip(rule.substr(0, question));
    auto expression = Expression::parse(condition);
    if (!expression) {
        return make_error(ErrorCode::InvalidArgument, "can't parse \"" + std::string(condition) +
                                                          "\": " + expression.error().message);
    }
    test->expression = std::move(expression).value();
    if (question == std::string_view::npos) {
        return std::shared_ptr<const PluralRules::Test>(std::move(test));
    }
    if (colon == std::string_view::npos || colon < question) {
        return make_error(ErrorCode::InvalidArgument,
                          "\"" + std::string(rule) + "\" has a '?' with no ':' after it");
    }
    auto left = plural_tests(strip(rule.substr(question + 1, colon - question - 1)));
    if (!left) {
        return left.error();
    }
    auto right = plural_tests(strip(rule.substr(colon + 1)));
    if (!right) {
        return right.error();
    }
    test->left = std::move(left).value();
    test->right = std::move(right).value();
    return std::shared_ptr<const PluralRules::Test>(std::move(test));
}

int as_int(const json::Value& value) {
    if (value.is_bool()) {
        return value.as_bool() ? 1 : 0;
    }
    if (value.is_int()) {
        return static_cast<int>(value.as_int());
    }
    if (value.is_double()) {
        return static_cast<int>(value.as_double());
    }
    return 0;
}

// The form a test picks for n, as PluralRules::_eq_test: a test that holds goes on to its left
// side, one that doesn't to its right, and the last test's value is the form.
int run_tests(const PluralRules::Test* test, int n, int result) {
    while (test != nullptr) {
        auto value =
            test->expression.execute([n](std::string_view name) -> std::optional<json::Value> {
                if (name == "n") {
                    return json::Value(static_cast<std::int64_t>(n));
                }
                return std::nullopt;
            });
        if (!value) {
            return 0;
        }
        result = as_int(value.value());
        test = truthy(value.value()) ? test->left.get() : test->right.get();
    }
    return result;
}

} // namespace

Result<PluralRules> PluralRules::parse(std::string_view rules) {
    // In the form "nplurals=<N>; plural=<Expression>;".
    const std::size_t nplurals_eq = rules.find('=');
    if (nplurals_eq == std::string_view::npos) {
        return make_error(ErrorCode::InvalidArgument,
                          "invalid plural rules: no equal sign for nplurals");
    }
    const std::size_t nplurals_semicolon = rules.find(';', nplurals_eq);
    if (nplurals_semicolon == std::string_view::npos) {
        return make_error(ErrorCode::InvalidArgument,
                          "invalid plural rules: no semicolon after nplurals");
    }
    const std::string_view count =
        strip(rules.substr(nplurals_eq + 1, nplurals_semicolon - nplurals_eq - 1));
    int nplurals = 0;
    const auto [end, error] = std::from_chars(count.data(), count.data() + count.size(), nplurals);
    if (error != std::errc{} || end != count.data() + count.size()) {
        return make_error(ErrorCode::InvalidArgument,
                          "invalid plural rules: nplurals should be an integer");
    }
    if (nplurals < 1) {
        return make_error(ErrorCode::InvalidArgument,
                          "invalid plural rules: nplurals should be at least 1");
    }
    const std::size_t expression_eq = rules.find('=', nplurals_semicolon + 1);
    if (expression_eq == std::string_view::npos) {
        return make_error(ErrorCode::InvalidArgument,
                          "invalid plural rules: no equal sign for plural");
    }
    std::size_t expression_end = rules.rfind(';');
    if (expression_end == std::string_view::npos || expression_end < expression_eq) {
        // Godot warns and reads to the end.
        expression_end = rules.size();
    }
    const std::size_t expression_start = expression_eq + 1;
    if (expression_end <= expression_start) {
        return make_error(ErrorCode::InvalidArgument,
                          "invalid plural rules: the plural expression is empty");
    }
    PluralRules out;
    out.nplurals_ = nplurals;
    out.plural_ =
        std::string(strip(rules.substr(expression_start, expression_end - expression_start)));
    auto tests = plural_tests(out.plural_);
    if (!tests) {
        return make_error(ErrorCode::InvalidArgument,
                          "invalid plural rules: " + tests.error().message);
    }
    out.tests_ = std::move(tests).value();
    return out;
}

int PluralRules::evaluate(int n) const {
    return run_tests(tests_.get(), n, 0);
}

void Translation::set_locale(std::string_view locale) {
    locale_ = standardize_locale(locale);
    if (plural_override_.empty()) {
        rules_ = nullptr;
    }
}

void Translation::set_plural_rules_override(std::string_view rules) {
    plural_override_ = rules;
    rules_ = nullptr;
}

const PluralRules& Translation::plural_rules() const {
    if (rules_ == nullptr && !plural_override_.empty()) {
        if (auto parsed = PluralRules::parse(plural_override_)) {
            rules_ = std::make_shared<const PluralRules>(std::move(parsed).value());
        }
    }
    if (rules_ == nullptr) {
        // The locale's own rules, else English's.
        if (const std::string rule = plural_rules_for(locale_); !rule.empty()) {
            if (auto parsed = PluralRules::parse(rule)) {
                rules_ = std::make_shared<const PluralRules>(std::move(parsed).value());
            }
        }
        if (rules_ == nullptr) {
            rules_ = std::make_shared<const PluralRules>(PluralRules::parse(kEnglishRules).value());
        }
    }
    return *rules_;
}

void Translation::add_message(std::string_view message, std::string_view translated,
                              std::string_view context) {
    messages_.insert_or_assign({std::string(context), std::string(message)},
                               std::vector<std::string>{std::string(translated)});
}

void Translation::add_plural_message(std::string_view message, std::vector<std::string> forms,
                                     std::string_view context) {
    if (forms.empty()) {
        return;
    }
    messages_.insert_or_assign({std::string(context), std::string(message)}, std::move(forms));
}

const std::string* Translation::message(std::string_view message, std::string_view context) const {
    const auto found = messages_.find(std::pair{context, message});
    return found != messages_.end() ? found->second.data() : nullptr;
}

const std::string* Translation::plural_message(std::string_view message, int n,
                                               std::string_view context) const {
    if (n < 0) {
        return nullptr;
    }
    const auto found = messages_.find(std::pair{context, message});
    if (found == messages_.end()) {
        return nullptr;
    }
    const int index = plural_rules().evaluate(n);
    if (index < 0 || std::cmp_greater_equal(index, found->second.size())) {
        return nullptr;
    }
    return &found->second[static_cast<std::size_t>(index)];
}

namespace {

// Reads the header's Plural-Forms line into the translation, and the locale its Language or
// X-Language names.
void read_header(std::string_view header, Translation& translation) {
    for (std::size_t start = 0; start <= header.size();) {
        std::size_t end = header.find('\n', start);
        if (end == std::string_view::npos) {
            end = header.size();
        }
        const std::string_view line = strip(header.substr(start, end - start));
        start = end + 1;
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string_view property = strip(line.substr(0, colon));
        const std::string_view value = strip(line.substr(colon + 1));
        if (property == "X-Language" || property == "Language") {
            translation.set_locale(value);
        }
    }
}

// Records the rule a header's Plural-Forms line gives, as Godot does: from "Plural-Forms" to
// the end of its line.
void read_plural_forms(std::string_view header, Translation& translation) {
    const std::size_t start = header.find("Plural-Forms");
    if (start != std::string_view::npos) {
        translation.set_plural_rules_override(
            header.substr(start, header.find('\n', start) - start));
    }
}

std::string line_error(int line, std::string_view message) {
    return "line " + std::to_string(line) + ": " + std::string(message);
}

} // namespace

Result<TranslationFile> read_po(std::string_view text) {
    enum class Status : std::uint8_t {
        None,
        ReadingId,
        ReadingString,
        ReadingContext,
        ReadingPlural
    };
    Translation translation;
    std::string config;
    Status status = Status::None;
    std::string msg_id;
    std::string msg_str;
    std::string msg_context;
    std::vector<std::string> msgs_plural;
    int line = 1;
    int plural_forms = 0;
    int plural_index = -1;
    bool entered_context = false;
    bool skip_this = false;
    bool skip_next = false;
    const auto fail = [&line](std::string_view message) {
        return make_error(ErrorCode::InvalidArgument, line_error(line, message));
    };
    const auto plurals_complete = [&] { return plural_index == plural_forms - 1; };

    const std::vector<std::string> lines = lines_of(text);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string_view l = strip(lines[i]);
        const bool is_eof = i + 1 == lines.size();
        // The last line ends the file unless it has content.
        if (is_eof && l.empty()) {
            if (status == Status::ReadingId || status == Status::ReadingContext ||
                (status == Status::ReadingPlural && !plurals_complete())) {
                return fail("the file ends in the middle of a message");
            }
            break;
        }
        if (l.starts_with("msgctxt")) {
            if (status != Status::ReadingString && status != Status::ReadingPlural) {
                return fail("unexpected 'msgctxt', expected 'msgid_plural' or 'msgstr' before it");
            }
            // msgctxt comes before msgid, so what was read so far is added now, once.
            if (!skip_this && !msg_id.empty()) {
                if (status == Status::ReadingString) {
                    translation.add_message(msg_id, msg_str, msg_context);
                } else {
                    if (!plurals_complete()) {
                        return fail("the number of 'msgstr[]' doesn't match the plural forms");
                    }
                    translation.add_plural_message(msg_id, msgs_plural, msg_context);
                }
            }
            msg_context.clear();
            l = strip(l.substr(7));
            status = Status::ReadingContext;
            entered_context = true;
        }
        if (l.starts_with("msgid_plural")) {
            if (plural_forms == 0) {
                return fail("'msgid_plural' needs a valid 'Plural-Forms' in the header");
            }
            if (status != Status::ReadingId) {
                return fail("unexpected 'msgid_plural', expected 'msgid' before it");
            }
            // tr_n gives the plural source text, so only the msgstr[] forms are kept.
            l = strip(l.substr(12));
            plural_index = -1;
            msgs_plural.assign(static_cast<std::size_t>(plural_forms), std::string());
            status = Status::ReadingPlural;
        } else if (l.starts_with("msgid")) {
            if (status == Status::ReadingId) {
                return fail("unexpected 'msgid', expected 'msgstr'");
            }
            if (!msg_id.empty()) {
                if (!skip_this && !entered_context) {
                    if (status == Status::ReadingString) {
                        translation.add_message(msg_id, msg_str, msg_context);
                    } else if (status == Status::ReadingPlural) {
                        if (!plurals_complete()) {
                            return fail("the number of 'msgstr[]' doesn't match the plural forms");
                        }
                        translation.add_plural_message(msg_id, msgs_plural, msg_context);
                    }
                }
            } else if (config.empty()) {
                config = msg_str;
                if (config.find("Plural-Forms") != std::string::npos) {
                    read_plural_forms(config, translation);
                    plural_forms = translation.plural_rules().nplurals();
                }
            }
            l = strip(l.substr(5));
            status = Status::ReadingId;
            // Without a msgctxt, the message has no context.
            if (!entered_context) {
                msg_context.clear();
            }
            msg_id.clear();
            msg_str.clear();
            skip_this = skip_next;
            skip_next = false;
            entered_context = false;
        }
        if (l.starts_with("msgstr[")) {
            if (status != Status::ReadingPlural) {
                return fail("unexpected 'msgstr[]', expected 'msgid_plural' before it");
            }
            ++plural_index;
            l = strip(l.substr(std::min<std::size_t>(9, l.size())));
        } else if (l.starts_with("msgstr")) {
            if (status != Status::ReadingId) {
                return fail("unexpected 'msgstr', expected 'msgid' before it");
            }
            l = strip(l.substr(6));
            status = Status::ReadingString;
        }
        if (l.empty() || l.starts_with('#')) {
            if (l.find("fuzzy") != std::string_view::npos) {
                skip_next = true;
            }
            ++line;
            continue; // Nothing to read, or a comment.
        }
        if (!l.starts_with('"') || status == Status::None) {
            return fail("invalid line '" + std::string(l) + "'");
        }
        l.remove_prefix(1);
        // The closing quote, past escaped ones (\"), and escaped backslashes (\\").
        std::size_t end = std::string_view::npos;
        bool escape_next = false;
        for (std::size_t k = 0; k < l.size(); ++k) {
            if (l[k] == '\\' && !escape_next) {
                escape_next = true;
                continue;
            }
            if (l[k] == '"' && !escape_next) {
                end = k;
                break;
            }
            escape_next = false;
        }
        if (end == std::string_view::npos) {
            return fail("expected '\"' at the end of the message");
        }
        const std::string part = c_unescape(l.substr(0, end));
        if (status == Status::ReadingId) {
            msg_id += part;
        } else if (status == Status::ReadingString) {
            msg_str += part;
        } else if (status == Status::ReadingContext) {
            msg_context += part;
        } else if (status == Status::ReadingPlural && plural_index >= 0) {
            if (plural_index >= plural_forms) {
                return fail("more 'msgstr[]' than the plural forms");
            }
            msgs_plural[static_cast<std::size_t>(plural_index)] += part;
        }
        ++line;
    }
    // The last message.
    if (status == Status::ReadingString) {
        if (!msg_id.empty()) {
            if (!skip_this) {
                translation.add_message(msg_id, msg_str, msg_context);
            }
        } else if (config.empty()) {
            config = msg_str;
        }
    } else if (status == Status::ReadingPlural) {
        if (!skip_this && !msg_id.empty()) {
            if (!plurals_complete()) {
                return fail("the number of 'msgstr[]' doesn't match the plural forms");
            }
            translation.add_plural_message(msg_id, msgs_plural, msg_context);
        }
    }
    if (config.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "no header (the msgid \"\" entry with Language and Plural-Forms)");
    }
    read_header(config, translation);
    TranslationFile out;
    out.translations.push_back(std::move(translation));
    return out;
}

Result<TranslationFile> read_mo(std::string_view bytes) {
    const auto u32 = [bytes](std::size_t at) -> std::optional<std::uint32_t> {
        if (at + 4 > bytes.size()) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + i]))
                     << (8 * i);
        }
        return value;
    };
    const auto truncated = [] {
        return make_error(ErrorCode::InvalidArgument, "the .mo file is cut short");
    };
    if (u32(0) != 0x950412deU) {
        return make_error(ErrorCode::InvalidArgument, "not a little-endian .mo file");
    }
    const std::optional<std::uint32_t> version = u32(4);
    const std::optional<std::uint32_t> count = u32(8);
    const std::optional<std::uint32_t> ids = u32(12);
    const std::optional<std::uint32_t> translated = u32(16);
    if (!version || !count || !ids || !translated) {
        return truncated();
    }
    if ((*version & 0xFFFFU) > 1) {
        return make_error(ErrorCode::InvalidArgument, "unsupported .mo version " +
                                                          std::to_string(*version & 0xFFFFU) + "." +
                                                          std::to_string(*version >> 16U));
    }
    // The string an entry of a table points to.
    const auto string_at = [&](std::uint32_t table,
                               std::uint32_t index) -> std::optional<std::string_view> {
        const std::optional<std::uint32_t> length =
            u32(std::size_t{table} + (std::size_t{index} * 8));
        const std::optional<std::uint32_t> offset =
            u32(std::size_t{table} + (std::size_t{index} * 8) + 4);
        if (!length || !offset || std::size_t{*offset} + *length > bytes.size()) {
            return std::nullopt;
        }
        return bytes.substr(*offset, *length);
    };
    Translation translation;
    std::string config;
    for (std::uint32_t i = 0; i < *count; ++i) {
        const std::optional<std::string_view> id = string_at(*ids, i);
        const std::optional<std::string_view> text = string_at(*translated, i);
        if (!id || !text) {
            return truncated();
        }
        // An id is "context\x04message\0plural", each part but the message optional.
        std::string_view message = *id;
        std::string_view context;
        if (const std::size_t eot = message.find('\x04'); eot != std::string_view::npos) {
            context = message.substr(0, eot);
            message.remove_prefix(eot + 1);
        }
        std::string_view plural;
        if (const std::size_t nul = message.find('\0'); nul != std::string_view::npos) {
            plural = message.substr(nul + 1);
            message = message.substr(0, nul);
        }
        if (message.empty()) {
            config = *text;
            if (config.find("Plural-Forms") != std::string::npos) {
                read_plural_forms(config, translation);
            }
            continue;
        }
        if (plural.empty()) {
            translation.add_message(message, text->substr(0, text->find('\0')), context);
            continue;
        }
        std::vector<std::string> forms;
        std::string_view rest = *text;
        while (true) {
            const std::size_t nul = rest.find('\0');
            forms.emplace_back(rest.substr(0, nul));
            if (nul == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(nul + 1);
        }
        translation.add_plural_message(message, std::move(forms), context);
    }
    if (config.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "no header (the entry with Language and Plural-Forms)");
    }
    read_header(config, translation);
    TranslationFile out;
    out.translations.push_back(std::move(translation));
    return out;
}

namespace {

// The next record of a CSV file from line, as Godot's FileAccess::get_csv_line: lines are joined
// while a quote is open, a delimiter outside quotes ends a field and "" inside quotes is a quote.
std::vector<std::string> csv_line(const std::vector<std::string>& lines, std::size_t& next,
                                  char delimiter, bool& unclosed) {
    std::string line;
    std::size_t quotes = 0;
    do {
        if (next >= lines.size()) {
            break;
        }
        line += lines[next++];
        line += '\n';
        quotes = static_cast<std::size_t>(std::ranges::count(line, '"'));
    } while (quotes % 2 != 0);
    if (!line.empty()) {
        line.pop_back(); // the '\n' added
    }
    std::vector<std::string> fields;
    bool in_quote = false;
    std::string current;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (!in_quote && c == delimiter) {
            fields.push_back(std::move(current));
            current.clear();
        } else if (c == '"') {
            if (in_quote && i + 1 < line.size() && line[i + 1] == '"') {
                current += '"';
                ++i;
            } else {
                in_quote = !in_quote;
            }
        } else {
            current += c;
        }
    }
    unclosed = unclosed || in_quote;
    fields.push_back(std::move(current));
    return fields;
}

} // namespace

Result<TranslationFile> read_csv(std::string_view text, const CsvOptions& options) {
    if (options.delimiter == '"') {
        return make_error(ErrorCode::InvalidArgument, "'\"' can't be a CSV file's delimiter");
    }
    const std::vector<std::string> lines = lines_of(text);
    std::size_t next = 0;
    bool unclosed = false;
    TranslationFile out;
    // Translations by the column they come from.
    std::vector<std::pair<std::size_t, Translation>> columns;
    std::optional<std::size_t> context_column;
    std::optional<std::size_t> plural_column;
    {
        const std::vector<std::string> header = csv_line(lines, next, options.delimiter, unclosed);
        for (std::size_t i = 1; i < header.size(); ++i) {
            if (header[i].starts_with('_')) {
                continue;
            }
            const std::string name = to_lower(header[i]);
            if (name == "?context" || name == "?plural") {
                std::optional<std::size_t>& column =
                    name == "?context" ? context_column : plural_column;
                if (column) {
                    out.problems.push_back({1, "more than one " + name +
                                                   " column; the ones after the first are "
                                                   "left out"});
                } else {
                    column = i;
                }
                continue;
            }
            const std::string locale = standardize_locale(header[i]);
            if (!is_locale(locale)) {
                out.problems.push_back(
                    {1, "column " + std::to_string(i + 1) + " names no locale; it is left out"});
                continue;
            }
            Translation translation;
            translation.set_locale(locale);
            columns.emplace_back(i, std::move(translation));
        }
        if (columns.empty()) {
            out.problems.push_back({1, "the header names no locale, so nothing is translated"});
            return out;
        }
    }
    const auto column_of = [&columns](std::size_t i) -> Translation* {
        const auto found =
            std::ranges::find(columns, i, &std::pair<std::size_t, Translation>::first);
        return found != columns.end() ? &found->second : nullptr;
    };

    bool reading_plurals = false;
    std::string plural_id;
    std::string plural_context;
    std::vector<std::pair<std::size_t, std::vector<std::string>>> plural_forms;
    const auto add_plurals = [&] {
        for (auto& [column, translation] : columns) {
            const auto forms = std::ranges::find(
                plural_forms, column, &std::pair<std::size_t, std::vector<std::string>>::first);
            if (forms != plural_forms.end() && !forms->second.empty()) {
                translation.add_plural_message(plural_id, forms->second, plural_context);
            }
        }
        plural_forms.clear();
    };
    while (next < lines.size()) {
        const int row = static_cast<int>(next) + 1;
        const std::vector<std::string> line = csv_line(lines, next, options.delimiter, unclosed);
        // Skip empty lines.
        if (line.size() == 1 && line[0].empty()) {
            continue;
        }
        if (to_lower(line[0]) == "?pluralrule") {
            for (std::size_t i = 1; i < line.size(); ++i) {
                Translation* translation = column_of(i);
                if (line[i].empty() || translation == nullptr) {
                    continue;
                }
                if (!translation->plural_rules_override().empty()) {
                    out.problems.push_back({row, "more than one ?pluralrule for " +
                                                     translation->locale() +
                                                     "; the ones after the first are left out"});
                    continue;
                }
                translation->set_plural_rules_override(line[i]);
            }
            continue;
        }
        const std::string id = options.unescape_keys ? c_unescape(line[0]) : line[0];
        if (!reading_plurals && id.empty()) {
            continue;
        }
        // Context and plural columns may go unused.
        const std::string context =
            context_column && *context_column < line.size() ? line[*context_column] : "";
        const std::string id_plural =
            plural_column && *plural_column < line.size() ? line[*plural_column] : "";
        // The end of a plural message's rows.
        if (reading_plurals && (!id.empty() || !context.empty() || !id_plural.empty())) {
            reading_plurals = false;
            add_plurals();
        }
        // The start of one.
        if (!reading_plurals && !id_plural.empty()) {
            reading_plurals = true;
            plural_id = id;
            plural_context = context;
        }
        for (std::size_t i = 1; i < line.size(); ++i) {
            Translation* translation = column_of(i);
            if (translation == nullptr) {
                continue;
            }
            std::string translated = options.unescape_translations ? c_unescape(line[i]) : line[i];
            if (translated.empty()) {
                continue;
            }
            if (reading_plurals) {
                auto forms = std::ranges::find(
                    plural_forms, i, &std::pair<std::size_t, std::vector<std::string>>::first);
                if (forms == plural_forms.end()) {
                    forms = plural_forms.insert(plural_forms.end(), {i, {}});
                }
                forms->second.push_back(std::move(translated));
            } else {
                translation->add_message(id, translated, context);
            }
        }
    }
    if (reading_plurals) {
        add_plurals();
    }
    if (unclosed) {
        out.problems.push_back({0, "the file ends before a '\"' closes"});
    }
    for (auto& [column, translation] : columns) {
        out.translations.push_back(std::move(translation));
    }
    return out;
}

Result<TranslationFile> read_translation_file(std::string_view path, std::string_view text) {
    if (path.ends_with(".po")) {
        return read_po(text);
    }
    if (path.ends_with(".mo")) {
        return read_mo(text);
    }
    return read_csv(text);
}

namespace {

bool is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

std::string joined(const std::vector<std::string>& parts) {
    if (parts.empty()) {
        return "none";
    }
    std::string out;
    for (const std::string& part : parts) {
        out += out.empty() ? "" : " ";
        out += part;
    }
    return out;
}

} // namespace

Placeholders placeholders_of(std::string_view text) {
    Placeholders out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%') {
            std::size_t at = i + 1;
            if (at < text.size() && text[at] == '%') {
                i = at; // "%%" is a percent sign
                continue;
            }
            // Godot's modifiers: flags, a width or "*", and a precision.
            while (at < text.size() &&
                   std::string_view("+-0123456789.* ").find(text[at]) != std::string_view::npos) {
                ++at;
            }
            if (at < text.size() &&
                std::string_view("sdioxXbcfv").find(text[at]) != std::string_view::npos) {
                out.conversions.push_back(std::string("%") + text[at]);
                i = at;
            }
        } else if (text[i] == '{') {
            std::size_t at = i + 1;
            while (at < text.size() &&
                   (is_letter(text[at]) || is_digit(text[at]) || text[at] == '_')) {
                ++at;
            }
            if (at > i + 1 && at < text.size() && text[at] == '}') {
                out.names.insert(std::string(text.substr(i, at - i + 1)));
                i = at;
            }
        }
    }
    return out;
}

std::string placeholder_problem(std::string_view source, std::string_view translated) {
    const Placeholders want = placeholders_of(source);
    const Placeholders got = placeholders_of(translated);
    if (want.conversions != got.conversions) {
        return "has " + joined(got.conversions) + " where the source has " +
               joined(want.conversions);
    }
    for (const std::string& name : got.names) {
        if (!want.names.contains(name)) {
            return "has " + name + ", which the source doesn't";
        }
    }
    return {};
}

} // namespace slash::l10n
