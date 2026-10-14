#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <variant>

// Structured logging. Every line is one JSON object, so agents and tools can parse engine output
// without scraping text, e.g. {"level":"info","msg":"window created","width":1280}.
namespace slash::log {

enum class Level : std::uint8_t { Debug, Info, Warn, Error };

std::string_view to_string(Level level) noexcept;

class Field {
public:
    using Value = std::variant<std::string_view, std::int64_t, double, bool>;

    Field(std::string_view key, std::string_view value) : key_(key), value_(value) {}
    Field(std::string_view key, const char* value) : key_(key), value_(std::string_view(value)) {}
    Field(std::string_view key, const std::string& value)
        : key_(key), value_(std::string_view(value)) {}
    Field(std::string_view key, bool value) : key_(key), value_(value) {}

    template <std::integral T>
        requires(!std::same_as<T, bool>)
    Field(std::string_view key, T value) : key_(key), value_(static_cast<std::int64_t>(value)) {}

    template <std::floating_point T>
    Field(std::string_view key, T value) : key_(key), value_(static_cast<double>(value)) {}

    [[nodiscard]] std::string_view key() const noexcept { return key_; }
    [[nodiscard]] const Value& value() const noexcept { return value_; }

private:
    std::string_view key_;
    Value value_;
};

// Receives each formatted line (without a trailing newline). The default sink writes to stderr,
// which keeps stdout free for command results.
using Sink = std::function<void(Level level, std::string_view line)>;

void set_sink(Sink sink);
void reset_sink();
void set_min_level(Level level);
[[nodiscard]] Level min_level();

// Escapes text for use inside a JSON string literal (without the surrounding quotes).
[[nodiscard]] std::string escape_json(std::string_view text);

[[nodiscard]] std::string format_line(Level level, std::string_view message,
                                      std::initializer_list<Field> fields = {});

void write(Level level, std::string_view message, std::initializer_list<Field> fields = {});

inline void debug(std::string_view message, std::initializer_list<Field> fields = {}) {
    write(Level::Debug, message, fields);
}
inline void info(std::string_view message, std::initializer_list<Field> fields = {}) {
    write(Level::Info, message, fields);
}
inline void warn(std::string_view message, std::initializer_list<Field> fields = {}) {
    write(Level::Warn, message, fields);
}
inline void error(std::string_view message, std::initializer_list<Field> fields = {}) {
    write(Level::Error, message, fields);
}

} // namespace slash::log
