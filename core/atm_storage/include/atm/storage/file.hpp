#pragma once
// M1 storage: the only place that touches the file system. Atomic writes, the project lock, and the
// CRC-framed append-only record log used by the journal.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "atm/base/error.hpp"

namespace atm::storage {

class File {
public:
  enum class Mode {
    read,     // existing file, read only
    append,   // read + write, created if missing; used by the record log
    truncate, // write, replaces the file
    lock      // exclusive handle that the OS releases when the process ends
  };
  static Result<File> open(const std::filesystem::path &path, Mode mode);

  File() = default;
  File(File &&other) noexcept : h_(other.h_) { other.h_ = -1; }
  File &operator=(File &&other) noexcept;
  File(const File &) = delete;
  File &operator=(const File &) = delete;
  ~File() { close(); }

  bool is_open() const { return h_ != -1; }
  Result<std::string> read_all();     // leaves the position at the end
  Result<void> write(std::string_view data);
  Result<void> sync();                // fsync / FlushFileBuffers
  Result<void> resize(uint64_t size); // truncates and moves the position to `size`
  void close();

private:
  intptr_t h_ = -1;
};

Result<std::string> read_file(const std::filesystem::path &path);
// Write to a temporary file, fsync, then rename over the target.
Result<void> atomic_write(const std::filesystem::path &path, std::string_view data);
Result<void> make_dirs(const std::filesystem::path &path);
bool exists(const std::filesystem::path &path);

// Record log: [u32 length][u32 crc32c(payload)][payload], little-endian. A torn tail is cut off on open.
class RecordLog {
public:
  static Result<RecordLog> open(const std::filesystem::path &path, std::vector<std::string> *records);
  Result<void> append(std::string_view payload); // buffered by the OS until sync()
  Result<void> sync();
  Result<void> clear(); // start a new epoch
  bool is_open() const { return file_.is_open(); }

private:
  File file_;
  std::string frame_;
};

} // namespace atm::storage
