#pragma once
// Daemon transports (MODULES §A.3.1): a per-user named pipe on Windows, a Unix socket elsewhere, or stdio.
// Every link carries LSP-style frames: "Content-Length: N\r\n\r\n" + UTF-8 JSON.

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "atm/base/error.hpp"

namespace atm::api {

class Stream {
public:
  virtual ~Stream() = default;
  virtual size_t read_some(char *buffer, size_t size) = 0; // 0 = closed
  virtual bool write_all(const char *data, size_t size) = 0;
};

// \\.\pipe\attome-<user>-default, $XDG_RUNTIME_DIR/attome/default.sock, or $ATTOME_ENDPOINT when set.
std::string default_endpoint();

std::unique_ptr<Stream> connect(const std::string &endpoint); // nullptr when no daemon listens there
std::unique_ptr<Stream> stdio_stream();

class Listener {
public:
  static Result<Listener> listen(const std::string &endpoint); // user-only access
  Listener(Listener &&other) noexcept;
  Listener &operator=(Listener &&) = delete;
  ~Listener();

  std::unique_ptr<Stream> accept(); // blocks; nullptr after close()
  void close();                     // may be called from another thread

private:
  Listener() = default;
  std::string endpoint_;
  intptr_t handle_ = -1;
  std::unique_ptr<std::atomic<bool>> closing_ = std::make_unique<std::atomic<bool>>(false);
};

// Reads frames from a Stream through a buffer, so a message costs about one system call.
class FrameReader {
public:
  explicit FrameReader(Stream &stream) : stream_(stream) {}
  bool read(std::string &body); // false when the peer closed or sent a broken frame

private:
  bool fill();
  Stream &stream_;
  std::string buffer_;
  size_t pos_ = 0;
};

bool write_frame(Stream &stream, std::string_view body);

// Starts `attomed` (found next to this executable) as a detached process.
bool spawn_daemon(const std::string &endpoint, const std::string &extra_args);

} // namespace atm::api
