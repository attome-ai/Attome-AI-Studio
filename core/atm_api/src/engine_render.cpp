// Looking at and exporting a sequence: render.*, see.* (frames, contact sheet) and media.probe.
#include "engine_impl.hpp"

namespace atm::api {

Result<json> Engine::Impl::media_probe(const json &params) {
  ATM_TRY(const std::string *path, string_param(params, "path"));
  ATM_TRY(media::MediaInfo info, media::probe(*path));
  if (info.is_image) // a still: any length on the timeline
    return json{{"path", *path},           {"has_video", true},      {"has_audio", false},
                {"image", true},           {"width", info.width},    {"height", info.height}};
  ATM_TRY(Rational duration, Rational::make(info.duration_hns, media::kHnsPerSecond));
  json out = {{"path", *path},           {"has_video", info.has_video}, {"has_audio", info.has_audio},
              {"duration", duration.to_string()}, {"seconds", duration.to_seconds_lossy()}};
  if (info.has_video) {
    ATM_TRY(Rational rate, Rational::make(info.rate_num, info.rate_den));
    out["width"] = info.width;
    out["height"] = info.height;
    out["rate"] = rate.to_string();
  }
  if (info.has_audio) {
    out["audio_rate"] = info.audio_rate;
    out["audio_channels"] = info.audio_channels;
  }
  return out;
}

// The generative clips of the part being exported that are not made yet (no Take: nothing of their own is drawn) or out of date
// (their last Take is drawn), so whoever exports can make them first: render.sequence answers with them as "not_made".
// A clip that is not made draws nothing, so where it is comes from its timing, not from the renderer's layers.
json Engine::Impl::unmade_clips(const Project &pr, const std::string &sequence, Rational rate, int64_t first, int64_t last) const {
  std::map<std::string, std::pair<int64_t, int64_t>> spans; // the exported sequence's clips, in frames
  const auto frames_of = [&](const json &timing, const char *key) {
    return Rational::parse(timing.value(key, std::string("0"))).and_then([&](Rational t) { return to_frames(t, rate, Round::nearest_even); }).value_or(0);
  };
  for_each_clip(pr.doc.root(), [&](const std::string &id, const json &c, const std::string &, const json &) {
    if (sequence_holding(pr.doc.root(), id) != sequence)
      return;
    const json timing = c.value("timing", json::object());
    const int64_t in = frames_of(timing, "record_in");
    spans[id] = {in, in + frames_of(timing, "duration")};
  });
  json out = json::array();
  for (const gen::ClipPlan &p : gen_plan(pr, {})) {
    if (p.state != gen::ClipState::empty && p.state != gen::ClipState::dirty)
      continue;
    if (const auto s = spans.find(p.id); s == spans.end() || s->second.second <= first || s->second.first >= last)
      continue; // in another sequence, or outside the part
    out.push_back({{"clip", p.id}, {"name", p.name}, {"state", p.state == gen::ClipState::empty ? "not made" : "out of date"}});
  }
  return out;
}

Result<json> Engine::Impl::render_sequence(const json &params) {
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *output, string_param(params, "output"));
  std::string sequence = params.value("sequence", std::string()); // the one drawn: the first when none is named
  if (const json &order = pr->doc.root().value("sequence_order", json::array()); sequence.empty() && !order.empty() && order[0].is_string())
    sequence = order[0].get<std::string>();
  // "rate": the film made at another frame rate. It is compiled with that rate, so every time becomes frames of it and a source plays
  // at its own times: 60 from 60 fps footage keeps all its frames, 24 from 30 leaves out evenly the ones between.
  const json *root = &pr->doc.root();
  json retimed;
  if (const auto r = params.find("rate"); r != params.end() && !r->is_null()) {
    const auto rate = r->is_number() ? Rational::make(std::llround(r->get<double>() * 1000.0), 1000)
                      : r->is_string() ? Rational::parse(r->get<std::string>()) : Result<Rational>(Rational());
    if (!rate || rate->num() <= 0 || rate->to_seconds_lossy() > 240.0)
      return bad_param("rate", "is a frame rate: 24, 25, 30, 50, 60, or \"30000/1001\"");
    retimed = pr->doc.root();
    if (!retimed["sequences"].contains(sequence))
      return fail(ErrorCode::NotFound, "R_SEQUENCE", "The project has no sequence \"" + sequence + "\".");
    retimed["sequences"][sequence]["rate"] = rate->to_string();
    root = &retimed;
  }
  ATM_TRY(render::Composition comp, render::compile(*root, sequence, to_utf8(pr->dir)));
  if (comp.frames <= 0)
    return fail(ErrorCode::InvalidArgument, "R_EMPTY", "The sequence has no media clips to render.", {},
                "Add a clip whose media_ref is {\"type\": \"file\", \"path\": …} first.");
  const std::string format = params.value("format", std::string("mp4"));
  if (format != "mp4" && format != "prores" && format != "dnxhr" && format != "wav" && format != "jpeg" && format != "png_sequence")
    return bad_param("format", "is \"mp4\" (video, the default), \"prores\" or \"dnxhr\" (a .mov made by your own FFmpeg: see media.codecs), \"wav\" (the sound only), \"jpeg\" (a picture of the frame at \"from\") or \"png_sequence\" (a numbered PNG for each frame, in the folder \"output\")");
  // The part to export: "from" (the first frame, default the start) and "to" (where it ends, that frame not drawn, default the end).
  const Rational frame_rate = *Rational::make(comp.rate_num, comp.rate_den);
  int64_t first = 0, last = comp.frames;
  for (const char *key : {"from", "to"}) {
    if (!params.contains(key) || params[key].is_null())
      continue;
    ATM_TRY(RationalTime t, parse_time(params[key], {.rate = frame_rate}));
    ATM_TRY(int64_t f, to_frames(t, frame_rate, Round::floor));
    (std::string(key) == "from" ? first : last) = f;
  }
  first = std::clamp<int64_t>(first, 0, comp.frames - 1);
  last = std::clamp<int64_t>(last, first + 1, comp.frames);
  json not_made = unmade_clips(*pr, sequence, frame_rate, first, last);
  if (format == "png_sequence") { // a folder of numbered pictures: `output` is the folder
    const fs::path folder = fs::absolute(to_path(*output));
    if (folder.extension() == ".png" || folder.extension() == ".mp4")
      return bad_param("output", "is the folder the pictures go in for png_sequence, not a file");
    if (!params.value("overwrite", true) && storage::exists(folder / (to_utf8(folder.filename()) + "_" + [&] {
                                                              char n[16];
                                                              std::snprintf(n, sizeof n, "%06lld", static_cast<long long>(first));
                                                              return std::string(n);
                                                            }() + ".png")))
      return fail(ErrorCode::OutputExists, "R_EXISTS", "\"" + to_utf8(folder) + "\" already holds these pictures.", {}, "Pass \"overwrite\": true or choose another folder.");
    ATM_CHECK(storage::make_dirs(folder));
    const int h = params.value("height", comp.height);
    const int w = params.contains("width") ? params.value("width", comp.width) : int(int64_t(comp.width) * h / std::max(1, comp.height));
    if (w < 16 || h < 16 || w > 16384 || h > 16384)
      return bad_param("height", "gives a size outside 16 x 16 … 16384 x 16384");
    auto job = std::make_shared<Job>();
    job->id = new_id("job");
    job->kind = "render.sequence";
    job->output = to_utf8(folder);
    job->units_total.store(last - first);
    jobs[job->id] = job;
    const std::string name = to_utf8(folder.filename());
    const int device = gpu_device_for(comp);
    job->thread = std::thread(run_png_sequence, job, std::move(comp), to_utf8(folder), name, first, last, w, h, device, gpu_pool);
    return json{{"job_id", job->id}, {"output", job->output}, {"frames", last - first}, {"format", format}, {"first_frame", first},
                {"pattern", name + "_%06d.png"}, {"width", w & ~1}, {"height", h & ~1}, {"not_made", std::move(not_made)}};
  }
  const bool piped = format == "prores" || format == "dnxhr"; // written by the user's FFmpeg, as .mov
  const char *extension = format == "mp4" ? ".mp4" : piped ? ".mov" : format == "wav" ? ".wav" : ".jpg";
  fs::path out_path = fs::absolute(to_path(*output));
  if (format == "jpeg" && out_path.extension() == ".jpeg")
    out_path.replace_extension(".jpg");
  if (out_path.extension() != extension)
    out_path += extension;
  if (!params.value("overwrite", true) && storage::exists(out_path))
    return fail(ErrorCode::OutputExists, "R_EXISTS", "\"" + to_utf8(out_path) + "\" already exists.", {},
                "Pass \"overwrite\": true or choose another name.");
  ATM_CHECK(storage::make_dirs(out_path.parent_path()));

  media::EncodeSettings settings;
  settings.path = to_utf8(out_path);
  settings.height = params.value("height", comp.height);
  settings.width = params.contains("width")
                       ? params.value("width", comp.width)
                       : int(int64_t(comp.width) * settings.height / std::max(1, comp.height));
  settings.rate_num = comp.rate_num;
  settings.rate_den = comp.rate_den;
  settings.audio = params.value("audio", true);
  // About 12 Mbit/s at 1080p30, scaled with the picture size.
  settings.bitrate = params.value("bitrate", std::max(1'000'000, settings.width * settings.height * 6));
  if (piped) {
    settings.codec = format;
    settings.profile = params.value("profile", std::string());
    static const std::vector<std::string> kProres = {"proxy", "lt", "standard", "hq", "4444", "4444xq"}, kDnxhr = {"lb", "sq", "hq", "hqx", "444"};
    const auto &known = format == "prores" ? kProres : kDnxhr;
    if (!settings.profile.empty() && std::find(known.begin(), known.end(), settings.profile) == known.end()) {
      std::string list;
      for (const std::string &k : known)
        list += (list.empty() ? "" : ", ") + k;
      return bad_param("profile", ("must be one of " + list + " for " + format).c_str());
    }
    settings.ffmpeg = this->settings().value("ffmpeg_path", std::string());
    ATM_TRY(media::FfmpegInfo found, media::find_ffmpeg(settings.ffmpeg)); // fails now, not later in the job, when there is none
    (void)found;
    if (settings.width < 16 || settings.height < 16 || settings.width > 8192 || settings.height > 4608)
      return bad_param("height", "gives a size outside 16 x 16 … 8192 x 4608");
  } else if (!media::h264_size_ok(settings.width, settings.height)) {
    return bad_param("height", "gives a size the H.264 encoder does not take (each side 16 … 4096, at most 4096 x 2304 pixels)");
  }

  auto job = std::make_shared<Job>();
  job->id = new_id("job");
  job->kind = "render.sequence";
  job->output = settings.path;
  jobs[job->id] = job;
  if (format != "mp4" && !piped) {
    job->units_total.store(1);
    job->thread = std::thread(run_extract, job, std::move(comp), settings.path, format == "wav", first, last, settings.width, settings.height);
    return json{{"job_id", job->id}, {"output", job->output}, {"frames", last - first}, {"format", format}, {"not_made", std::move(not_made)}};
  }
  job->units_total.store(last - first);
  const json device = render_device_now();
  const int gpu_device = gpu_device_for(comp);
  job->thread = std::thread(run_export, job, std::move(comp), settings, first, last, gpu_device, gpu_pool);
  json answer{{"job_id", job->id}, {"output", job->output}, {"frames", job->units_total.load()},
              {"width", settings.width & ~1}, {"height", settings.height & ~1}};
  answer["render_device"] = device; // the device chosen; jobs.get says where the effects really ran (rendered_on)
  answer["not_made"] = std::move(not_made);
  return answer;
}

// media.codecs {ffmpeg_path?}: the formats this machine can write. mp4 (H.264) uses the operating system's encoder; prores and dnxhr need an FFmpeg program
// of the user's own (found on the PATH, in ATTOME_FFMPEG or at the saved ffmpeg_path). With ffmpeg_path that program is remembered ("" forgets it).
Result<json> Engine::Impl::media_codecs(const json &params) {
  ATM_PROFILE_SCOPE("api.media_codecs");
  if (params.contains("ffmpeg_path") && params["ffmpeg_path"].is_string()) {
    const std::string chosen = params["ffmpeg_path"].get<std::string>();
    if (!chosen.empty() && !fs::exists(to_path(chosen)))
      return bad_param("ffmpeg_path", "is not a file");
    if (const fs::path path = settings_path(); !path.empty()) {
      json all = settings();
      if (chosen.empty())
        all.erase("ffmpeg_path");
      else
        all["ffmpeg_path"] = chosen;
      ATM_CHECK(storage::make_dirs(path.parent_path()));
      ATM_CHECK(storage::atomic_write(path, all.dump(2) + "\n"));
    }
    saved_ffmpeg = chosen;
  }
  const std::string configured = params.contains("ffmpeg_path") ? saved_ffmpeg : settings().value("ffmpeg_path", saved_ffmpeg);
  json out = {{"formats", {{"mp4", true}, {"wav", true}, {"jpeg", true}, {"png_sequence", true}, {"prores", false}, {"dnxhr", false}}}};
  const auto found = media::find_ffmpeg(configured);
  if (!found) {
    out["ffmpeg"] = {{"found", false}, {"hint", found.error().hint}};
    return out;
  }
  out["ffmpeg"] = {{"found", true}, {"path", found->path}, {"version", found->version}, {"license", found->license}, {"encoders", found->encoders}};
  for (const std::string &e : found->encoders)
    out["formats"][e == "prores_ks" ? "prores" : "dnxhr"] = true;
  return out;
}

  // Renders `frames` at width x height as packed BGRX pictures.
Result<std::vector<std::vector<uint8_t>>> Engine::Impl::render_stills(render::Composition comp, const std::vector<int64_t> &frames,
                                                               int width, int height, std::string *warning){
  render::Renderer renderer(std::move(comp), width, height);
  renderer.use_gpu(gpu_for_stills());
  std::vector<uint8_t> nv12(media::nv12_size(renderer.width(), renderer.height()));
  std::vector<std::vector<uint8_t>> out;
  for (const int64_t f : frames) {
    ATM_CHECK(renderer.render(f, nv12.data()));
    std::vector<uint8_t> &bgrx = out.emplace_back(size_t(renderer.width()) * size_t(renderer.height()) * 4);
    media::nv12_to_bgrx(nv12.data(), renderer.width(), renderer.height(), bgrx.data());
  }
  *warning = renderer.take_warning();
  return out;
}

  // Shared set-up: the compiled sequence and an emptied output folder. Earlier pictures are removed, so the folder
  // never grows; a client reads the files before its next see.* call.
Result<std::pair<render::Composition, fs::path>> Engine::Impl::see_setup(Project &pr, const json &params) {
  ATM_TRY(render::Composition comp, render::compile(pr.doc.root(), params.value("sequence", std::string()), to_utf8(pr.dir)));
  if (comp.frames <= 0)
    return fail(ErrorCode::InvalidArgument, "R_EMPTY", "The sequence is empty, so there is nothing to see.", {},
                "Add clips with project.patch first.");
  const fs::path dir = pr.dir / ".attome" / "see";
  ATM_CHECK(storage::make_dirs(dir));
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(dir, ec))
    if (entry.path().extension() == ".jpg")
      fs::remove(entry.path(), ec);
  return std::pair{std::move(comp), dir};
}

json Engine::Impl::time_of(int64_t frame, const render::Composition &comp) {
  const Rational rate = *Rational::make(comp.rate_num, comp.rate_den);
  const auto t = from_frames(frame, rate);
  json out = t ? to_json(format_time(*t, rate)) : json::object();
  out["frame"] = frame;
  return out;
}

Result<json> Engine::Impl::see_frames(const json &params) {
  ATM_TRY(Project *pr, project(params));
  const auto times = params.find("times");
  if (times == params.end() || !times->is_array() || times->empty() || times->size() > 16)
    return bad_param("times", "is required: 1 to 16 times such as \"2.5s\", \"75@30\" or \"00:00:02:15\"");
  const int height = params.value("height", 540);
  if (height < 64 || height > 2160)
    return bad_param("height", "must be between 64 and 2160 pixels");
  ATM_TRY(auto setup, see_setup(*pr, params));
  auto &[comp, dir] = setup;
  const Rational rate = *Rational::make(comp.rate_num, comp.rate_den);
  std::vector<int64_t> frames;
  for (const json &value : *times) {
    ATM_TRY(RationalTime t, parse_time(value, {.rate = rate}));
    ATM_TRY(int64_t f, to_frames(t, rate, Round::floor));
    frames.push_back(std::clamp<int64_t>(f, 0, comp.frames - 1)); // a time past the end shows the last frame
  }
  const int width = int(int64_t(comp.width) * height / std::max(1, comp.height));
  std::string warning;
  json images = json::array();
  {
    ATM_TRY(auto stills, render_stills(comp, frames, width, height, &warning));
    for (size_t i = 0; i < frames.size(); ++i) {
      const fs::path file = dir / ("r" + std::to_string(pr->revision) + "-f" + std::to_string(frames[i]) + ".jpg");
      ATM_CHECK(media::write_jpeg(to_utf8(file), stills[i].data(), width & ~1, height & ~1));
      json image = time_of(frames[i], comp);
      image["path"] = to_utf8(file);
      images.push_back(std::move(image));
    }
  }
  json out = {{"images", std::move(images)}, {"width", width & ~1}, {"height", height & ~1},
              {"duration", time_of(comp.frames, comp)}, {"revision", pr->revision}};
  if (!warning.empty())
    out["warning"] = warning;
  return out;
}

  // One JPEG with `count` evenly spaced frames in a grid, each with its timecode underneath.
Result<json> Engine::Impl::see_contact_sheet(const json &params) {
  ATM_TRY(Project *pr, project(params));
  const int count = params.value("count", 12), columns = std::clamp(params.value("columns", 4), 1, 8);
  const int tile_h = params.value("tile_height", 180);
  if (count < 1 || count > 48)
    return bad_param("count", "must be between 1 and 48");
  if (tile_h < 64 || tile_h > 540)
    return bad_param("tile_height", "must be between 64 and 540 pixels");
  ATM_TRY(auto setup, see_setup(*pr, params));
  auto &[comp, dir] = setup;
  std::vector<int64_t> frames; // the middle of each of `count` equal parts
  for (int i = 0; i < count; ++i)
    frames.push_back(std::min(comp.frames - 1, (2 * i + 1) * comp.frames / (2 * count)));
  frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
  const int tile_w = int(int64_t(comp.width) * tile_h / std::max(1, comp.height)) & ~1;
  const int th = tile_h & ~1, label_h = std::max(16, th / 7), gap = 4;
  const int n = int(frames.size()), cols = std::min(columns, n), rows = (n + cols - 1) / cols;
  const int sheet_w = cols * tile_w + (cols + 1) * gap, sheet_h = rows * (th + label_h) + (rows + 1) * gap;
  std::vector<uint8_t> sheet(size_t(sheet_w) * size_t(sheet_h) * 4);
  for (size_t i = 0; i < sheet.size(); i += 4) { // dark grey background
    sheet[i] = sheet[i + 1] = sheet[i + 2] = 24;
    sheet[i + 3] = 255;
  }
  std::string warning;
  ATM_TRY(auto stills, render_stills(comp, frames, tile_w, th, &warning));
  json tiles = json::array();
  for (int i = 0; i < n; ++i) {
    const int x0 = gap + (i % cols) * (tile_w + gap), y0 = gap + (i / cols) * (th + label_h + gap);
    for (int y = 0; y < th; ++y)
      std::memcpy(&sheet[(size_t(y0 + y) * size_t(sheet_w) + size_t(x0)) * 4],
                  &stills[size_t(i)][size_t(y) * size_t(tile_w) * 4], size_t(tile_w) * 4);
    json tile = time_of(frames[size_t(i)], comp);
    if (auto label = media::render_text(tile.value("timecode", ""), float(label_h) * 0.7f, false, tile_w)) {
      const int lx = x0 + std::max(0, (tile_w - label->width) / 2), ly = y0 + th + (label_h - label->height) / 2;
      for (int y = 0; y < label->height; ++y)
        for (int x = 0; x < label->width; ++x) {
          const int sx = lx + x, sy = ly + y;
          if (sx < 0 || sy < 0 || sx >= sheet_w || sy >= sheet_h)
            continue;
          uint8_t *px = &sheet[(size_t(sy) * size_t(sheet_w) + size_t(sx)) * 4];
          const int a = label->alpha[size_t(y) * size_t(label->width) + size_t(x)];
          for (int c = 0; c < 3; ++c)
            px[c] = uint8_t(px[c] + (230 - px[c]) * a / 255);
        }
    }
    tiles.push_back(std::move(tile));
  }
  const fs::path file = dir / ("r" + std::to_string(pr->revision) + "-sheet.jpg");
  ATM_CHECK(media::write_jpeg(to_utf8(file), sheet.data(), sheet_w, sheet_h));
  json out = {{"images", json::array({{{"path", to_utf8(file)}}})},
              {"tiles", std::move(tiles)},
              {"columns", cols},
              {"width", sheet_w},
              {"height", sheet_h},
              {"duration", time_of(comp.frames, comp)},
              {"revision", pr->revision}};
  if (!warning.empty())
    out["warning"] = warning;
  return out;
}

} // namespace atm::api
