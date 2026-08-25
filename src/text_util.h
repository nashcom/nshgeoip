#pragma once

// Small text-sanitization helpers shared by the JSON body and header
// writers. MMDB field values are normally well-formed, but they are
// external data (a downloaded database file) and are treated as untrusted
// output here regardless.

#include <string>

namespace nshgeoip
{

// Escapes a UTF-8 string for embedding inside a JSON string literal
// (quotes, backslashes, and C0 control characters).
std::string json_escape(const std::string &in);

// Strips CR, LF, and other control characters from a value before it is
// placed into an HTTP header, so an MMDB field can never be used to inject
// a header or split the response.
std::string sanitize_header_value(const std::string &in);

} // namespace nshgeoip
