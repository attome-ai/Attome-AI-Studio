#include "atm/models/models.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>
#include <thread>

#include "atm/base/hash.hpp"
#include "atm/base/profiler.hpp"

namespace atm::models {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

fs::path to_path(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string to_utf8(const fs::path &p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

tl::unexpected<Error> catalog_error(const std::string &path, const std::string &message, const std::string &hint) {
  return fail(ErrorCode::SchemaViolation, "M_CATALOG", message, path, hint);
}

// A path that stays inside the models folder: relative, forward slashes, no "." or ".." part, no drive letter.
bool safe_relative(const std::string &p) {
  if (p.empty() || p.front() == '/' || p.back() == '/' || p.find('\\') != std::string::npos || p.find(':') != std::string::npos)
    return false;
  size_t start = 0;
  while (start <= p.size()) {
    const size_t end = std::min(p.find('/', start), p.size());
    const std::string part = p.substr(start, end - start);
    if (part.empty() || part == "." || part == "..")
      return false;
    start = end + 1;
  }
  return true;
}

bool is_sha256(const std::string &s) {
  return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

int64_t size_of(const fs::path &p) {
  std::error_code ec;
  const auto n = fs::file_size(p, ec);
  return ec ? -1 : int64_t(n);
}

fs::path part_of(const fs::path &dest) {
  fs::path p = dest;
  p += ".part";
  return p;
}

void phase(const FetchProgress &progress, const std::string &text) {
  if (progress.on_phase)
    progress.on_phase(text);
}
bool cancelled(const FetchProgress &progress) { return progress.cancel && progress.cancel->load(); }
void set_done(const FetchProgress &progress, int64_t bytes) {
  if (progress.done)
    progress.done->store(bytes);
}
void add_done(const FetchProgress &progress, int64_t bytes) {
  if (progress.done)
    progress.done->fetch_add(bytes);
}

tl::unexpected<Error> cancelled_error(const std::string &name) {
  return fail(ErrorCode::Cancelled, "M_CANCELLED", "The download of " + name + " was stopped.", {},
              "Start it again to continue from where it stopped.");
}

} // namespace

int64_t CatalogEntry::size() const {
  int64_t total = 0;
  for (const CatalogFile &f : files)
    total += f.size;
  return total;
}

Result<std::vector<CatalogEntry>> parse_catalog(const json &catalog) {
  if (!catalog.is_object() || !catalog.contains("entries") || !catalog["entries"].is_array())
    return catalog_error("entries", "The model catalog must be an object with an \"entries\" array.",
                         "{\"version\": 1, \"entries\": [{\"id\": …, \"title\": …, \"files\": [{\"url\", \"path\", \"size\", \"sha256\"}]}]}");
  std::vector<CatalogEntry> out;
  std::set<std::string> ids;
  size_t i = 0;
  for (const json &e : catalog["entries"]) {
    const std::string at = "entries/" + std::to_string(i++);
    if (!e.is_object())
      return catalog_error(at, "A catalog entry must be an object.", "Give it \"id\", \"title\" and \"files\".");
    CatalogEntry entry;
    entry.id = e.value("id", std::string());
    entry.title = e.value("title", entry.id);
    entry.kind = e.value("kind", std::string("model"));
    entry.licence = e.value("licence", std::string());
    entry.licence_url = e.value("licence_url", std::string());
    entry.notes = e.value("notes", std::string());
    if (const auto declares = e.find("declares"); declares != e.end()) {
      if (!declares->is_object())
        return catalog_error(at + "/declares", "\"declares\" of a catalog entry must be an object.", "Give it \"kinds\" and \"settings\".");
      entry.declares = *declares;
    }
    const bool id_ok = !entry.id.empty() && std::all_of(entry.id.begin(), entry.id.end(), [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    });
    if (!id_ok || !ids.insert(entry.id).second)
      return catalog_error(at + "/id", "The entry id \"" + entry.id + "\" is empty, repeated, or has characters other than a-z, 0-9, '.', '-' and '_'.",
                           "Use a unique name such as \"minimax-h3.fl2va.turbo8-int8\".");
    if (entry.kind != "model" && entry.kind != "runtime")
      return catalog_error(at + "/kind", "The kind of " + entry.id + " must be \"model\" or \"runtime\".", "Leave it out for a model.");
    const auto files = e.find("files");
    if (files == e.end() || !files->is_array() || files->empty())
      return catalog_error(at + "/files", "The entry " + entry.id + " lists no files.", "Give each file a url, a path, a size and a sha256.");
    size_t k = 0;
    for (const json &f : *files) {
      const std::string fat = at + "/files/" + std::to_string(k++);
      if (!f.is_object())
        return catalog_error(fat, "A file of " + entry.id + " must be an object.", "{\"url\", \"path\", \"size\", \"sha256\"}");
      CatalogFile file;
      file.url = f.value("url", std::string());
      file.path = f.value("path", std::string());
      file.sha256 = f.value("sha256", std::string());
      file.size = f.contains("size") && f["size"].is_number_integer() ? f["size"].get<int64_t>() : 0;
      if (file.url.rfind("https://", 0) != 0)
        return catalog_error(fat + "/url", "The address of " + file.path + " must start with https://.", "Downloads are only made over HTTPS.");
      if (!safe_relative(file.path))
        return catalog_error(fat + "/path", "\"" + file.path + "\" is not a path inside the models folder.",
                             "Use a relative path with forward slashes, such as \"vae/name.safetensors\"; no \"..\", no drive letter.");
      if (std::any_of(entry.files.begin(), entry.files.end(), [&](const CatalogFile &o) { return o.path == file.path; }))
        return catalog_error(fat + "/path", "\"" + file.path + "\" is listed twice in " + entry.id + ".", "List each file once per entry.");
      if (file.size < 1)
        return catalog_error(fat + "/size", "The size of " + file.path + " must be its number of bytes.", "A whole number above 0.");
      if (!file.sha256.empty() && !is_sha256(file.sha256))
        return catalog_error(fat + "/sha256", "The sha256 of " + file.path + " must be 64 lowercase hex digits.",
                             "Copy it from the publisher's file page.");
      entry.files.push_back(std::move(file));
    }
    out.push_back(std::move(entry));
  }
  return out;
}

const CatalogEntry *find_entry(const std::vector<CatalogEntry> &catalog, std::string_view id) {
  for (const CatalogEntry &e : catalog)
    if (e.id == id)
      return &e;
  return nullptr;
}

fs::path default_models_dir() {
  if (const char *dir = std::getenv("ATTOME_MODELS_DIR"); dir && *dir)
    return to_path(dir);
#ifdef _WIN32
  if (const char *local = std::getenv("LOCALAPPDATA"); local && *local)
    return to_path(local) / "Attome" / "models";
  return fs::path("models");
#else
  if (const char *home = std::getenv("HOME"); home && *home)
    return to_path(home) / ".local" / "share" / "attome" / "models";
  return fs::path("models");
#endif
}

FileStatus file_status(const CatalogFile &file, const fs::path &models_dir) {
  const fs::path dest = models_dir / to_path(file.path);
  if (size_of(dest) == file.size)
    return {FileState::installed, file.size};
  const int64_t part = size_of(part_of(dest));
  if (part > 0)
    return {FileState::partial, std::min(part, file.size)};
  return {};
}

Result<void> fetch_file(net::Transport &transport, const CatalogFile &file, const fs::path &models_dir,
                        const FetchProgress &progress, const FetchOptions &options) {
  ATM_PROFILE_SCOPE("models.fetch_file");
  const fs::path dest = models_dir / to_path(file.path), part = part_of(dest);
  const std::string name = to_utf8(dest.filename());
  const int64_t before = progress.done ? progress.done->load() : 0; // bytes of the files before this one
  if (size_of(dest) == file.size) {
    set_done(progress, before + file.size);
    return {};
  }
  std::error_code ec;
  fs::create_directories(dest.parent_path(), ec);
  if (ec)
    return fail(ErrorCode::IoError, "M_DISK", "The folder for " + name + " could not be made: " + ec.message() + ".", {},
                "Check that the models folder can be written to: " + to_utf8(models_dir));
  fs::remove(dest, ec); // a file of the wrong size under the final name is not ours to trust

  Error last = Error{ErrorCode::ProviderUnavailable, "M_INCOMPLETE", "The download of " + name + " kept stopping before the end.", {},
                     "Check the internet connection and start it again; it continues from where it stopped.", {}, {}};
  bool complete = false;
  int failures = 0; // tries in a row that brought nothing
  for (int64_t had = -1; failures < options.attempts && !complete; ++failures) {
    if (cancelled(progress))
      return cancelled_error(name);
    if (const int64_t now = std::max<int64_t>(0, size_of(part)); now > had && had >= 0)
      failures = 0; // the last try brought bytes: not a failure in a row
    if (failures > 0) { // waited in slices, so a cancel is not held up by a long wait
      const int64_t wait = std::min<int64_t>(int64_t(options.retry_delay_ms) << std::min(failures - 1, 20), options.max_retry_delay_ms);
      for (int64_t waited = 0; waited < wait && !cancelled(progress); waited += 100)
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(100, wait - waited)));
      if (cancelled(progress))
        return cancelled_error(name);
    }
    had = std::max<int64_t>(0, size_of(part));
    int64_t offset = std::max<int64_t>(0, size_of(part));
    if (offset > file.size) { // larger than the file can be: not a part of it
      fs::remove(part, ec);
      offset = 0;
    }
    set_done(progress, before + offset);
    if (offset == file.size) {
      complete = true;
      break;
    }
    std::ofstream out;
    int status = 0;
    bool disk_failed = false, restart = false;
    const auto on_response = [&](const net::Response &r) {
      status = r.status;
      if (r.status == 206 && offset > 0) {
        out.open(part, std::ios::binary | std::ios::app);
      } else if (r.status == 200) { // the whole file, even if a part was asked for: start again from the first byte
        offset = 0;
        set_done(progress, before);
        out.open(part, std::ios::binary | std::ios::trunc);
      } else {
        restart = r.status == 416; // "that range does not exist": the part on disk does not belong to this file
        return false;
      }
      disk_failed = !out.is_open();
      return !disk_failed;
    };
    const auto sink = [&](const uint8_t *data, size_t size) {
      out.write(reinterpret_cast<const char *>(data), std::streamsize(size));
      if (!out) {
        disk_failed = true;
        return false;
      }
      add_done(progress, int64_t(size));
      if (progress.fetched)
        progress.fetched->fetch_add(int64_t(size));
      return !cancelled(progress);
    };
    phase(progress, "Downloading " + name);
    const auto got = transport.get({file.url, offset}, on_response, sink);
    out.close();
    if (disk_failed)
      return fail(ErrorCode::IoError, "M_DISK", name + " could not be written; the disk may be full.", {},
                  "It needs " + std::to_string((file.size - std::max<int64_t>(0, size_of(part))) / 1000000) + " MB more in " + to_utf8(models_dir) + ".");
    if (cancelled(progress))
      return cancelled_error(name);
    if (!got) {
      last = got.error(); // the network failed: what arrived is kept, and the next attempt asks for the rest
      continue;
    }
    if (restart) {
      fs::remove(part, ec);
      continue;
    }
    if (status != 200 && status != 206) {
      if (status >= 500 || status == 429 || status == 408) { // the server's trouble, worth another try
        last = Error{ErrorCode::ProviderUnavailable, "M_HTTP_STATUS", "The server answered " + std::to_string(status) + " for " + name + ".", {},
                     "Try again later.", {}, {}};
        continue;
      }
      return fail(ErrorCode::ProviderError, "M_HTTP_STATUS", "The server answered " + std::to_string(status) + " for " + name + ".", {},
                  status == 404 ? "The file is no longer at that address; this version of Attome's catalog is out of date."
                                : status == 401 || status == 403 ? "The publisher asks for a login or an accepted licence for this file."
                                                                 : "Try again later.");
    }
    const int64_t have = size_of(part);
    if (have > file.size)
      fs::remove(part, ec); // more bytes than the catalog says: start over
    complete = have == file.size;
  }
  if (!complete)
    return tl::unexpected(last);

  if (!file.sha256.empty()) {
    ATM_PROFILE_SCOPE("models.verify");
    phase(progress, "Checking " + name);
    std::ifstream in(part, std::ios::binary);
    Sha256 hash;
    std::vector<char> buffer(4 << 20);
    while (in) {
      in.read(buffer.data(), std::streamsize(buffer.size()));
      hash.update(buffer.data(), size_t(in.gcount()));
      if (cancelled(progress))
        return cancelled_error(name);
    }
    in.close();
    if (hash.hex() != file.sha256) {
      fs::remove(part, ec);
      set_done(progress, before);
      return fail(ErrorCode::CorruptData, "M_HASH", name + " arrived damaged, or is not the file the catalog describes; it was removed.", {},
                  "Start the download again. If it happens twice, the publisher changed the file.");
    }
  }
  fs::rename(part, dest, ec);
  if (ec)
    return fail(ErrorCode::IoError, "M_DISK", name + " could not be put in place: " + ec.message() + ".", {},
                "Close any program that has the file open and start again.");
  set_done(progress, before + file.size);
  return {};
}

Result<void> fetch_entry(net::Transport &transport, const CatalogEntry &entry, const fs::path &models_dir,
                         const FetchProgress &progress, const FetchOptions &options) {
  set_done(progress, 0);
  size_t n = 0;
  for (const CatalogFile &file : entry.files) {
    ++n;
    FetchProgress one = progress;
    one.on_phase = [&](const std::string &text) {
      phase(progress, text + " (" + std::to_string(n) + " of " + std::to_string(entry.files.size()) + ")");
    };
    ATM_CHECK(fetch_file(transport, file, models_dir, one, options));
  }
  return {};
}

} // namespace atm::models
