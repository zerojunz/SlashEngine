#include "slash/core/json.hpp"

#include "slash/core/assert.hpp"
#include "slash/core/log.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <yyjson.h>

namespace slash::json {
namespace {

struct DocumentDeleter {
    void operator()(yyjson_doc* document) const { yyjson_doc_free(document); }
};

std::string describe_position(std::string_view text, std::size_t offset) {
    std::size_t line = 1;
    std::size_t column = 1;
    for (std::size_t i = 0; i < offset && i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return "line " + std::to_string(line) + ", column " + std::to_string(column);
}

std::string display_path(const std::string& path) {
    return path.empty() ? std::string("(root)") : path;
}

Result<Value> convert(yyjson_val* node, std::string& path) {
    switch (yyjson_get_type(node)) {
    case YYJSON_TYPE_BOOL:
        return Value(yyjson_get_bool(node));
    case YYJSON_TYPE_NUM:
        switch (yyjson_get_subtype(node)) {
        case YYJSON_SUBTYPE_UINT:
            return Value(yyjson_get_uint(node));
        case YYJSON_SUBTYPE_SINT:
            return Value(yyjson_get_sint(node));
        default:
            return Value(yyjson_get_real(node));
        }
    case YYJSON_TYPE_STR:
        return Value(std::string(yyjson_get_str(node), yyjson_get_len(node)));
    case YYJSON_TYPE_ARR: {
        Array items;
        items.reserve(yyjson_arr_size(node));
        std::size_t index = 0;
        std::size_t count = 0;
        yyjson_val* item = nullptr;
        yyjson_arr_foreach(node, index, count, item) {
            const std::size_t length = path.size();
            path += '/';
            path += std::to_string(index);
            auto converted = convert(item, path);
            path.resize(length);
            if (!converted) {
                return converted.error();
            }
            items.push_back(std::move(converted).value());
        }
        return Value(std::move(items));
    }
    case YYJSON_TYPE_OBJ: {
        Object members;
        std::size_t index = 0;
        std::size_t count = 0;
        yyjson_val* key = nullptr;
        yyjson_val* item = nullptr;
        yyjson_obj_foreach(node, index, count, key, item) {
            std::string name(yyjson_get_str(key), yyjson_get_len(key));
            if (members.contains(name)) {
                return make_error(ErrorCode::InvalidArgument, "duplicate key \"" + name +
                                                                  "\" in the object at " +
                                                                  display_path(path));
            }
            const std::size_t length = path.size();
            path += '/';
            path += pointer_escape(name);
            auto converted = convert(item, path);
            path.resize(length);
            if (!converted) {
                return converted.error();
            }
            members.emplace(std::move(name), std::move(converted).value());
        }
        return Value(std::move(members));
    }
    default:
        return Value();
    }
}

void write_number(std::string& out, std::int64_t number) {
    out += std::to_string(number);
}

void write_number(std::string& out, double number) {
    if (!std::isfinite(number)) {
        out += "null"; // JSON has no NaN or infinity
        return;
    }
    // yyjson writes the shortest text that reads back as the same double, independent of the C
    // locale, and keeps a ".0" on whole numbers so they stay recognizably real.
    yyjson_val node{};
    yyjson_set_double(&node, number);
    char buffer[40];
    const char* end = yyjson_write_number(&node, buffer);
    SLASH_ASSERT(end != nullptr, "yyjson failed to format a finite number");
    out.append(buffer, static_cast<std::size_t>(end - buffer));
}

void write_string(std::string& out, std::string_view text) {
    out += '"';
    out += log::escape_json(text);
    out += '"';
}

bool is_container(const Value& value) {
    return value.is_array() || value.is_object();
}

void indent(std::string& out, int depth) {
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
}

void write_value(std::string& out, const Value& value, Style style, int depth) {
    switch (value.type()) {
    case Type::Null:
        out += "null";
        return;
    case Type::Bool:
        out += value.as_bool() ? "true" : "false";
        return;
    case Type::Int:
        write_number(out, value.as_int());
        return;
    case Type::Double:
        write_number(out, value.as_double());
        return;
    case Type::String:
        write_string(out, value.as_string());
        return;
    case Type::Array: {
        const Array& items = value.as_array();
        if (items.empty()) {
            out += "[]";
            return;
        }
        // Pretty arrays stay on one line unless they hold objects or arrays.
        const bool multiline = style == Style::Pretty && std::ranges::any_of(items, is_container);
        out += '[';
        bool first = true;
        for (const Value& item : items) {
            if (!first) {
                out += ',';
            }
            if (multiline) {
                out += '\n';
                indent(out, depth + 1);
            } else if (!first && style == Style::Pretty) {
                out += ' ';
            }
            first = false;
            write_value(out, item, style, depth + 1);
        }
        if (multiline) {
            out += '\n';
            indent(out, depth);
        }
        out += ']';
        return;
    }
    case Type::Object: {
        const Object& members = value.as_object();
        if (members.empty()) {
            out += "{}";
            return;
        }
        const bool pretty = style == Style::Pretty;
        out += '{';
        bool first = true;
        for (const auto& [key, item] : members) {
            if (!first) {
                out += ',';
            }
            first = false;
            if (pretty) {
                out += '\n';
                indent(out, depth + 1);
            }
            write_string(out, key);
            out += pretty ? ": " : ":";
            write_value(out, item, style, depth + 1);
        }
        if (pretty) {
            out += '\n';
            indent(out, depth);
        }
        out += '}';
        return;
    }
    }
}

} // namespace

std::string_view to_string(Type type) noexcept {
    switch (type) {
    case Type::Null:
        return "null";
    case Type::Bool:
        return "boolean";
    case Type::Int:
        return "integer";
    case Type::Double:
        return "number";
    case Type::String:
        return "string";
    case Type::Array:
        return "array";
    case Type::Object:
        return "object";
    }
    return "null";
}

Value::Value(float value) : data_(static_cast<double>(value)) {
    if (!std::isfinite(value)) {
        return;
    }
    yyjson_val node{};
    yyjson_set_float(&node, value);
    char buffer[40];
    if (yyjson_write_number(&node, buffer) == nullptr) {
        return;
    }
    yyjson_val parsed{};
    if (yyjson_read_number(buffer, &parsed, YYJSON_READ_NOFLAG, nullptr, nullptr) != nullptr) {
        data_ = yyjson_get_num(&parsed);
    }
}

bool Value::is_integral() const noexcept {
    if (is_int()) {
        return true;
    }
    if (!is_double()) {
        return false;
    }
    const double number = *std::get_if<double>(&data_);
    return std::isfinite(number) && std::trunc(number) == number;
}

bool Value::as_bool() const {
    SLASH_ASSERT(is_bool(), "JSON value is not a boolean");
    return *std::get_if<bool>(&data_);
}

std::int64_t Value::as_int() const {
    SLASH_ASSERT(is_int(), "JSON value is not an integer");
    return *std::get_if<std::int64_t>(&data_);
}

double Value::as_double() const {
    if (const auto* integer = std::get_if<std::int64_t>(&data_)) {
        return static_cast<double>(*integer);
    }
    SLASH_ASSERT(is_double(), "JSON value is not a number");
    return *std::get_if<double>(&data_);
}

const std::string& Value::as_string() const {
    SLASH_ASSERT(is_string(), "JSON value is not a string");
    return *std::get_if<std::string>(&data_);
}

const Array& Value::as_array() const {
    SLASH_ASSERT(is_array(), "JSON value is not an array");
    return *std::get_if<Array>(&data_);
}

Array& Value::as_array() {
    SLASH_ASSERT(is_array(), "JSON value is not an array");
    return *std::get_if<Array>(&data_);
}

const Object& Value::as_object() const {
    SLASH_ASSERT(is_object(), "JSON value is not an object");
    return *std::get_if<Object>(&data_);
}

Object& Value::as_object() {
    SLASH_ASSERT(is_object(), "JSON value is not an object");
    return *std::get_if<Object>(&data_);
}

const Value* Value::find(std::string_view key) const {
    const auto* members = std::get_if<Object>(&data_);
    if (members == nullptr) {
        return nullptr;
    }
    const auto it = members->find(key);
    return it == members->end() ? nullptr : &it->second;
}

Value* Value::find(std::string_view key) {
    auto* members = std::get_if<Object>(&data_);
    if (members == nullptr) {
        return nullptr;
    }
    const auto it = members->find(key);
    return it == members->end() ? nullptr : &it->second;
}

Value& Value::operator[](std::string_view key) {
    if (is_null()) {
        data_ = Object{};
    }
    Object& members = as_object();
    auto it = members.find(key);
    if (it == members.end()) {
        it = members.emplace(std::string(key), Value()).first;
    }
    return it->second;
}

void Value::push_back(Value item) {
    if (is_null()) {
        data_ = Array{};
    }
    as_array().push_back(std::move(item));
}

std::size_t Value::size() const noexcept {
    if (const auto* items = std::get_if<Array>(&data_)) {
        return items->size();
    }
    if (const auto* members = std::get_if<Object>(&data_)) {
        return members->size();
    }
    return 0;
}

bool operator==(const Value& a, const Value& b) {
    if (a.is_number() && b.is_number()) {
        if (a.is_int() && b.is_int()) {
            return a.as_int() == b.as_int();
        }
        return a.as_double() == b.as_double();
    }
    return a.data_ == b.data_;
}

Result<Value> parse(std::string_view text) {
    yyjson_read_err error{};
    // yyjson only reads the buffer unless YYJSON_READ_INSITU is set, so the cast is safe.
    const std::unique_ptr<yyjson_doc, DocumentDeleter> document(yyjson_read_opts(
        const_cast<char*>(text.data()), text.size(), YYJSON_READ_NOFLAG, nullptr, &error));
    if (!document) {
        return make_error(ErrorCode::InvalidArgument,
                          describe_position(text, error.pos) + ": " + error.msg);
    }
    std::string path;
    return convert(yyjson_doc_get_root(document.get()), path);
}

std::string write(const Value& value, Style style) {
    std::string out;
    write_value(out, value, style, 0);
    return out;
}

std::string pointer_escape(std::string_view key) {
    std::string out;
    out.reserve(key.size());
    for (const char c : key) {
        if (c == '~') {
            out += "~0";
        } else if (c == '/') {
            out += "~1";
        } else {
            out += c;
        }
    }
    return out;
}

std::string pointer_first_key(std::string_view pointer) {
    if (!pointer.starts_with('/')) {
        return {};
    }
    pointer.remove_prefix(1);
    const std::string_view segment = pointer.substr(0, pointer.find('/'));
    std::string out;
    out.reserve(segment.size());
    for (std::size_t i = 0; i < segment.size(); ++i) {
        if (segment[i] == '~' && i + 1 < segment.size()) {
            out += segment[i + 1] == '1' ? '/' : '~';
            ++i;
        } else {
            out += segment[i];
        }
    }
    return out;
}

} // namespace slash::json
