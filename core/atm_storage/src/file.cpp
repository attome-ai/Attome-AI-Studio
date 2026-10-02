#include "atm/storage/file.hpp"

#include <algorithm>
#include <cstring>

#include "atm/base/hash.hpp"
#include "atm/base/profiler.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace atm::storage {
namespace {

namespace fs = std::filesystem;

tl::unexpected<Error> io_error(const char *what, const fs::path &path = {}) {
#if defined(_WIN32)
  const unsigned long code = GetLastError();
#else
  const int code = errno;
#endif
  Error e;
  e.code = ErrorCode::IoError;
  e.rule = "IO";
  e.message = std::string("Could not ") + what + (path.empty() ? std::string() : " \"" + path.string() + "\"") + ".";
  e.hint = "Check that the folder exists, is writable and has free space.";
  e.details = {{"os_error", code}};
  return tl::unexpected(std::move(e));
}

#if defined(_WIN32)
HANDLE handle(intptr_t h) { return reinterpret_cast<HANDLE>(h); }
#else
int fd(intptr_t h) { return int(h); }
#endif

void put_u32(char *p, uint32_t v) {
  p[0] = char(v);
  p[1] = char(v >> 8);
  p[2] = char(v >> 16);
  p[3] = char(v >> 24);
}

uint32_t get_u32(const char *p) {
  const auto *u = reinterpret_cast<const unsigned char *>(p);
  return uint32_t(u[0]) | uint32_t(u[1]) << 8 | uint32_t(u[2]) << 16 | uint32_t(u[3]) << 24;
}

} // namespace

File &File::operator=(File &&other) noexcept {
  if (this != &other) {
    close();
    h_ = other.h_;
    other.h_ = -1;
  }
  return *this;
}

Result<File> File::open(const fs::path &path, Mode mode) {
  File f;
#if defined(_WIN32)
  DWORD access = GENERIC_READ, share = FILE_SHARE_READ, create = OPEN_EXISTING, flags = FILE_ATTRIBUTE_NORMAL;
  switch (mode) {
  case Mode::read:
    share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    break;
  case Mode::append:
    access = GENERIC_READ | GENERIC_WRITE;
    create = OPEN_ALWAYS;
    break;
  case Mode::truncate:
    access = GENERIC_WRITE;
    create = CREATE_ALWAYS;
    break;
  case Mode::lock:
    access = GENERIC_WRITE;
    share = 0;
    create = OPEN_ALWAYS;
    flags |= FILE_FLAG_DELETE_ON_CLOSE;
    break;
  }
  const HANDLE h = CreateFileW(path.c_str(), access, share, nullptr, create, flags, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    if (mode == Mode::lock && GetLastError() == ERROR_SHARING_VIOLATION)
      return fail(ErrorCode::ProjectLocked, "R_PROJECT_LOCKED", "The project is open in another Attome process.",
                  {}, "Use the running daemon (attome --daemon require ...) or stop it with: attome daemon stop");
    if (mode == Mode::read && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND))
      return fail(ErrorCode::NotFound, "IO_NOT_FOUND", "The file \"" + path.string() + "\" does not exist.");
    return io_error("open", path);
  }
  f.h_ = reinterpret_cast<intptr_t>(h);
#else
  int flags = O_RDONLY;
  switch (mode) {
  case Mode::read:
    break;
  case Mode::append:
  case Mode::lock:
    flags = O_RDWR | O_CREAT;
    break;
  case Mode::truncate:
    flags = O_WRONLY | O_CREAT | O_TRUNC;
    break;
  }
  const int d = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
  if (d < 0) {
    if (mode == Mode::read && errno == ENOENT)
      return fail(ErrorCode::NotFound, "IO_NOT_FOUND", "The file \"" + path.string() + "\" does not exist.");
    return io_error("open", path);
  }
  f.h_ = d;
  if (mode == Mode::lock && ::flock(d, LOCK_EX | LOCK_NB) != 0)
    return fail(ErrorCode::ProjectLocked, "R_PROJECT_LOCKED", "The project is open in another Attome process.", {},
                "Use the running daemon (attome --daemon require ...) or stop it with: attome daemon stop");
#endif
  return f;
}

void File::close() {
  if (h_ == -1)
    return;
#if defined(_WIN32)
  CloseHandle(handle(h_));
#else
  ::close(fd(h_));
#endif
  h_ = -1;
}

Result<std::string> File::read_all() {
  std::string out;
#if defined(_WIN32)
  LARGE_INTEGER size{}, zero{};
  if (!GetFileSizeEx(handle(h_), &size) || !SetFilePointerEx(handle(h_), zero, nullptr, FILE_BEGIN))
    return io_error("read a file");
  out.resize(size_t(size.QuadPart));
  size_t done = 0;
  while (done < out.size()) {
    DWORD got = 0;
    const DWORD want = DWORD(std::min<size_t>(out.size() - done, 1u << 30));
    if (!ReadFile(handle(h_), out.data() + done, want, &got, nullptr))
      return io_error("read a file");
    if (got == 0)
      break;
    done += got;
  }
  out.resize(done);
#else
  struct stat st {};
  if (::fstat(fd(h_), &st) != 0 || ::lseek(fd(h_), 0, SEEK_SET) < 0)
    return io_error("read a file");
  out.resize(size_t(st.st_size));
  size_t done = 0;
  while (done < out.size()) {
    const ssize_t got = ::read(fd(h_), out.data() + done, out.size() - done);
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0)
      return io_error("read a file");
    if (got == 0)
      break;
    done += size_t(got);
  }
  out.resize(done);
#endif
  return out;
}

Result<void> File::write(std::string_view data) {
  size_t done = 0;
  while (done < data.size()) {
#if defined(_WIN32)
    DWORD put = 0;
    const DWORD want = DWORD(std::min<size_t>(data.size() - done, 1u << 30));
    if (!WriteFile(handle(h_), data.data() + done, want, &put, nullptr))
      return io_error("write a file");
    done += put;
#else
    const ssize_t put = ::write(fd(h_), data.data() + done, data.size() - done);
    if (put < 0 && errno == EINTR)
      continue;
    if (put < 0)
      return io_error("write a file");
    done += size_t(put);
#endif
  }
  return {};
}

Result<void> File::sync() {
#if defined(_WIN32)
  if (!FlushFileBuffers(handle(h_)))
    return io_error("flush a file to disk");
#else
  if (::fsync(fd(h_)) != 0)
    return io_error("flush a file to disk");
#endif
  return {};
}

Result<void> File::resize(uint64_t size) {
#if defined(_WIN32)
  LARGE_INTEGER pos{};
  pos.QuadPart = LONGLONG(size);
  if (!SetFilePointerEx(handle(h_), pos, nullptr, FILE_BEGIN) || !SetEndOfFile(handle(h_)))
    return io_error("truncate a file");
#else
  if (::ftruncate(fd(h_), off_t(size)) != 0 || ::lseek(fd(h_), off_t(size), SEEK_SET) < 0)
    return io_error("truncate a file");
#endif
  return {};
}

Result<std::string> read_file(const fs::path &path) {
  ATM_TRY(File f, File::open(path, File::Mode::read));
  return f.read_all();
}

Result<void> atomic_write(const fs::path &path, std::string_view data) {
  fs::path tmp = path;
  tmp += ".tmp";
  {
    ATM_TRY(File f, File::open(tmp, File::Mode::truncate));
    ATM_CHECK(f.write(data));
    ATM_PROFILE_SCOPE("storage.fsync");
    ATM_CHECK(f.sync());
  }
#if defined(_WIN32)
  if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    return io_error("replace", path);
#else
  if (::rename(tmp.c_str(), path.c_str()) != 0)
    return io_error("replace", path);
  if (const int dir = ::open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC); dir >= 0) {
    ::fsync(dir); // make the rename durable
    ::close(dir);
  }
#endif
  return {};
}

Result<void> make_dirs(const fs::path &path) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec)
    return fail(ErrorCode::IoError, "IO", "Could not create the folder \"" + path.string() + "\": " + ec.message() + ".");
  return {};
}

bool exists(const fs::path &path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

Result<RecordLog> RecordLog::open(const fs::path &path, std::vector<std::string> *records) {
  RecordLog log;
  ATM_TRY(File f, File::open(path, File::Mode::append));
  log.file_ = std::move(f);
  ATM_TRY(std::string data, log.file_.read_all());
  size_t off = 0;
  while (data.size() - off >= 8) {
    const uint32_t len = get_u32(data.data() + off);
    const uint32_t crc = get_u32(data.data() + off + 4);
    if (len > data.size() - off - 8 || crc32c(data.data() + off + 8, len) != crc)
      break;
    if (records)
      records->emplace_back(data, off + 8, len);
    off += 8 + size_t(len);
  }
  if (off != data.size()) // torn or corrupt tail: everything before it is intact
    ATM_CHECK(log.file_.resize(off));
  return log;
}

Result<void> RecordLog::append(std::string_view payload) {
  frame_.resize(8);
  put_u32(frame_.data(), uint32_t(payload.size()));
  put_u32(frame_.data() + 4, crc32c(payload.data(), payload.size()));
  frame_.append(payload);
  return file_.write(frame_);
}

Result<void> RecordLog::sync() {
  ATM_PROFILE_SCOPE("storage.fsync");
  return file_.sync();
}

Result<void> RecordLog::clear() { return file_.resize(0); }

} // namespace atm::storage
