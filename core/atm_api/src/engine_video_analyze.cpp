// video.analyze: the numbers of a finished video's style - its cuts and their rhythm, picture and sound. Read by an agent (the
// niche-from-video skill) or by the editor's "Learn from a video"; the words come from asr.transcribe.
#include "engine_impl.hpp"

namespace atm::api {

  // video.analyze {path, from?, to?}: shot cuts (a cut detector: how much the small grey picture changes between two samples taken 1/15 s
  // apart, against the changes around it), the cut rhythm, brightness and contrast, the main colours, and the sound (audio.analyze) with how
  // many cuts fall on its beat. Reads at most 180 s.
Result<json> Engine::Impl::video_analyze(const json &params) {
  ATM_PROFILE_SCOPE("api.video_analyze");
  const std::string path = params.value("path", std::string());
  if (path.empty())
    return bad_param("path", "is required: the video file to analyse");
  ATM_TRY(json info, media_probe({{"path", path}}));
  if (!info.value("has_video", false))
    return fail(ErrorCode::InvalidArgument, "E_PARAM", "\"" + path + "\" has no picture.");
  const double file_seconds = info.value("seconds", 0.0);
  double from = std::clamp(params.value("from", 0.0), 0.0, std::max(0.0, file_seconds));
  double to = params.value("to", 0.0);
  if (to <= from)
    to = file_seconds;
  to = std::min({to, file_seconds, from + 180.0});
  if (to - from < 0.5)
    return fail(ErrorCode::InvalidArgument, "E_PARAM", "There is less than half a second to analyse.");

  constexpr double kStep = 1.0 / 15.0;
  constexpr int kBox = 96;
  ATM_TRY(auto reader, media::VideoReader::open(path, kBox, kBox));
  const int count = int((to - from) / kStep);
  std::vector<double> time;                  // seconds of the file, per sample
  std::vector<double> change;                // change[i]: from sample i-1 to sample i, mean grey step 0..255
  std::vector<uint8_t> previous, grey;
  double bright_sum = 0.0, contrast_sum = 0.0;
  int measured = 0;
  std::map<uint32_t, int> colours;           // 4 levels per channel
  int colour_samples = 0;
  std::vector<uint8_t> bgrx;
  for (int i = 0; i < count; ++i) {
    const double t = from + double(i) * kStep;
    ATM_TRY(media::FrameView f, reader->frame_at(int64_t(t * double(media::kHnsPerSecond))));
    grey.assign(size_t(f.width) * size_t(f.height), 0);
    for (int y = 0; y < f.height; ++y)
      std::memcpy(&grey[size_t(y) * size_t(f.width)], f.y + size_t(y) * size_t(f.y_pitch), size_t(f.width));
    double sum = 0.0;
    for (uint8_t v : grey)
      sum += v;
    const double mean = sum / double(grey.size());
    if (i % 5 == 0) { // brightness, contrast and colours from every fifth sample
      double var = 0.0;
      for (uint8_t v : grey)
        var += (double(v) - mean) * (double(v) - mean);
      bright_sum += mean;
      contrast_sum += std::sqrt(var / double(grey.size()));
      ++measured;
      if (i % 15 == 0) {
        std::vector<uint8_t> packed(media::nv12_size(f.width, f.height));
        std::memcpy(packed.data(), grey.data(), grey.size());
        for (int y = 0; y < f.height / 2; ++y)
          std::memcpy(&packed[grey.size() + size_t(y) * size_t(f.width)], f.uv + size_t(y) * size_t(f.uv_pitch), size_t(f.width));
        bgrx.assign(size_t(f.width) * size_t(f.height) * 4, 0);
        media::nv12_to_bgrx(packed.data(), f.width, f.height, bgrx.data());
        for (size_t p = 0; p < bgrx.size(); p += 16) { // every fourth pixel
          const uint32_t key = (uint32_t(bgrx[p + 2] >> 6) << 4) | (uint32_t(bgrx[p + 1] >> 6) << 2) | uint32_t(bgrx[p] >> 6);
          ++colours[key];
        }
        ++colour_samples;
      }
    }
    double diff = 0.0;
    if (!previous.empty() && previous.size() == grey.size()) {
      uint64_t acc = 0;
      for (size_t p = 0; p < grey.size(); ++p)
        acc += uint64_t(std::abs(int(grey[p]) - int(previous[p])));
      diff = double(acc) / double(grey.size());
    }
    time.push_back(t);
    change.push_back(diff);
    previous = grey;
  }

  // A cut: the biggest change within two samples either side, large on its own and well above the usual change around it (a fast camera
  // move, a pulse or a shake is not a cut). Tuned on a busy action short (16 of its 18 cuts found) and a plain one (all found).
  std::vector<double> cuts;
  for (size_t i = 1; i < change.size(); ++i) {
    std::vector<double> around;
    for (size_t k = (i > 6 ? i - 6 : 1); k < std::min(change.size(), i + 7); ++k)
      if (k != i)
        around.push_back(change[k]);
    std::sort(around.begin(), around.end());
    const double usual = around.empty() ? 0.0 : around[around.size() / 2];
    double local = 0.0;
    for (size_t k = (i > 2 ? i - 2 : 1); k < std::min(change.size(), i + 3); ++k)
      local = std::max(local, change[k]);
    if (change[i] > 30.0 && change[i] > 1.8 * usual && change[i] >= local && (cuts.empty() || time[i] - cuts.back() >= 0.25))
      cuts.push_back(time[i]);
  }
  json cut_times = json::array();
  for (double c : cuts)
    cut_times.push_back(std::round(c * 100.0) / 100.0);
  std::vector<double> shots;
  double last = from;
  for (double c : cuts) {
    shots.push_back(c - last);
    last = c;
  }
  shots.push_back(to - last);
  const double span = to - from;
  json rhythm = {{"cuts", int(cuts.size())}, {"shots", int(shots.size())}, {"cuts_per_second", std::round(double(cuts.size()) / span * 100.0) / 100.0},
                 {"average_shot", std::round(span / double(shots.size()) * 100.0) / 100.0},
                 {"shortest_shot", std::round(*std::min_element(shots.begin(), shots.end()) * 100.0) / 100.0},
                 {"longest_shot", std::round(*std::max_element(shots.begin(), shots.end()) * 100.0) / 100.0}};

  json out = {{"path", path}, {"from", from}, {"to", to}, {"seconds", span}, {"width", info.value("width", 0)}, {"height", info.value("height", 0)},
              {"rate", info.value("rate", std::string())}, {"cut_times", std::move(cut_times)}, {"rhythm", std::move(rhythm)}};
  if (params.value("series", false)) { // the change between samples, for tuning and for checking
    json series = json::array();
    for (double c : change)
      series.push_back(std::round(c * 10.0) / 10.0);
    out["change_per_sample"] = std::move(series);
  }
  out["brightness"] = measured ? std::round(bright_sum / double(measured) / 255.0 * 100.0) / 100.0 : 0.0; // 0 black .. 1 white
  out["contrast"] = measured ? std::round(contrast_sum / double(measured) / 128.0 * 100.0) / 100.0 : 0.0;  // 0 flat .. about 1 harsh

  std::vector<std::pair<int, uint32_t>> ranked;
  int total = 0;
  for (const auto &[key, n] : colours) {
    ranked.push_back({n, key});
    total += n;
  }
  std::sort(ranked.rbegin(), ranked.rend());
  json palette = json::array();
  for (size_t i = 0; i < ranked.size() && palette.size() < 5; ++i) {
    const uint32_t key = ranked[i].second;
    char hex[16];
    std::snprintf(hex, sizeof hex, "#%02X%02X%02X", ((key >> 4) & 3) * 85, ((key >> 2) & 3) * 85, (key & 3) * 85);
    palette.push_back({{"colour", hex}, {"share", std::round(double(ranked[i].first) / double(std::max(1, total)) * 100.0) / 100.0}});
  }
  out["palette"] = std::move(palette);

  if (info.value("has_audio", false)) {
    auto sound = audio_analyze({{"path", path}, {"from", from}, {"to", to}});
    if (sound) {
      json s = {{"lufs", (*sound).value("lufs", 0.0)}, {"peak_db", (*sound).value("peak_db", 0.0)}, {"has_beat", (*sound).value("has_beat", false)}};
      if ((*sound).value("has_beat", false)) {
        s["bpm"] = (*sound)["bpm"];
        const double period = (*sound).value("beat_period", 0.0), first = (*sound).value("first_beat", 0.0);
        int on_beat = 0;
        for (double c : cuts) {
          double k = std::fmod(c - first, period);
          if (k < 0.0)
            k += period;
          if (std::min(k, period - k) <= 0.06) // within 60 ms of a beat
            ++on_beat;
        }
        s["cuts_on_beat"] = cuts.empty() ? 0.0 : std::round(double(on_beat) / double(cuts.size()) * 100.0) / 100.0;
      }
      out["sound"] = std::move(s);
    }
  }
  return out;
}

  // video.extract_audio {path, project?, output?, from?, to?}: the sound of a video (or sound) file, as its own WAV - a niche often fits
  // one piece of music or one voice, not just its pace, so this keeps the actual sound to reuse. With `project` and no `output` it is
  // imported as the project's own asset (asset_id comes back, ready for timeline.edit add_clip); with `output` it is written there instead.
  // At most 10 minutes; from..to (seconds) take a part of it.
Result<json> Engine::Impl::video_extract_audio(const json &params) {
  ATM_PROFILE_SCOPE("api.video_extract_audio");
  const std::string path = params.value("path", std::string());
  if (path.empty())
    return bad_param("path", "is required: the video or sound file to take the sound from");
  const std::string output = params.value("output", std::string());
  if (output.empty() && !params.contains("project"))
    return bad_param("output", "is required (a file to write) unless project is given");
  ATM_TRY(json info, media_probe({{"path", path}}));
  if (!info.value("has_audio", false))
    return fail(ErrorCode::InvalidArgument, "E_PARAM", "\"" + path + "\" has no sound.");
  const double file_seconds = info.value("seconds", 0.0);
  double from = std::clamp(params.value("from", 0.0), 0.0, std::max(0.0, file_seconds));
  double to = params.value("to", 0.0);
  if (to <= from)
    to = file_seconds;
  to = std::min({to, file_seconds, from + 600.0});
  if (to - from < 0.1)
    return fail(ErrorCode::InvalidArgument, "E_PARAM", "There is less than a tenth of a second of sound to take.");
  ATM_TRY(std::vector<float> pcm, media::read_audio(path, int64_t(from * double(media::kHnsPerSecond)), int64_t((to - from) * double(media::kHnsPerSecond))));
  const audio::Level level = audio::measure(pcm);
  std::string dest = output;
  std::optional<std::string> imported;
  if (dest.empty()) {
    ATM_TRY(Project *pr, project(params));
    const std::u8string stem = to_path(path).stem().u8string();
    dest = to_utf8(pr->dir / ".attome" / "audio" / (std::string(stem.begin(), stem.end()) + "_sound.wav"));
    ATM_CHECK(audio::write_wav(dest, pcm));
    ATM_TRY(json added, media_import({{"project", params["project"]}, {"paths", json::array({dest})}}));
    if (added.contains("assets") && added["assets"].is_array() && !added["assets"].empty())
      imported = added["assets"][0].value("id", std::string());
  } else {
    ATM_CHECK(audio::write_wav(dest, pcm));
  }
  json out = {{"path", dest}, {"seconds", double(pcm.size() / 2) / double(media::kAudioRate)}, {"peak_db", level.peak_db}, {"lufs", level.lufs}};
  if (imported)
    out["asset_id"] = *imported;
  return out;
}

} // namespace atm::api
