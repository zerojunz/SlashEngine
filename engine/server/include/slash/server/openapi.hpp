#pragma once

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"

#include <string_view>

namespace slash::server {

// An OpenAPI 3.1 document for the REST mirror: one POST operation per command, with the
// command's params schema as the request body. OpenAPI 3.1 uses JSON Schema 2020-12, the same
// dialect as the params, so the schemas are used unchanged.
[[nodiscard]] json::Value openapi(const command::Registry& commands, std::string_view server_url);

// The HTTP status for a command error code, e.g. 404 for an unknown entity.
[[nodiscard]] int http_status(int error_code) noexcept;

} // namespace slash::server
