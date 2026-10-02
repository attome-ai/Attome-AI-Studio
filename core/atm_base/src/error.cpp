#include "atm/base/error.hpp"

namespace atm {

tl::unexpected<Error> fail(ErrorCode code, std::string rule, std::string message, std::string path,
                           std::string hint) {
  Error e;
  e.code = code;
  e.rule = std::move(rule);
  e.message = std::move(message);
  e.path = std::move(path);
  e.hint = std::move(hint);
  return tl::unexpected(std::move(e));
}

nlohmann::json error_to_json(const Error &e) {
  nlohmann::json data = nlohmann::json::object();
  if (!e.rule.empty())
    data["rule"] = e.rule;
  if (!e.path.empty())
    data["path"] = e.path;
  if (!e.hint.empty())
    data["hint"] = e.hint;
  if (!e.details.is_null())
    data["details"] = e.details;
  if (e.errors.is_array())
    data["errors"] = e.errors;
  return {{"code", uint32_t(e.code)}, {"message", e.message}, {"data", std::move(data)}};
}

int exit_code(uint32_t c) {
  if (c == 0)
    return 0;
  if (c >= 1000 && c <= 1005)
    return 3;
  if (c >= 1006 && c <= 1010)
    return 10;
  if (c == 1100 || c == 1101 || c == 1102)
    return 11;
  if (c >= 1200 && c < 1300)
    return 5;
  if (c == 1300)
    return 4;
  if (c > 1300 && c < 1400)
    return 6;
  if (c >= 1400 && c < 1500)
    return 8;
  if (c >= 1500 && c < 1600)
    return 7;
  if (c == 1600)
    return 9;
  return 70;
}

} // namespace atm
