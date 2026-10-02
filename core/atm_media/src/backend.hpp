#pragma once
// Inside atm_media: what each media backend (Media Foundation, FFmpeg, none) provides behind the shared API.

#include <string>

#include "atm/media/media.hpp"

namespace atm::media {

// probe() for audio and video files; probe() itself answers still pictures first.
Result<MediaInfo> probe_av(const std::string &path);

} // namespace atm::media
