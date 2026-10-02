#include "atm/api/transport.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#if defined(_WIN32)
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <sddl.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace atm::api {
namespace {

constexpr size_t kMaxMessage = 64u << 20; // 64 MiB

#if defined(_WIN32)

std::wstring widen(const std::string &s) {
  if (s.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

class HandleStream final : public Stream {
public:
  HandleStream(HANDLE h, bool server) : h_(h), server_(server) {}
  ~HandleStream() override {
    if (server_)
      DisconnectNamedPipe(h_);
    CloseHandle(h_);
  }
  size_t read_some(char *buffer, size_t size) override {
    DWORD got = 0;
    if (!ReadFile(h_, buffer, DWORD(size), &got, nullptr))
      return 0;
    return got;
  }
  bool write_all(const char *data, size_t size) override {
    while (size > 0) {
      DWORD put = 0;
      if (!WriteFile(h_, data, DWORD(size), &put, nullptr))
        return false;
      data += put;
      size -= put;
    }
    return true;
  }

private:
  HANDLE h_;
  bool server_;
};

class StdioStream final : public Stream {
public:
  StdioStream() {
    _setmode(0, _O_BINARY);
    _setmode(1, _O_BINARY);
  }
  size_t read_some(char *buffer, size_t size) override {
    const int n = _read(0, buffer, unsigned(size));
    return n > 0 ? size_t(n) : 0;
  }
  bool write_all(const char *data, size_t size) override {
    while (size > 0) {
      const int n = _write(1, data, unsigned(size));
      if (n <= 0)
        return false;
      data += n;
      size -= size_t(n);
    }
    return true;
  }
};

// A security descriptor that lets only the current user open the pipe.
SECURITY_ATTRIBUTES *user_only() {
  static SECURITY_ATTRIBUTES sa{};
  static bool ready = false;
  if (ready)
    return &sa;
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    return nullptr;
  alignas(void *) char info[256];
  DWORD len = 0;
  LPWSTR sid = nullptr;
  PSECURITY_DESCRIPTOR sd = nullptr;
  const bool ok = GetTokenInformation(token, TokenUser, info, sizeof info, &len) &&
                  ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(info)->User.Sid, &sid) &&
                  ConvertStringSecurityDescriptorToSecurityDescriptorW(
                      (std::wstring(L"D:P(A;;GA;;;") + sid + L")").c_str(), SDDL_REVISION_1, &sd, nullptr);
  if (sid)
    LocalFree(sid);
  CloseHandle(token);
  if (!ok)
    return nullptr;
  sa.nLength = sizeof sa;
  sa.lpSecurityDescriptor = sd; // kept for the life of the process
  sa.bInheritHandle = FALSE;
  ready = true;
  return &sa;
}

HANDLE create_pipe(const std::string &endpoint, bool first) {
  SECURITY_ATTRIBUTES *sa = user_only();
  if (!sa)
    return INVALID_HANDLE_VALUE;
  return CreateNamedPipeW(widen(endpoint).c_str(), PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                          PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, sa);
}

#else

class FdStream final : public Stream {
public:
  FdStream(int in, int out, bool owned) : in_(in), out_(out), owned_(owned) {}
  ~FdStream() override {
    if (owned_)
      ::close(in_);
  }
  size_t read_some(char *buffer, size_t size) override {
    for (;;) {
      const ssize_t n = ::read(in_, buffer, size);
      if (n < 0 && errno == EINTR)
        continue;
      return n > 0 ? size_t(n) : 0;
    }
  }
  bool write_all(const char *data, size_t size) override {
    while (size > 0) {
      const ssize_t n = ::write(out_, data, size);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return false;
      data += n;
      size -= size_t(n);
    }
    return true;
  }

private:
  int in_, out_;
  bool owned_;
};

bool socket_address(const std::string &endpoint, sockaddr_un &addr) {
  addr = {};
  addr.sun_family = AF_UNIX;
  if (endpoint.size() >= sizeof addr.sun_path)
    return false;
  std::memcpy(addr.sun_path, endpoint.c_str(), endpoint.size() + 1);
  return true;
}

#endif

} // namespace

std::string default_endpoint() {
  if (const char *e = std::getenv("ATTOME_ENDPOINT"); e && *e)
    return e;
#if defined(_WIN32)
  std::string user = "user";
  if (const char *u = std::getenv("USERNAME"); u && *u)
    user = u;
  for (char &c : user)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
      c = '_';
  return "\\\\.\\pipe\\attome-" + user + "-default";
#elif defined(__APPLE__)
  const char *home = std::getenv("HOME");
  return std::string(home ? home : "/tmp") + "/Library/Caches/attome/default.sock";
#else
  if (const char *run = std::getenv("XDG_RUNTIME_DIR"); run && *run)
    return std::string(run) + "/attome/default.sock";
  return "/tmp/attome-" + std::to_string(getuid()) + "/default.sock";
#endif
}

std::unique_ptr<Stream> connect(const std::string &endpoint) {
#if defined(_WIN32)
  const std::wstring name = widen(endpoint);
  for (int attempt = 0; attempt < 5; ++attempt) {
    const HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE)
      return std::make_unique<HandleStream>(h, false);
    if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(name.c_str(), 2000))
      return nullptr; // every instance is busy only while the daemon creates the next one
  }
  return nullptr;
#else
  sockaddr_un addr;
  if (!socket_address(endpoint, addr))
    return nullptr;
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return nullptr;
  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return nullptr;
  }
  return std::make_unique<FdStream>(fd, fd, true);
#endif
}

std::unique_ptr<Stream> stdio_stream() {
#if defined(_WIN32)
  return std::make_unique<StdioStream>();
#else
  return std::make_unique<FdStream>(0, 1, false);
#endif
}

Result<Listener> Listener::listen(const std::string &endpoint) {
  Listener l;
  l.endpoint_ = endpoint;
  const auto in_use = [&] {
    return fail(ErrorCode::ProjectLocked, "R_DAEMON_RUNNING", "A daemon is already listening on " + endpoint + ".", {},
                "Use it, or stop it with: attome daemon stop");
  };
#if defined(_WIN32)
  const HANDLE h = create_pipe(endpoint, true);
  if (h == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_ACCESS_DENIED || GetLastError() == ERROR_PIPE_BUSY)
      return in_use();
    return fail(ErrorCode::IoError, "IO", "Could not create the pipe " + endpoint + ".");
  }
  l.handle_ = reinterpret_cast<intptr_t>(h);
#else
  std::signal(SIGPIPE, SIG_IGN);
  if (connect(endpoint))
    return in_use();
  sockaddr_un addr;
  if (!socket_address(endpoint, addr))
    return fail(ErrorCode::IoError, "IO", "The socket path " + endpoint + " is too long.");
  std::error_code ec;
  const auto dir = std::filesystem::path(endpoint).parent_path();
  std::filesystem::create_directories(dir, ec);
  ::chmod(dir.c_str(), 0700);
  ::unlink(endpoint.c_str()); // a stale socket of a daemon that is gone
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 ||
      ::chmod(endpoint.c_str(), 0600) != 0 || ::listen(fd, 64) != 0) {
    if (fd >= 0)
      ::close(fd);
    return fail(ErrorCode::IoError, "IO", "Could not listen on " + endpoint + ".");
  }
  l.handle_ = fd;
#endif
  return l;
}

Listener::Listener(Listener &&other) noexcept
    : endpoint_(std::move(other.endpoint_)), handle_(other.handle_), closing_(std::move(other.closing_)) {
  other.handle_ = -1;
}

Listener::~Listener() {
  if (handle_ == -1)
    return;
#if defined(_WIN32)
  CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
  ::close(int(handle_));
  ::unlink(endpoint_.c_str());
#endif
}

std::unique_ptr<Stream> Listener::accept() {
#if defined(_WIN32)
  const HANDLE h = reinterpret_cast<HANDLE>(handle_);
  if (h == INVALID_HANDLE_VALUE)
    return nullptr;
  const bool connected = ConnectNamedPipe(h, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
  if (closing_->load()) {
    return nullptr; // the destructor closes the instance
  }
  handle_ = reinterpret_cast<intptr_t>(create_pipe(endpoint_, false)); // the next client's instance
  if (!connected) {
    CloseHandle(h);
    return accept();
  }
  return std::make_unique<HandleStream>(h, true);
#else
  for (;;) {
    const int fd = ::accept(int(handle_), nullptr, nullptr);
    if (closing_->load()) {
      if (fd >= 0)
        ::close(fd);
      return nullptr;
    }
    if (fd >= 0)
      return std::make_unique<FdStream>(fd, fd, true);
    if (errno != EINTR)
      return nullptr;
  }
#endif
}

void Listener::close() {
  closing_->store(true);
  (void)connect(endpoint_); // wakes the blocked accept()
}

bool FrameReader::fill() {
  if (pos_ > 0 && pos_ == buffer_.size()) {
    buffer_.clear();
    pos_ = 0;
  }
  if (buffer_.size() - pos_ > kMaxMessage + 256)
    return false;
  char chunk[1 << 16];
  const size_t n = stream_.read_some(chunk, sizeof chunk);
  if (n == 0)
    return false;
  buffer_.append(chunk, n);
  return true;
}

bool FrameReader::read(std::string &body) {
  for (;;) {
    const size_t end = buffer_.find("\r\n\r\n", pos_);
    if (end == std::string::npos) {
      if (!fill())
        return false;
      continue;
    }
    // Header names are case-insensitive; only Content-Length matters.
    size_t length = kMaxMessage + 1;
    for (size_t line = pos_; line < end;) {
      size_t eol = buffer_.find("\r\n", line);
      if (eol == std::string::npos || eol > end)
        eol = end;
      static constexpr std::string_view kName = "content-length:";
      bool match = eol - line > kName.size();
      for (size_t i = 0; match && i < kName.size(); ++i) {
        const char c = buffer_[line + i];
        match = (c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c) == kName[i];
      }
      if (match) {
        const char *first = buffer_.data() + line + kName.size(), *last = buffer_.data() + eol;
        while (first < last && *first == ' ')
          ++first;
        std::from_chars(first, last, length);
      }
      line = eol + 2;
    }
    if (length > kMaxMessage)
      return false;
    const size_t start = end + 4;
    while (buffer_.size() - start < length)
      if (!fill())
        return false;
    body.assign(buffer_, start, length);
    pos_ = start + length;
    return true;
  }
}

bool write_frame(Stream &stream, std::string_view body) {
  char header[48];
  const int n = std::snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n", body.size());
  std::string out;
  out.reserve(size_t(n) + body.size());
  out.append(header, size_t(n));
  out.append(body);
  return stream.write_all(out.data(), out.size());
}

bool spawn_daemon(const std::string &endpoint, const std::string &extra_args) {
#if defined(_WIN32)
  wchar_t self[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
  if (n == 0 || n >= std::size(self))
    return false;
  const std::filesystem::path exe = std::filesystem::path(self).parent_path() / L"attomed.exe";
  std::wstring cmd = L"\"" + exe.wstring() + L"\" --endpoint \"" + widen(endpoint) + L"\" " + widen(extra_args);
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                      nullptr, nullptr, &si, &pi))
    return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  char self[4096] = {};
#if defined(__APPLE__)
  uint32_t size = sizeof self;
  if (_NSGetExecutablePath(self, &size) != 0)
    return false;
#else
  if (::readlink("/proc/self/exe", self, sizeof self - 1) <= 0)
    return false;
#endif
  const std::string exe = (std::filesystem::path(self).parent_path() / "attomed").string();
  const pid_t pid = ::fork();
  if (pid < 0)
    return false;
  if (pid == 0) {
    ::setsid();
    if (::fork() != 0) // the grandchild is the daemon
      ::_exit(0);
    const std::string command = "exec \"" + exe + "\" --endpoint \"" + endpoint + "\" " + extra_args +
                                " </dev/null >/dev/null 2>&1";
    ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
    ::_exit(127);
  }
  return true;
#endif
}

} // namespace atm::api
