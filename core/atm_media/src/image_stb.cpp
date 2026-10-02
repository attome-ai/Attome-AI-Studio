#if defined(ATM_IMAGE_STB)
// Still images for agents (see.frames, see.contact_sheet) on every system, written with stb_image_write (public
// domain). The Windows build uses the Windows Imaging Component by default (ATM_IMAGE_BACKEND).

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "atm/base/profiler.hpp"
#include "atm/media/media.hpp"

namespace atm::media {

Result<void> write_jpeg(const std::string &path, const uint8_t *bgrx, int width, int height, float quality) {
  ATM_PROFILE_SCOPE("image.jpeg");
  const auto failed = [&](const char *step) {
    return fail(ErrorCode::IoError, "M_IMAGE_WRITE", std::string("Could not write the image \"") + path + "\" (" +
                                                         step + ").");
  };
  if (width <= 0 || height <= 0)
    return failed("size");
  std::vector<uint8_t> rgb(size_t(width) * size_t(height) * 3);
  for (size_t i = 0, n = size_t(width) * size_t(height); i < n; ++i) {
    rgb[i * 3 + 0] = bgrx[i * 4 + 2];
    rgb[i * 3 + 1] = bgrx[i * 4 + 1];
    rgb[i * 3 + 2] = bgrx[i * 4 + 0];
  }
  // The path is UTF-8; as a u8string it opens the right file on Windows too.
  std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
  if (!out)
    return failed("open");
  const int q = std::clamp(int(std::lround(quality * 100.0f)), 1, 100);
  const auto put = [](void *context, void *data, int size) {
    static_cast<std::ofstream *>(context)->write(static_cast<const char *>(data), size);
  };
  if (!stbi_write_jpg_to_func(put, &out, width, height, 3, rgb.data(), q))
    return failed("encode");
  out.close();
  if (!out)
    return failed("write");
  return {};
}

} // namespace atm::media
#endif
