#pragma once
// One picture of a video, as a JPEG: the Get Frame node. Runs here, with no model.

#include <string>

namespace atm::api {

// The frame at `at_seconds` into the video, or the last frame when it is negative (a time past the end gives the last
// frame too). False when the file cannot be read as a video.
bool write_frame(const std::string &video, const std::string &jpeg, double at_seconds);

} // namespace atm::api
