#pragma once
// Stable IDs (MODULES §A.2.4): <prefix>_<ULID>, 26 characters of Crockford base32, time-sortable.

#include <string>
#include <string_view>

namespace atm {

std::string new_id(std::string_view prefix); // monotonic within the process

// True for ^[a-z]{2,4}_[0-9A-HJKMNP-TV-Z]{26}$.
bool is_stable_id(std::string_view s) noexcept;

// "clp" for "clp_01J…"; empty when s is not a Stable ID.
std::string_view id_prefix(std::string_view s) noexcept;

// Placeholder IDs inside one Patch: "$new:<name>".
inline bool is_placeholder(std::string_view s) noexcept { return s.size() > 5 && s.substr(0, 5) == "$new:"; }

} // namespace atm
