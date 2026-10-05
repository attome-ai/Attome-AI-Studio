#pragma once
// The model store: a catalog of what can be downloaded (models now, runtimes later), where the files live on this
// machine, and the download itself. A download can stop at any byte and continue from there: the bytes go to
// "<file>.part", a later run asks the server for the rest (HTTP Range), and only a file with the right size and
// SHA-256 is renamed into place. So a file under its own name is always complete and checked.
//
// The store's folders follow ComfyUI's layout (diffusion_models/, text_encoders/, loras/, vae/), so one copy of a
// model serves Attome's own engine and a ComfyUI that is pointed at the same folder.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "atm/base/error.hpp"
#include "atm/net/http.hpp"

namespace atm::models {

struct CatalogFile {
  std::string url;    // https://…, pinned to a revision so the bytes never change under the hash
  std::string path;   // where it goes under the models folder, with forward slashes: "vae/x.safetensors"
  std::string sha256; // 64 lowercase hex digits; empty = not checked (not allowed in the built-in catalog)
  int64_t size = 0;   // bytes
};

struct CatalogEntry {
  std::string id;      // "minimax-h3.fl2va.turbo8-int8"
  std::string title;   // for the Models panel
  std::string kind;    // "model" or "runtime"
  std::string licence; // one sentence a user should read before downloading
  std::string licence_url;
  std::string notes;
  // What the model does, for the nodes that use it: the kinds, the settings and their ranges (atm_gen reads it; see
  // atm/gen/models.hpp). Null for an entry that is not a model a node can run.
  nlohmann::json declares;
  std::vector<CatalogFile> files;
  int64_t size() const;
};

// {"version": 1, "entries": [{"id", "title", "kind"?, "licence"?, "licence_url"?, "notes"?, "files": [{"url", "path",
// "size", "sha256"?}]}]}. Refused with rule M_CATALOG and the path of the field: an empty or repeated id, no files, a
// URL that is not https, a size below 1, a hash that is not 64 hex digits, and a path that could leave the models folder
// (absolute, a drive letter, a backslash, or a ".." part).
Result<std::vector<CatalogEntry>> parse_catalog(const nlohmann::json &catalog);

// The catalog built into this version of Attome.
const std::vector<CatalogEntry> &builtin_catalog();
const CatalogEntry *find_entry(const std::vector<CatalogEntry> &catalog, std::string_view id);

// ATTOME_MODELS_DIR when set; else %LOCALAPPDATA%\Attome\models on Windows, ~/.local/share/attome/models elsewhere.
std::filesystem::path default_models_dir();

enum class FileState { missing, partial, installed };
struct FileStatus {
  FileState state = FileState::missing;
  int64_t bytes = 0; // on disk: the whole size when installed, the size of the .part file when partial
};
// Installed means a file of the right size under its final name; the hash was checked when it was put there.
FileStatus file_status(const CatalogFile &file, const std::filesystem::path &models_dir);

// Where the whole file is: the models folder first, then each of `also`, the folders the user said already hold models
// (a ComfyUI's models folder, a folder of files they downloaded). In those it is looked for under its catalog path and
// then by its name alone. Empty when it is nowhere. A file in `also` is known by its size only: hashing tens of
// gigabytes at every look is not affordable.
std::filesystem::path find_file(const CatalogFile &file, const std::filesystem::path &models_dir,
                                const std::vector<std::filesystem::path> &also);
// The same question as file_status, with `also` counted: a file found there is installed.
FileStatus file_status(const CatalogFile &file, const std::filesystem::path &models_dir,
                       const std::vector<std::filesystem::path> &also);

struct FetchOptions {
  // Failed tries in a row before a file is given up; each try continues where the last one stopped, and a try that
  // brought bytes starts the count again, so a long download survives many short drops.
  int attempts = 8;
  // The wait before the second try in a row; it doubles with each failure, up to max_retry_delay_ms. With the
  // defaults the tries are spread over about two and a half minutes: a server that is briefly down (502) is waited out.
  int retry_delay_ms = 750;
  int max_retry_delay_ms = 60000;
};

struct FetchProgress {
  std::atomic<int64_t> *done = nullptr;        // bytes of the entry on disk so far, files already installed included
  const std::atomic<bool> *cancel = nullptr;   // set to stop; the .part file stays for the next run
  std::function<void(const std::string &)> on_phase; // "Downloading x (2 of 4)", "Checking x"
  std::atomic<int64_t> *fetched = nullptr;     // bytes that came over the network in this run (for the speed shown)
};

// Fetches what is missing of one file. Errors: M_CANCELLED, M_HTTP_STATUS (the server refused), M_DISK (cannot write:
// the disk is full or the folder is read-only), M_INCOMPLETE (the connection kept dropping), M_HASH (the file is not
// what the catalog says; the .part file is removed), and the transport's N_* errors after the last attempt.
Result<void> fetch_file(net::Transport &transport, const CatalogFile &file, const std::filesystem::path &models_dir,
                        const FetchProgress &progress, const FetchOptions &options = {});
// Every file of an entry, in order. Files already installed, or found whole in one of `also`, are skipped.
Result<void> fetch_entry(net::Transport &transport, const CatalogEntry &entry, const std::filesystem::path &models_dir,
                         const FetchProgress &progress, const FetchOptions &options = {},
                         const std::vector<std::filesystem::path> &also = {});

} // namespace atm::models
