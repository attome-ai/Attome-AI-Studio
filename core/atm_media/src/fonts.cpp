// Where the bundled fonts live, for the portable text backend and the editor's interface.

#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

#include "atm/media/media.hpp"

namespace atm::media {
namespace {

namespace fs = std::filesystem;

fs::path exe_dir() {
#if defined(_WIN32)
  wchar_t self[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
  if (n == 0 || n >= std::size(self))
    return {};
  return fs::path(self).parent_path();
#else
  char self[4096] = {};
#if defined(__APPLE__)
  uint32_t size = sizeof self;
  if (_NSGetExecutablePath(self, &size) != 0)
    return {};
#else
  if (::readlink("/proc/self/exe", self, sizeof self - 1) <= 0)
    return {};
#endif
  return fs::path(self).parent_path();
#endif
}

} // namespace

std::string font_dir() {
  static const std::string found = [] {
    std::vector<fs::path> dirs;
    if (const char *env = std::getenv("ATTOME_FONTS"); env && *env)
      dirs.emplace_back(env);
    if (const fs::path exe = exe_dir(); !exe.empty()) {
      dirs.push_back(exe / "fonts");
      dirs.push_back(exe.parent_path() / "share" / "attome" / "fonts");
    }
    for (const fs::path &dir : dirs) {
      std::error_code ec;
      if (fs::is_regular_file(dir / "NotoSans-Regular.ttf", ec)) {
        const std::u8string u8 = dir.u8string();
        return std::string(u8.begin(), u8.end());
      }
    }
    return std::string();
  }();
  return found;
}

} // namespace atm::media
