#pragma once

#include "slash/core/json.hpp"

#include <string>
#include <vector>

namespace slash::json {

struct ValidationError {
    std::string path;    // JSON Pointer to the offending value; "" is the document root
    std::string message; // e.g. "expected number, got string"
};

// Validates instance against a JSON Schema (draft 2020-12) and returns every error found, or an
// empty list if it is valid.
//
// Supports the keywords the engine's generated schemas use: type, enum, const, properties,
// required, additionalProperties, propertyNames, minProperties, maxProperties, items,
// prefixItems, minItems, maxItems, minimum, maximum, exclusiveMinimum, exclusiveMaximum,
// minLength, maxLength, allOf, anyOf, oneOf, not, and $ref to "#..." pointers inside the same
// schema. Other keywords (pattern, format, $id and so on) are ignored, the way JSON Schema treats
// unknown keywords.
[[nodiscard]] std::vector<ValidationError> validate(const Value& instance, const Value& schema);

} // namespace slash::json
