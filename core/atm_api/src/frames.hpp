#pragma once
// One picture of a video, as a JPEG: the Get Frame node. Runs here, with no model.

#include <string>

namespace atm::api {

// The frame at `at_seconds` into the video, or the last frame when it is negative (a time past the end gives the last
// frame too). False when the file cannot be read as a video.
bool write_frame(const std::string &video, const std::string &jpeg, double at_seconds);

// The length of a video or a sound in seconds, written as text to `path`. False when the file cannot be read.
// A part of a picture, or of the frame of a video at `at_seconds`, as a PNG: x, y, w, h are fractions of the picture (0..1).
bool write_crop(const std::string &source, const std::string &png, double at_seconds, double x, double y, double w, double h);
// The same as write_frame for a part of the picture (x, y, w, h: fractions of it), brought back to the size of the whole frame: a
// closer view of the same moment, for a shot that starts where another shot is.
bool write_frame_part(const std::string &video, const std::string &jpeg, double at_seconds, double x, double y, double w, double h);
bool write_duration(const std::string &media, const std::string &path);

} // namespace atm::api
