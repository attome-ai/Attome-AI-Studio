#pragma once
// The daemon's threading model (F0 §4.3): connection threads only frame and parse messages; one writer thread
// owns the Engine and runs every Tool call in arrival order.

#include <chrono>
#include <functional>
#include <memory>

#include "atm/api/engine.hpp"
#include "atm/api/transport.hpp"

namespace atm::api {

class Server {
public:
  explicit Server(Engine &engine);
  ~Server(); // stops the writer and waits for connection threads

  void serve(Listener &listener);   // accept loop; one "atm-conn" thread per client; returns after listener.close()
  void serve_stream(Stream &stream); // one connection on the calling thread (stdio mode)

  json submit(json request);            // run one JSON-RPC request on the writer thread and wait for the response
  void post(std::function<void()> job); // run something on the writer thread, without waiting

  // True once a client called daemon.shutdown; waits at most `timeout`.
  bool wait_shutdown(std::chrono::milliseconds timeout);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace atm::api
