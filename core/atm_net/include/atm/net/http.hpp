#pragma once
// HTTP for downloads: one GET that can start in the middle of a file (Range), hands the body over in pieces, and can be
// stopped. Everything above it (the model store) talks to the Transport interface, so tests run against a fake one and
// another operating system gets another implementation. On Windows the system transport is WinHTTP: no dependency, the
// system's certificates and proxy settings (or HTTPS_PROXY / HTTP_PROXY when set).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "atm/base/error.hpp"

namespace atm::net {

struct Request {
  std::string url;
  int64_t range_start = 0; // first byte wanted; 0 asks for the whole file
  // For talking to a local service (the user's ComfyUI): a body makes the request a POST of that content type.
  std::string body, content_type;
};

struct Response {
  int status = 0;              // 200 the whole file, 206 the part asked for, anything else is not content
  int64_t content_length = -1; // bytes of this response's body; -1 when the server does not say
};

// Called once the status is known, before any body. Return false to stop without reading the body (not an error).
using OnResponse = std::function<bool(const Response &)>;
// Called with each piece of the body, in order. Return false to stop (the caller cancelled).
using Sink = std::function<bool(const uint8_t *data, size_t size)>;

class Transport {
public:
  virtual ~Transport() = default;
  // Returns the response once the body has been delivered, or was stopped by a callback. Errors (rule N_*) are
  // failures to reach the server or to read from it; an HTTP error status is a Response, not an Error.
  virtual Result<Response> get(const Request &request, const OnResponse &on_response, const Sink &sink) = 0;
};

// One request whose whole answer is wanted as text: the status and the body. A POST when `body` is not empty.
struct Reply {
  int status = 0;
  std::string body;
};
Result<Reply> fetch(Transport &transport, const std::string &url, const std::string &body = {}, const std::string &content_type = "application/json");

// The operating system's HTTP client. A server on this machine (127.0.0.1, localhost) is reached directly, never through a proxy. On a system without one built in, every get() fails with N_UNSUPPORTED.
std::unique_ptr<Transport> system_transport();

} // namespace atm::net
