#pragma once

#include <string>

namespace uvnvr {

// Builds the OpenAPI 3.1 document served at /api/openapi.json.
std::string BuildOpenApi(const std::string& version);

} // namespace uvnvr
