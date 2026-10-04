#include "atm/net/http.hpp"

#include <cstdlib>
#include <vector>

#include "atm/base/profiler.hpp"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace atm::net {
namespace {

#ifdef _WIN32

std::wstring widen(const std::string &utf8) {
  if (utf8.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
  return out;
}

tl::unexpected<Error> net_error(const char *what, const std::string &url) {
  const DWORD code = GetLastError();
  return fail(ErrorCode::ProviderUnavailable, "N_HTTP",
              std::string("Could not ") + what + " (" + url + "), Windows error " + std::to_string(code) + ".", {},
              "Check the internet connection and try again; a download continues where it stopped.");
}

struct Handle {
  HINTERNET h = nullptr;
  Handle() = default;
  explicit Handle(HINTERNET handle) : h(handle) {}
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  ~Handle() {
    if (h)
      WinHttpCloseHandle(h);
  }
  explicit operator bool() const { return h != nullptr; }
};

// "http://host:port" from HTTPS_PROXY / HTTP_PROXY, as WinHTTP wants it ("host:port"); empty when none is set.
std::wstring env_proxy(bool https) {
  for (const char *name : {https ? "HTTPS_PROXY" : "HTTP_PROXY", https ? "https_proxy" : "http_proxy"}) {
    const char *v = std::getenv(name);
    if (!v || !*v)
      continue;
    std::string s = v;
    if (const size_t scheme = s.find("://"); scheme != std::string::npos)
      s = s.substr(scheme + 3);
    while (!s.empty() && s.back() == '/')
      s.pop_back();
    if (!s.empty())
      return widen(s);
  }
  return {};
}

class WinHttpTransport final : public Transport {
public:
  Result<Response> get(const Request &request, const OnResponse &on_response, const Sink &sink) override {
    ATM_PROFILE_SCOPE("net.get");
    const std::wstring url = widen(request.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256], path[4096];
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &parts))
      return fail(ErrorCode::InvalidArgument, "N_URL", "\"" + request.url + "\" is not a web address.", {},
                  "A download address starts with https://.");
    const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;

    const std::wstring name(host, parts.dwHostNameLength);
    const bool local = name == L"127.0.0.1" || name == L"localhost" || name == L"::1" || name == L"[::1]";
    const std::wstring proxy = local ? std::wstring() : env_proxy(https);
    Handle session(local           ? WinHttpOpen(L"Attome/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)
                   : proxy.empty() ? WinHttpOpen(L"Attome/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                               WINHTTP_NO_PROXY_BYPASS, 0)
                   : WinHttpOpen(L"Attome/0.1", WINHTTP_ACCESS_TYPE_NAMED_PROXY, proxy.c_str(), WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
      return net_error("start the HTTP client", request.url);
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000); // resolve, connect, send, receive (ms)
    Handle connection(WinHttpConnect(session.h, host, parts.nPort, 0));
    if (!connection)
      return net_error("connect", request.url);
    const bool post = !request.body.empty();
    Handle req(WinHttpOpenRequest(connection.h, post ? L"POST" : L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  https ? WINHTTP_FLAG_SECURE : 0));
    if (!req)
      return net_error("open the request", request.url);
    if (request.range_start > 0) {
      const std::wstring range = L"Range: bytes=" + std::to_wstring(request.range_start) + L"-";
      WinHttpAddRequestHeaders(req.h, range.c_str(), DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (post) {
      const std::wstring type = L"Content-Type: " + widen(request.content_type.empty() ? std::string("application/json") : request.content_type);
      WinHttpAddRequestHeaders(req.h, type.c_str(), DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, post ? const_cast<char *>(request.body.data()) : WINHTTP_NO_REQUEST_DATA,
                            DWORD(request.body.size()), DWORD(request.body.size()), 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
      return net_error("reach the server", request.url);

    Response response;
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                             &size, WINHTTP_NO_HEADER_INDEX))
      return net_error("read the response", request.url);
    response.status = int(status);
    wchar_t length[32];
    size = sizeof length; // as text: a file can be larger than a 32-bit number
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &size,
                            WINHTTP_NO_HEADER_INDEX))
      response.content_length = _wtoi64(length);
    if (on_response && !on_response(response))
      return response;

    std::vector<uint8_t> buffer(1 << 20);
    for (;;) {
      DWORD got = 0;
      if (!WinHttpReadData(req.h, buffer.data(), DWORD(buffer.size()), &got))
        return net_error("read from the server", request.url);
      if (got == 0)
        break;
      if (sink && !sink(buffer.data(), size_t(got)))
        break;
    }
    return response;
  }
};

#else

class NoTransport final : public Transport {
public:
  Result<Response> get(const Request &request, const OnResponse &, const Sink &) override {
    return fail(ErrorCode::Unsupported, "N_UNSUPPORTED", "Downloads are not built for this system yet (" + request.url + ").", {},
                "Download the file by hand and put it in the models folder.");
  }
};

#endif

} // namespace

Result<Reply> fetch(Transport &transport, const std::string &url, const std::string &body, const std::string &content_type) {
  Request request;
  request.url = url;
  request.body = body;
  request.content_type = content_type;
  Reply reply;
  const auto response = transport.get(
      request, nullptr, [&](const uint8_t *data, size_t size) {
        reply.body.append(reinterpret_cast<const char *>(data), size);
        return reply.body.size() < (size_t(64) << 20); // an answer, not a download
      });
  if (!response)
    return tl::unexpected(response.error());
  reply.status = response->status;
  return reply;
}

std::unique_ptr<Transport> system_transport() {
#ifdef _WIN32
  return std::make_unique<WinHttpTransport>();
#else
  return std::make_unique<NoTransport>();
#endif
}

} // namespace atm::net
