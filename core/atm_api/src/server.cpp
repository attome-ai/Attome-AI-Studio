#include "atm/api/server.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

#include "atm/base/profiler.hpp"

namespace atm::api {

struct Server::Impl {
  Engine &engine;
  std::mutex mutex;
  std::condition_variable wake, changed;
  std::deque<std::function<void()>> jobs;
  bool stopping = false;
  bool shutdown = false;
  std::atomic<int> connections{0};
  std::thread writer;

  explicit Impl(Engine &e) : engine(e) {}

  void run_writer() {
    prof::set_thread_name("atm-writer");
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return stopping || !jobs.empty(); });
        if (jobs.empty())
          return;
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      job();
      if (engine.shutdown_requested()) {
        std::lock_guard lock(mutex);
        shutdown = true;
        changed.notify_all();
      }
    }
  }
};

Server::Server(Engine &engine) : impl_(std::make_unique<Impl>(engine)) {
  impl_->writer = std::thread([this] { impl_->run_writer(); });
}

Server::~Server() {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->stopping = true;
  }
  impl_->wake.notify_all();
  impl_->writer.join();
  while (impl_->connections.load() > 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void Server::post(std::function<void()> job) {
  {
    std::lock_guard lock(impl_->mutex);
    impl_->jobs.push_back(std::move(job));
  }
  impl_->wake.notify_one();
}

json Server::submit(json request) {
  std::promise<json> promise;
  std::future<json> response = promise.get_future();
  post([&] {
    ATM_PROFILE_FRAME(); // one request = one frame of the writer thread
    promise.set_value(rpc_dispatch(impl_->engine, request));
  });
  return response.get();
}

void Server::serve_stream(Stream &stream) {
  FrameReader reader(stream);
  std::string body;
  while (reader.read(body)) {
    ATM_PROFILE_FRAME();
    json request;
    {
      ATM_PROFILE_SCOPE("rpc.parse");
      request = json::parse(body, nullptr, false);
    }
    json response;
    if (request.is_discarded()) {
      response = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "Parse error."}}}};
    } else {
      ATM_PROFILE_SCOPE("rpc.wait_writer");
      response = submit(std::move(request));
    }
    if (response.is_null())
      continue;
    std::string text;
    {
      ATM_PROFILE_SCOPE("rpc.serialize");
      text = response.dump();
    }
    ATM_PROFILE_SCOPE("rpc.write");
    if (!write_frame(stream, text))
      break;
  }
}

void Server::serve(Listener &listener) {
  while (std::unique_ptr<Stream> stream = listener.accept()) {
    ++impl_->connections;
    std::thread([this, s = std::move(stream)]() mutable {
      prof::set_thread_name("atm-conn");
      serve_stream(*s);
      s.reset();
      --impl_->connections;
    }).detach();
  }
}

bool Server::wait_shutdown(std::chrono::milliseconds timeout) {
  std::unique_lock lock(impl_->mutex);
  return impl_->changed.wait_for(lock, timeout, [&] { return impl_->shutdown; });
}

} // namespace atm::api
