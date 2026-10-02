#pragma once
// Agent-readable errors (MODULES §A.2.2). Public functions that can fail return atm::Result<T>.

#include <cstdint>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <tl/expected.hpp>

namespace atm {

enum class ErrorCode : uint32_t { // stable numbers; never renumbered
  Ok = 0,
  InvalidArgument = 1000,
  SchemaViolation = 1001,
  UnknownId = 1002,
  TypeMismatch = 1003,
  CycleDetected = 1004,
  TimeOverflow = 1005,
  ConflictingPatch = 1006,
  StaleBaseRevision = 1007,
  ProjectLocked = 1008,
  ObjectLocked = 1009,
  GestureExpired = 1010,
  Unsupported = 1100,
  SchemaVersionUnsupported = 1101,
  EncoderUnavailable = 1102,
  PolicyBudgetExceeded = 1200,
  PolicyPermissionDenied = 1201,
  PolicyConsentMissing = 1202,
  PolicyLicenseBlocked = 1203,
  PolicyHumanOnly = 1204,
  NotFound = 1300,
  IoError = 1301,
  CorruptData = 1302,
  MediaDecodeFailed = 1303,
  OutputExists = 1304,
  WorkerCrashed = 1400,
  WorkerTimeout = 1401,
  ProviderError = 1402,
  ProviderRateLimited = 1403,
  ModelMissing = 1404,
  ProviderUnavailable = 1405,
  WorkerUnavailable = 1406,
  GpuOutOfMemory = 1500,
  GpuDeviceLost = 1501,
  GpuUnsupported = 1502,
  Cancelled = 1600,
  Internal = 1900
};

struct Error {
  ErrorCode code = ErrorCode::Internal;
  std::string rule;       // stable rule ID of the check that failed, e.g. "R_TRACK_OVERLAP"
  std::string message;    // one plain sentence
  std::string path;       // Stable-ID path, may be empty
  std::string hint;       // what to do next
  nlohmann::json details; // machine data
  nlohmann::json errors;  // array of {code, rule, path, message, hint} when several problems are reported
};

template <class T> using Result = tl::expected<T, Error>;

tl::unexpected<Error> fail(ErrorCode code, std::string rule, std::string message, std::string path = {},
                           std::string hint = {});

// JSON-RPC error object: {code, message, data{rule, path, hint, details, errors}}.
nlohmann::json error_to_json(const Error &e);

// CLI exit code for an ErrorCode number (MODULES §M16 table).
int exit_code(uint32_t error_code);

} // namespace atm

#define ATM_CONCAT2(a, b) a##b
#define ATM_CONCAT(a, b) ATM_CONCAT2(a, b)
#define ATM_TRY_IMPL(tmp, decl, expr)                                                                        \
  auto tmp = (expr);                                                                                         \
  if (!tmp)                                                                                                  \
    return tl::unexpected(std::move(tmp.error()));                                                           \
  decl = std::move(*tmp)
// ATM_TRY(auto x, may_fail());  returns the error from the enclosing function, else binds the value.
#define ATM_TRY(decl, expr) ATM_TRY_IMPL(ATM_CONCAT(atm_try_, __LINE__), decl, expr)
// ATM_CHECK(may_fail());  for Result<void>.
#define ATM_CHECK(expr)                                                                                      \
  if (auto ATM_CONCAT(atm_chk_, __LINE__) = (expr); !ATM_CONCAT(atm_chk_, __LINE__))                         \
    return tl::unexpected(std::move(ATM_CONCAT(atm_chk_, __LINE__).error()))
