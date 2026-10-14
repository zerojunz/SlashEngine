#include "slash/core/log.hpp"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace slash::log {
namespace {

// Logs go to stderr, or to logcat on Android, where apps have no stderr to read.
void default_sink([[maybe_unused]] Level level, std::string_view line) {
#ifdef __ANDROID__
    int priority = ANDROID_LOG_INFO;
    switch (level) {
    case Level::Debug:
        priority = ANDROID_LOG_DEBUG;
        break;
    case Level::Info:
        break;
    case Level::Warn:
        priority = ANDROID_LOG_WARN;
        break;
    case Level::Error:
        priority = ANDROID_LOG_ERROR;
        break;
    }
    __android_log_write(priority, "SlashEngine", std::string(line).c_str());
#else
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
#endif
}

struct State {
    std::mutex mutex;
    Sink sink = default_sink;
    Level min_level = Level::Info;
};

State& state() {
    static State instance;
    return instance;
}

void append_value(std::string& out, const Field::Value& value) {
    if (const auto* text = std::get_if<std::string_view>(&value)) {
        out += '"';
        out += escape_json(*text);
        out += '"';
    } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        out += std::to_string(*integer);
    } else if (const auto* real = std::get_if<double>(&value)) {
        if (!std::isfinite(*real)) {
            out += "null"; // JSON has no NaN or infinity
            return;
        }
        char buffer[32];
        const int length = std::snprintf(buffer, sizeof(buffer), "%.17g", *real);
        out.append(buffer, static_cast<std::size_t>(length));
    } else if (const auto* flag = std::get_if<bool>(&value)) {
        out += *flag ? "true" : "false";
    }
}

} // namespace

std::string_view to_string(Level level) noexcept {
    switch (level) {
    case Level::Debug:
        return "debug";
    case Level::Info:
        return "info";
    case Level::Warn:
        return "warn";
    case Level::Error:
        return "error";
    }
    return "info";
}

void set_sink(Sink sink) {
    const std::scoped_lock lock(state().mutex);
    state().sink = sink ? std::move(sink) : Sink(default_sink);
}

void reset_sink() {
    set_sink(default_sink);
}

void set_min_level(Level level) {
    const std::scoped_lock lock(state().mutex);
    state().min_level = level;
}

Level min_level() {
    const std::scoped_lock lock(state().mutex);
    return state().min_level;
}

std::string escape_json(std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                const auto byte = static_cast<unsigned char>(c);
                out += "\\u00";
                out += kHex[byte >> 4U];
                out += kHex[byte & 0x0FU];
            } else {
                out += c;
            }
        }
    }
    return out;
}

std::string format_line(Level level, std::string_view message,
                        std::initializer_list<Field> fields) {
    std::string out = R"({"level":")";
    out += to_string(level);
    out += R"(","msg":")";
    out += escape_json(message);
    out += '"';
    for (const Field& field : fields) {
        out += ",\"";
        out += escape_json(field.key());
        out += "\":";
        append_value(out, field.value());
    }
    out += '}';
    return out;
}

void write(Level level, std::string_view message, std::initializer_list<Field> fields) {
    const std::scoped_lock lock(state().mutex);
    if (level < state().min_level) {
        return;
    }
    state().sink(level, format_line(level, message, fields));
}

} // namespace slash::log
