#include "slash/core/json_schema.hpp"

#include "slash/core/text.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

namespace slash::json {
namespace {

constexpr int kMaxRefDepth = 32;

std::string_view type_name(const Value& value) {
    return to_string(value.type());
}

bool matches_type(const Value& value, std::string_view type) {
    if (type == "null") {
        return value.is_null();
    }
    if (type == "boolean") {
        return value.is_bool();
    }
    if (type == "integer") {
        return value.is_integral();
    }
    if (type == "number") {
        return value.is_number();
    }
    if (type == "string") {
        return value.is_string();
    }
    if (type == "array") {
        return value.is_array();
    }
    if (type == "object") {
        return value.is_object();
    }
    return false;
}

std::size_t code_points(std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(
        text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}

std::vector<std::string_view> property_names(const Object& properties) {
    std::vector<std::string_view> names;
    names.reserve(properties.size());
    for (const auto& [name, unused] : properties) {
        names.emplace_back(name);
    }
    return names;
}

std::string text_of(const Value& value) {
    return write(value, Style::Compact);
}

std::string unescape_pointer_segment(std::string_view segment) {
    std::string out;
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

class Validator {
public:
    Validator(const Value& root, std::vector<ValidationError>& errors)
        : root_(root), errors_(&errors) {}

    void check(const Value& instance, const Value& schema, std::string& path, int ref_depth) {
        if (schema.is_bool()) {
            if (!schema.as_bool()) {
                fail(path, "no value is allowed here");
            }
            return;
        }
        if (!schema.is_object()) {
            return;
        }

        if (const Value* ref = schema.find("$ref"); ref != nullptr && ref->is_string()) {
            check_ref(instance, ref->as_string(), path, ref_depth);
        }
        if (!check_type(instance, schema, path)) {
            return; // the remaining keywords would only repeat the type error
        }
        check_enum_and_const(instance, schema, path);
        check_combinators(instance, schema, path, ref_depth);

        if (instance.is_object()) {
            check_object(instance, schema, path, ref_depth);
        } else if (instance.is_array()) {
            check_array(instance, schema, path, ref_depth);
        } else if (instance.is_number()) {
            check_number(instance, schema, path);
        } else if (instance.is_string()) {
            check_string(instance, schema, path);
        }
    }

private:
    void fail(const std::string& path, std::string message) {
        errors_->push_back({path, std::move(message)});
    }

    // Validates against a subschema without recording errors; true if it matched.
    bool passes(const Value& instance, const Value& schema, std::string& path, int ref_depth) {
        std::vector<ValidationError> scratch;
        std::vector<ValidationError>* saved = std::exchange(errors_, &scratch);
        check(instance, schema, path, ref_depth);
        errors_ = saved;
        return scratch.empty();
    }

    void check_ref(const Value& instance, const std::string& ref, std::string& path,
                   int ref_depth) {
        if (ref_depth >= kMaxRefDepth) {
            fail(path, "schema error: $ref chain is too deep at \"" + ref + "\"");
            return;
        }
        const Value* target = resolve(ref);
        if (target == nullptr) {
            fail(path, "schema error: cannot resolve $ref \"" + ref + "\"");
            return;
        }
        check(instance, *target, path, ref_depth + 1);
    }

    [[nodiscard]] const Value* resolve(std::string_view ref) const {
        if (ref.empty() || ref.front() != '#') {
            return nullptr; // only references inside this schema are supported
        }
        ref.remove_prefix(1);
        const Value* node = &root_;
        while (!ref.empty()) {
            if (ref.front() != '/') {
                return nullptr;
            }
            ref.remove_prefix(1);
            const std::size_t end = std::min(ref.find('/'), ref.size());
            const std::string segment = unescape_pointer_segment(ref.substr(0, end));
            ref.remove_prefix(end);
            node = node->find(segment);
            if (node == nullptr) {
                return nullptr;
            }
        }
        return node;
    }

    bool check_type(const Value& instance, const Value& schema, const std::string& path) {
        const Value* type = schema.find("type");
        if (type == nullptr) {
            return true;
        }
        std::string expected;
        if (type->is_string()) {
            if (matches_type(instance, type->as_string())) {
                return true;
            }
            expected = type->as_string();
        } else if (type->is_array()) {
            for (const Value& option : type->as_array()) {
                if (!option.is_string()) {
                    continue;
                }
                if (matches_type(instance, option.as_string())) {
                    return true;
                }
                expected += expected.empty() ? "" : " or ";
                expected += option.as_string();
            }
        } else {
            return true;
        }
        fail(path, "expected " + expected + ", got " + std::string(type_name(instance)));
        return false;
    }

    void check_enum_and_const(const Value& instance, const Value& schema, const std::string& path) {
        if (const Value* allowed = schema.find("enum"); allowed != nullptr && allowed->is_array()) {
            const Array& options = allowed->as_array();
            if (std::ranges::find(options, instance) == options.end()) {
                std::string list;
                for (const Value& option : options) {
                    list += list.empty() ? "" : ", ";
                    list += text_of(option);
                }
                fail(path, "expected one of " + list + ", got " + text_of(instance));
            }
        }
        if (const Value* constant = schema.find("const");
            constant != nullptr && !(*constant == instance)) {
            fail(path, "expected " + text_of(*constant) + ", got " + text_of(instance));
        }
    }

    void check_combinators(const Value& instance, const Value& schema, std::string& path,
                           int ref_depth) {
        if (const Value* all = schema.find("allOf"); all != nullptr && all->is_array()) {
            for (const Value& option : all->as_array()) {
                check(instance, option, path, ref_depth);
            }
        }
        if (const Value* any = schema.find("anyOf"); any != nullptr && any->is_array()) {
            const bool matched = std::ranges::any_of(any->as_array(), [&](const Value& option) {
                return passes(instance, option, path, ref_depth);
            });
            if (!matched) {
                fail(path, "does not match any of the allowed schemas");
            }
        }
        if (const Value* one = schema.find("oneOf"); one != nullptr && one->is_array()) {
            const auto matches = std::ranges::count_if(one->as_array(), [&](const Value& option) {
                return passes(instance, option, path, ref_depth);
            });
            if (matches != 1) {
                fail(path, "matches " + std::to_string(matches) +
                               " of the allowed schemas; expected exactly one");
            }
        }
        if (const Value* negated = schema.find("not");
            negated != nullptr && passes(instance, *negated, path, ref_depth)) {
            fail(path, "matches a schema it must not match");
        }
    }

    void check_object(const Value& instance, const Value& schema, std::string& path,
                      int ref_depth) {
        const Object& members = instance.as_object();
        static const Object kNoProperties;
        const Value* properties_value = schema.find("properties");
        const Object& properties = properties_value != nullptr && properties_value->is_object()
                                       ? properties_value->as_object()
                                       : kNoProperties;
        const Value* additional = schema.find("additionalProperties");
        const Value* names = schema.find("propertyNames");

        if (const Value* required = schema.find("required");
            required != nullptr && required->is_array()) {
            for (const Value& name : required->as_array()) {
                if (name.is_string() && !members.contains(name.as_string())) {
                    fail(path, "missing required property \"" + name.as_string() + "\"");
                }
            }
        }
        check_count(members.size(), schema, "minProperties", "maxProperties", "properties", path);

        for (const auto& [key, member] : members) {
            const std::size_t length = path.size();
            path += '/';
            path += pointer_escape(key);
            if (names != nullptr) {
                check(Value(key), *names, path, ref_depth);
            }
            if (const auto it = properties.find(key); it != properties.end()) {
                check(member, it->second, path, ref_depth);
            } else if (additional != nullptr && additional->is_bool() && !additional->as_bool()) {
                std::string message = "unknown property \"" + key + "\"";
                if (const std::string_view hint = closest_match(key, property_names(properties));
                    !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                fail(path, std::move(message));
            } else if (additional != nullptr) {
                check(member, *additional, path, ref_depth);
            }
            path.resize(length);
        }
    }

    void check_array(const Value& instance, const Value& schema, std::string& path, int ref_depth) {
        const Array& items = instance.as_array();
        check_count(items.size(), schema, "minItems", "maxItems", "items", path);

        std::size_t prefix_count = 0;
        const Value* prefix = schema.find("prefixItems");
        if (prefix != nullptr && prefix->is_array()) {
            prefix_count = prefix->as_array().size();
        }
        const Value* rest = schema.find("items");
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Value* item_schema = nullptr;
            if (i < prefix_count) {
                item_schema = &prefix->as_array()[i];
            } else if (rest != nullptr) {
                item_schema = rest;
            } else {
                break;
            }
            const std::size_t length = path.size();
            path += '/';
            path += std::to_string(i);
            check(items[i], *item_schema, path, ref_depth);
            path.resize(length);
        }
    }

    void check_count(std::size_t count, const Value& schema, std::string_view min_key,
                     std::string_view max_key, std::string_view noun, const std::string& path) {
        const Value* min = schema.find(min_key);
        if (min != nullptr && min->is_integral() && static_cast<double>(count) < min->as_double()) {
            fail(path, "expected at least " + text_of(*min) + " " + std::string(noun) + ", got " +
                           std::to_string(count));
        }
        const Value* max = schema.find(max_key);
        if (max != nullptr && max->is_integral() && static_cast<double>(count) > max->as_double()) {
            fail(path, "expected at most " + text_of(*max) + " " + std::string(noun) + ", got " +
                           std::to_string(count));
        }
    }

    void check_number(const Value& instance, const Value& schema, const std::string& path) {
        const double number = instance.as_double();
        const auto limit = [&](std::string_view key) -> const Value* {
            const Value* value = schema.find(key);
            return value != nullptr && value->is_number() ? value : nullptr;
        };
        if (const Value* min = limit("minimum"); min != nullptr && number < min->as_double()) {
            fail(path, "must be at least " + text_of(*min) + ", got " + text_of(instance));
        }
        if (const Value* max = limit("maximum"); max != nullptr && number > max->as_double()) {
            fail(path, "must be at most " + text_of(*max) + ", got " + text_of(instance));
        }
        if (const Value* min = limit("exclusiveMinimum");
            min != nullptr && number <= min->as_double()) {
            fail(path, "must be greater than " + text_of(*min) + ", got " + text_of(instance));
        }
        if (const Value* max = limit("exclusiveMaximum");
            max != nullptr && number >= max->as_double()) {
            fail(path, "must be less than " + text_of(*max) + ", got " + text_of(instance));
        }
    }

    void check_string(const Value& instance, const Value& schema, const std::string& path) {
        const std::size_t length = code_points(instance.as_string());
        const Value* min = schema.find("minLength");
        if (min != nullptr && min->is_integral() &&
            static_cast<double>(length) < min->as_double()) {
            fail(path, "must be at least " + text_of(*min) + " characters long");
        }
        const Value* max = schema.find("maxLength");
        if (max != nullptr && max->is_integral() &&
            static_cast<double>(length) > max->as_double()) {
            fail(path, "must be at most " + text_of(*max) + " characters long");
        }
    }

    const Value& root_;
    std::vector<ValidationError>* errors_;
};

} // namespace

std::vector<ValidationError> validate(const Value& instance, const Value& schema) {
    std::vector<ValidationError> errors;
    Validator validator(schema, errors);
    std::string path;
    validator.check(instance, schema, path, 0);
    return errors;
}

} // namespace slash::json
