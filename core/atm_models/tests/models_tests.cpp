#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "atm/base/hash.hpp"
#include "atm/base/id.hpp"
#include "atm/models/models.hpp"

namespace fs = std::filesystem;
namespace models = atm::models;
using json = nlohmann::json;

namespace {

// A server in memory: one file, served whole or from a byte offset, with the faults a real download meets.
struct FakeServer final : atm::net::Transport {
  std::string body;
  int fail_first = 0;            // this many calls lose the connection after `drop_after` bytes
  size_t drop_after = 0;
  bool ignore_range = false;     // answers 200 with the whole file whatever was asked
  int status = 0;                // when set, every call answers this status with no body
  std::vector<int64_t> ranges;   // the range_start of every call
  std::atomic<bool> *cancel_after = nullptr; // set once `cancel_at` bytes have been sent
  size_t cancel_at = 0;

  atm::Result<atm::net::Response> get(const atm::net::Request &request, const atm::net::OnResponse &on_response,
                                      const atm::net::Sink &sink) override {
    ranges.push_back(request.range_start);
    atm::net::Response r;
    if (status != 0) {
      r.status = status;
      on_response(r);
      return r;
    }
    const size_t from = ignore_range ? 0 : size_t(request.range_start);
    if (from > body.size()) {
      r.status = 416;
      on_response(r);
      return r;
    }
    r.status = from > 0 ? 206 : 200;
    r.content_length = int64_t(body.size() - from);
    if (!on_response(r))
      return r;
    const bool drop = fail_first > 0;
    if (drop)
      --fail_first;
    size_t sent = 0;
    for (size_t at = from; at < body.size();) {
      const size_t n = std::min<size_t>(7, body.size() - at); // small pieces, so every boundary is crossed
      if (drop && sent + n > drop_after)
        return atm::fail(atm::ErrorCode::ProviderUnavailable, "N_HTTP", "The connection was lost.");
      if (cancel_after && sent + n >= cancel_at)
        cancel_after->store(true); // the user stops it while this piece is on its way
      if (!sink(reinterpret_cast<const uint8_t *>(body.data()) + at, n))
        return r;
      at += n, sent += n;
    }
    return r;
  }
};

struct Store {
  fs::path dir = fs::temp_directory_path() / atm::new_id("attome-models");
  Store() { fs::create_directories(dir); }
  ~Store() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  std::string read(const std::string &rel) const {
    std::ifstream in(dir / rel, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
  }
};

std::string body_of(size_t n) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i)
    s[i] = char('a' + (i * 31 + i / 7) % 26);
  return s;
}

models::CatalogFile file_for(const std::string &body, const std::string &path = "vae/test.safetensors") {
  return {"https://example.test/" + path, path, atm::sha256_hex(body), int64_t(body.size())};
}

const models::FetchOptions kFast{8, 0}; // no waiting between attempts in tests

} // namespace

TEST_CASE("models: a file is downloaded, checked and put in place; a second fetch does no work", "[models]") {
  Store store;
  FakeServer server;
  server.body = body_of(1000);
  const models::CatalogFile file = file_for(server.body);
  std::atomic<int64_t> done{0};
  std::vector<std::string> phases;
  models::FetchProgress progress{&done, nullptr, [&](const std::string &p) { phases.push_back(p); }};

  CHECK(models::file_status(file, store.dir).state == models::FileState::missing);
  auto r = models::fetch_file(server, file, store.dir, progress, kFast);
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  CHECK(store.read("vae/test.safetensors") == server.body);
  CHECK_FALSE(fs::exists(store.dir / "vae/test.safetensors.part"));
  CHECK(done.load() == 1000);
  CHECK(models::file_status(file, store.dir).state == models::FileState::installed);
  REQUIRE(phases.size() == 2);
  CHECK(phases[0] == "Downloading test.safetensors");
  CHECK(phases[1] == "Checking test.safetensors");

  server.ranges.clear();
  done.store(0);
  REQUIRE(models::fetch_file(server, file, store.dir, progress, kFast));
  CHECK(server.ranges.empty()); // already there: the server is not asked
  CHECK(done.load() == 1000);   // and it still counts as done
}

TEST_CASE("models: a download that loses its connection continues from the byte it reached", "[models]") {
  Store store;
  FakeServer server;
  server.body = body_of(5000);
  server.fail_first = 3;
  server.drop_after = 1400;
  const models::CatalogFile file = file_for(server.body);
  std::atomic<int64_t> done{0};
  auto r = models::fetch_file(server, file, store.dir, {&done, nullptr, {}}, kFast);
  INFO((r ? "" : r.error().message));
  REQUIRE(r);
  CHECK(store.read("vae/test.safetensors") == server.body);
  // Each dropped call delivered 1400 bytes (200 pieces of 7): the next call asks for the rest, never from 0 again.
  REQUIRE(server.ranges.size() == 4);
  CHECK(server.ranges[0] == 0);
  CHECK(server.ranges[1] == 1400);
  CHECK(server.ranges[2] == 2800);
  CHECK(server.ranges[3] == 4200);
  CHECK(done.load() == 5000);
  // "fetched" counts only what came over the network: here every byte once, none twice.
  std::atomic<int64_t> again{0}, fetched{0};
  Store other;
  fs::create_directories(other.dir / "vae");
  std::ofstream(other.dir / "vae/test.safetensors.part", std::ios::binary) << server.body.substr(0, 3000);
  server.fail_first = 0;
  REQUIRE(models::fetch_file(server, file, other.dir, {&again, nullptr, {}, &fetched}, kFast));
  CHECK(again.load() == 5000);
  CHECK(fetched.load() == 2000); // 3000 were already on disk
}

TEST_CASE("models: a stopped download keeps its part and the next run finishes it", "[models]") {
  Store store;
  FakeServer server;
  server.body = body_of(3000);
  const models::CatalogFile file = file_for(server.body);
  std::atomic<bool> cancel{false};
  std::atomic<int64_t> done{0};
  server.cancel_after = &cancel;
  server.cancel_at = 700;
  auto stopped = models::fetch_file(server, file, store.dir, {&done, &cancel, {}}, kFast);
  REQUIRE_FALSE(stopped);
  CHECK(stopped.error().rule == "M_CANCELLED");
  CHECK(stopped.error().code == atm::ErrorCode::Cancelled);
  const models::FileStatus partial = models::file_status(file, store.dir);
  CHECK(partial.state == models::FileState::partial);
  CHECK(partial.bytes == 700);
  CHECK_FALSE(fs::exists(store.dir / "vae/test.safetensors")); // nothing under the final name until it is whole

  cancel.store(false);
  server.cancel_after = nullptr;
  server.ranges.clear();
  REQUIRE(models::fetch_file(server, file, store.dir, {&done, &cancel, {}}, kFast));
  REQUIRE(server.ranges.size() == 1);
  CHECK(server.ranges[0] == 700);
  CHECK(store.read("vae/test.safetensors") == server.body);
}

TEST_CASE("models: a server that ignores the range restarts the file instead of appending to it", "[models]") {
  Store store;
  FakeServer server;
  server.body = body_of(2000);
  const models::CatalogFile file = file_for(server.body);
  fs::create_directories(store.dir / "vae");
  std::ofstream(store.dir / "vae/test.safetensors.part", std::ios::binary) << server.body.substr(0, 900);
  server.ignore_range = true;
  std::atomic<int64_t> done{0};
  REQUIRE(models::fetch_file(server, file, store.dir, {&done, nullptr, {}}, kFast));
  CHECK(store.read("vae/test.safetensors") == server.body); // 2000 bytes, not 900 + 2000
  CHECK(done.load() == 2000);
}

TEST_CASE("models: a file that is not what the catalog says is refused and removed", "[models]") {
  Store store;
  FakeServer server;
  server.body = body_of(1500);
  models::CatalogFile file = file_for(server.body);
  file.sha256 = atm::sha256_hex("something else");
  std::atomic<int64_t> done{0};
  auto r = models::fetch_file(server, file, store.dir, {&done, nullptr, {}}, kFast);
  REQUIRE_FALSE(r);
  CHECK(r.error().rule == "M_HASH");
  CHECK_FALSE(fs::exists(store.dir / "vae/test.safetensors"));
  CHECK_FALSE(fs::exists(store.dir / "vae/test.safetensors.part"));
  CHECK(done.load() == 0);

  // A part on disk that belongs to another file (too long for this one) is thrown away, not trusted.
  file = file_for(server.body);
  std::ofstream(store.dir / "vae/test.safetensors.part", std::ios::binary) << body_of(4000);
  REQUIRE(models::fetch_file(server, file, store.dir, {&done, nullptr, {}}, kFast));
  CHECK(store.read("vae/test.safetensors") == server.body);
}

TEST_CASE("models: the server's refusals are reported with what to do; its own trouble is retried", "[models]") {
  Store store;
  FakeServer server;
  server.body = body_of(100);
  const models::CatalogFile file = file_for(server.body);
  std::atomic<int64_t> done{0};

  server.status = 404;
  auto gone = models::fetch_file(server, file, store.dir, {&done, nullptr, {}}, kFast);
  REQUIRE_FALSE(gone);
  CHECK(gone.error().rule == "M_HTTP_STATUS");
  CHECK(gone.error().hint.find("out of date") != std::string::npos);
  CHECK(server.ranges.size() == 1); // not retried: asking again will not make the file exist

  server.ranges.clear();
  server.status = 403;
  auto denied = models::fetch_file(server, file, store.dir, {&done, nullptr, {}}, kFast);
  REQUIRE_FALSE(denied);
  CHECK(denied.error().hint.find("licence") != std::string::npos);

  server.ranges.clear();
  server.status = 503;
  auto busy = models::fetch_file(server, file, store.dir, {&done, nullptr, {}}, models::FetchOptions{3, 0});
  REQUIRE_FALSE(busy);
  CHECK(busy.error().rule == "M_HTTP_STATUS");
  CHECK(server.ranges.size() == 3); // every attempt was used

  server.ranges.clear();
  server.status = 0;
  server.fail_first = 100; // a connection that never holds
  server.drop_after = 0;
  auto never = models::fetch_file(server, file, store.dir, {&done, nullptr, {}}, models::FetchOptions{4, 0});
  REQUIRE_FALSE(never);
  CHECK(never.error().rule == "N_HTTP");
  CHECK(server.ranges.size() == 4);
}

TEST_CASE("models: an entry's files are fetched in order and progress counts the whole entry", "[models]") {
  Store store;
  FakeServer server;
  server.body = body_of(800);
  models::CatalogEntry entry;
  entry.id = "test.entry";
  entry.files = {file_for(server.body, "vae/a.safetensors"), file_for(server.body, "loras/b.safetensors"),
                 file_for(server.body, "diffusion_models/c.safetensors")};
  CHECK(entry.size() == 2400);
  // The second file is already installed: it is skipped and still counted.
  fs::create_directories(store.dir / "loras");
  std::ofstream(store.dir / "loras/b.safetensors", std::ios::binary) << server.body;
  std::atomic<int64_t> done{0};
  std::vector<std::string> phases;
  REQUIRE(models::fetch_entry(server, entry, store.dir, {&done, nullptr, [&](const std::string &p) { phases.push_back(p); }}, kFast));
  CHECK(done.load() == 2400);
  CHECK(server.ranges.size() == 2);
  REQUIRE(phases.size() == 4);
  CHECK(phases[0] == "Downloading a.safetensors (1 of 3)");
  CHECK(phases[2] == "Downloading c.safetensors (3 of 3)");
  CHECK(store.read("diffusion_models/c.safetensors") == server.body);
}

TEST_CASE("models: the catalog refuses what could leave the models folder or cannot be checked", "[models]") {
  const auto rule_at = [](json files, json extra = json::object()) {
    json entry = {{"id", "x.y"}, {"title", "X"}, {"files", std::move(files)}};
    entry.update(extra);
    const auto r = models::parse_catalog({{"version", 1}, {"entries", json::array({entry})}});
    return r ? std::string("ok") : r.error().rule + " " + r.error().path;
  };
  const std::string hash(64, 'a');
  const auto f = [&](const char *path, json more = json::object()) {
    json file = {{"url", "https://example.test/f"}, {"path", path}, {"size", 10}, {"sha256", hash}};
    file.update(more);
    return json::array({file});
  };
  CHECK(rule_at(f("vae/a.safetensors")) == "ok");
  for (const char *bad : {"../a", "vae/../../a", "/etc/a", "C:/a", "vae\\a", "vae//a", "vae/./a", "", "vae/"})
    CHECK(rule_at(f(bad)) == "M_CATALOG entries/0/files/0/path");
  CHECK(rule_at(f("a", {{"url", "http://example.test/f"}})) == "M_CATALOG entries/0/files/0/url"); // not https
  CHECK(rule_at(f("a", {{"size", 0}})) == "M_CATALOG entries/0/files/0/size");
  CHECK(rule_at(f("a", {{"sha256", "ABC"}})) == "M_CATALOG entries/0/files/0/sha256");
  CHECK(rule_at(json::array()) == "M_CATALOG entries/0/files");
  CHECK(rule_at(f("a"), {{"id", "Bad Id"}}) == "M_CATALOG entries/0/id");
  CHECK(rule_at(f("a"), {{"kind", "virus"}}) == "M_CATALOG entries/0/kind");
  json twice = f("a");
  twice.push_back(twice[0]);
  CHECK(rule_at(twice) == "M_CATALOG entries/0/files/1/path");
  CHECK_FALSE(models::parse_catalog(json::array()));
}

TEST_CASE("models: the built-in catalog parses, and every file is pinned, hashed and inside the store", "[models]") {
  const auto &catalog = models::builtin_catalog();
  REQUIRE_FALSE(catalog.empty());
  const models::CatalogEntry *h3 = models::find_entry(catalog, "minimax-h3.fl2va.turbo8-int8");
  REQUIRE(h3);
  CHECK(h3->files.size() == 6);
  CHECK(h3->size() == 47237843655); // 47.2 GB: the six files of the set-up
  CHECK_FALSE(h3->licence.empty());
  for (const models::CatalogEntry &e : catalog)
    for (const models::CatalogFile &f : e.files) {
      INFO(f.path);
      CHECK(f.sha256.size() == 64);
      CHECK(f.url.find("/resolve/main/") == std::string::npos); // a revision, not a branch that can move
      CHECK(f.url.size() > f.path.size());
      CHECK(f.url.compare(f.url.size() - f.path.size(), f.path.size(), f.path) == 0);
    }
  CHECK(models::find_entry(catalog, "nope") == nullptr);
}
