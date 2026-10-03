// atm_bench: local performance check against the targets in the plan (F0 §5.1, §5.5, §5.6), followed by the zone
// profile of the run, so every slow number comes with the place the time went. Never gates CI.
//
//   atm_bench [--clips 10000] [--patches 2000]
//   atm_bench --export          only the export scenes of F1 §7.5 (writes test clips first)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "atm/api/engine.hpp"
#include "atm/api/server.hpp"
#include "atm/base/id.hpp"
#include "atm/base/profiler.hpp"
#include "atm/base/rational.hpp"
#include "atm/media/media.hpp"
#include "atm/render/render.hpp"

namespace {

using atm::api::Engine;
using atm::api::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

struct Series {
  std::vector<double> us;
  template <class F> void time(F &&f) {
    const auto t0 = Clock::now();
    f();
    us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
  }
  double pct(double p) {
    std::sort(us.begin(), us.end());
    return us.empty() ? 0.0 : us[std::min(us.size() - 1, size_t(p * double(us.size())))];
  }
};

int g_failed = 0;

void report(const char *what, double value, const char *unit, double target) {
  const bool ok = value <= target;
  g_failed += ok ? 0 : 1;
  std::printf("  %-52s %12.3f %-3s  target <= %-9g %s\n", what, value, unit, target, ok ? "ok" : "SLOW");
}

json must(Engine &e, const char *tool, const json &params) {
  auto r = e.call(tool, params);
  if (!r) {
    std::fprintf(stderr, "%s failed: %s\n", tool, r.error().message.c_str());
    std::exit(1);
  }
  return std::move(*r);
}

json clip_value(int index) {
  return {{"name", "clip " + std::to_string(index)},
          {"timing", {{"record_in", std::to_string(index * 2) + "s"}, {"duration", "2s"}, {"source_in", "0s"}}},
          {"media_ref", {{"type", "file"}, {"path", "media/a.mov"}, {"rate", "30000/1001"}}},
          {"transform", {{"position", {0.5, 0.5}}, {"scale", {1, 1}}, {"rotation", "0"}, {"opacity", 1.0}}}};
}

// A 10-op patch as an agent would send it: property edits on ten clips.
json edit_patch(const std::string &project, const std::vector<std::string> &clips, size_t round) {
  json ops = json::array();
  for (size_t k = 0; k < 10; ++k) {
    const std::string &id = clips[(round * 7919 + k * 104729) % clips.size()];
    if (k % 2 == 0)
      ops.push_back({{"op", "replace"}, {"path", id + "/transform/opacity"}, {"value", double(round % 100) / 100.0}});
    else
      ops.push_back({{"op", "replace"}, {"path", id + "/name"}, {"value", "take " + std::to_string(round)}});
  }
  return {{"project", project}, {"patch", {{"ops", std::move(ops)}}}};
}

void bench_rational() {
  std::printf("Rational Time\n");
  const atm::Rational a = *atm::Rational::make(1001, 30000), b = *atm::Rational::make(1, 48000);
  constexpr int kN = 10'000'000;
  atm::Rational t;
  auto t0 = Clock::now();
  for (int i = 0; i < kN; ++i)
    t = *add(t, (i & 1) ? a : b); // mixed rates: the denominators differ
  const double mixed = ms_since(t0);
  report("10 M mixed-rate additions", mixed, "ms", 1000.0);
  t = {};
  t0 = Clock::now();
  for (int i = 0; i < kN; ++i)
    t = *add(t, a);
  report("accumulating 29.97 fps frame durations, per add", ms_since(t0) * 1.0e6 / kN, "ns", 20.0);
  int64_t order = 0;
  t0 = Clock::now();
  for (int i = 0; i < kN; ++i)
    order += compare(a, (i & 1) ? b : t);
  report("compare (128-bit cross-multiply)", ms_since(t0) * 1.0e6 / kN, "ns", 20.0);
  std::printf("  (checksum %s %lld)\n", t.to_string().c_str(), static_cast<long long>(order));
}

// One 1080p frame of an adjustment layer with each effect, and of a wipe, against their references (a blur, a dissolve).
// The pixel passes run on the packed NV12 picture the renderer works in, so this is the cost the export pays per frame.
void bench_effects() {
  std::printf("\nEffects, wipe and push (1920 x 1080 frame, p50 of 40)\n");
  const auto frame_ms = [](atm::render::Composition comp, int64_t frame) {
    atm::render::Renderer renderer(std::move(comp), 1920, 1080);
    std::vector<uint8_t> out(atm::media::nv12_size(1920, 1080));
    (void)renderer.render(frame, out.data()); // the first frame opens fonts and allocates the scratch pictures
    Series s;
    for (int i = 0; i < 40; ++i)
      s.time([&] { (void)renderer.render(frame, out.data()); });
    return s.pct(0.5) / 1000.0;
  };
  const auto with_effect = [](const char *kind, float a, float b, float c) {
    atm::render::Composition comp;
    comp.frames = 60;
    atm::render::Layer adj;
    adj.clip_id = "clp_adj";
    adj.frames = 60;
    adj.is_adjustment = true;
    atm::render::Effect e;
    e.kind = kind;
    e.v[0] = a, e.v[1] = b, e.v[2] = c;
    adj.effects.push_back(e);
    comp.layers.push_back(std::move(adj));
    return comp;
  };
  const auto keyed_vignette = [&] { // strength animated by 50 keys: the cost of evaluating keyframes on top of the effect
    atm::render::Composition comp = with_effect("vignette", 0.6f, 0.5f, 0.4f);
    json keys = json::object();
    for (int i = 0; i < 50; ++i)
      keys["kf_" + std::to_string(i)] = {{"t", std::to_string(i) + "/3"}, {"v", i % 2 ? 0.8 : 0.2}, {"interp", "easing"}, {"ease", "ease_in_out_quad"}};
    atm::render::Effect &e = comp.layers[0].effects[0];
    e.def = atm::eval::find_effect("vignette");
    e.curve[0] = *atm::eval::parse_curve(keys, 1);
    return comp;
  };
  const auto two_clips = [](atm::eval::TransitionKind kind) { // text clips: no files needed; they are cached after the first frame
    atm::render::Composition comp;
    comp.frames = 60;
    for (int i = 0; i < 2; ++i) {
      atm::render::Layer l;
      l.clip_id = i ? "clp_b" : "clp_a";
      l.is_text = true;
      l.text = i ? "B" : "A";
      l.text_size = 0.5f;
      l.frames = i ? 40 : 60;
      l.start_frame = i ? 20 : 0;
      comp.layers.push_back(std::move(l));
    }
    comp.layers[0].mix_with = 1;
    comp.layers[1].mixed_by = 0;
    comp.layers[0].mix_start = 20;
    comp.layers[0].mix_frames = 20;
    comp.layers[0].mix_kind = kind;
    return comp;
  };
  report("blur 0.02 (reference), adjustment layer", frame_ms(with_effect("gaussian_blur", 0.02f, 0, 0), 10), "ms", 40.0);
  report("color grade, adjustment layer", frame_ms(with_effect("color_grade", 0.1f, 0.2f, 1.2f), 10), "ms", 2.0);
  report("vignette, adjustment layer", frame_ms(with_effect("vignette", 0.6f, 0.5f, 0.4f), 10), "ms", 3.0);
  report("vignette, strength animated by 50 keys", frame_ms(keyed_vignette(), 10), "ms", 3.0);
  report("dissolve (reference), two text clips", frame_ms(two_clips(atm::eval::TransitionKind::dissolve), 30), "ms", 20.0);
  report("wipe, two text clips", frame_ms(two_clips(atm::eval::TransitionKind::wipe), 30), "ms", 4.0);
  report("push, two text clips", frame_ms(two_clips(atm::eval::TransitionKind::push), 30), "ms", 4.0);
}

void bench_profiler() {
  std::printf("\nProfiler\n");
  constexpr int kN = 10'000'000;
  auto t0 = Clock::now();
  for (int i = 0; i < kN; ++i) {
    ATM_PROFILE_SCOPE("bench.zone");
  }
  report("cost of one zone, switched on", ms_since(t0) * 1.0e6 / kN, "ns", 50.0);
  atm::prof::set_enabled(false);
  t0 = Clock::now();
  for (int i = 0; i < kN; ++i) {
    ATM_PROFILE_SCOPE("bench.zone");
  }
  report("cost of one zone, switched off at run time", ms_since(t0) * 1.0e6 / kN, "ns", 2.0);
  atm::prof::set_enabled(true);
}

// A test clip: a gradient with a moving bar, and a tone. Written once and reused by later runs.
std::string test_clip(const fs::path &dir, int width, int height, int seconds) {
  const fs::path path = dir / ("src_" + std::to_string(height) + "p_" + std::to_string(seconds) + "s.mp4");
  std::error_code ec;
  if (fs::exists(path, ec))
    return path.string();
  std::printf("  writing the test clip %s ...\n", path.filename().string().c_str());
  auto enc = atm::media::Encoder::create({path.string(), width, height, 30, 1, width * height * 6, true});
  if (!enc) {
    std::fprintf(stderr, "%s\n", enc.error().message.c_str());
    std::exit(1);
  }
  std::vector<uint8_t> px(size_t(width) * size_t(height) * 4), nv12(atm::media::nv12_size(width, height));
  std::vector<float> tone(1600 * 2, 0.1f);
  for (int f = 0; f < seconds * 30; ++f) {
    const int bar = f * width / (seconds * 30);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        uint8_t *q = px.data() + (size_t(y) * size_t(width) + size_t(x)) * 4;
        const bool on = x >= bar && x < bar + width / 40;
        q[0] = on ? 255 : uint8_t(y * 255 / height);
        q[1] = on ? 255 : uint8_t(x * 255 / width);
        q[2] = on ? 255 : uint8_t((x + y + f) & 255);
        q[3] = 255;
      }
    atm::media::bgrx_to_nv12(px.data(), width, height, nv12.data());
    (void)(*enc)->video(nv12.data(), f);
    (void)(*enc)->audio(tone.data(), 1600);
  }
  (void)(*enc)->finish();
  return path.string();
}

// Exports `seconds` of a two-track sequence (a full-length bottom track, a half-transparent clip above it for the
// middle third) and returns the speed as a multiple of real time.
double export_scene(Engine &engine, const fs::path &dir, const std::string &clip, int width, int height, int seconds) {
  const std::string project = (dir / ("Export" + std::to_string(height) + ".attome")).string();
  std::error_code ec;
  fs::remove_all(project, ec);
  const std::string seq =
      must(engine, "project.create", {{"path", project}, {"canvas", {{"width", width}, {"height", height}}}})["sequence"];
  const auto clip_at = [&](int in, int dur, double opacity) {
    return json{{"name", "c"},
                {"timing", {{"record_in", std::to_string(in) + "s"}, {"duration", std::to_string(dur) + "s"}, {"source_in", "0"}}},
                {"media_ref", {{"type", "file"}, {"path", clip}}},
                {"transform", {{"opacity", opacity}}}};
  };
  json ops = json::array({{{"op", "add"}, {"path", seq + "/tracks/$new:v1"}, {"value", {{"kind", "video"}, {"name", "V1"}}}},
                          {{"op", "add"}, {"path", seq + "/tracks/$new:v2"}, {"value", {{"kind", "video"}, {"name", "V2"}}}}});
  const int part = seconds / 3;
  for (int t = 0; t < seconds; t += part) // the bottom track is cut every `part` seconds, as edited footage is
    ops.push_back({{"op", "add"}, {"path", "$new:v1/clips/$new:a" + std::to_string(t)}, {"value", clip_at(t, part, 1.0)}});
  ops.push_back({{"op", "add"}, {"path", "$new:v2/clips/$new:b"}, {"value", clip_at(part, part, 0.5)}});
  must(engine, "project.patch", {{"project", project}, {"patch", {{"ops", ops}}}});

  const auto t0 = Clock::now();
  const json job = must(engine, "render.sequence", {{"project", project}, {"output", (dir / "out.mp4").string()}});
  json state;
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    state = must(engine, "jobs.get", {{"job_id", job["job_id"]}});
  } while (state["state"] == "running");
  const double wall = ms_since(t0) / 1000.0;
  if (state["state"] != "done") {
    std::fprintf(stderr, "export failed: %s\n", state.dump().c_str());
    std::exit(1);
  }
  std::printf("  encoder: %s\n", state.value("encoder", "?").c_str());
  if (state.contains("warning"))
    std::printf("  warning: %s\n", state["warning"].get<std::string>().c_str());
  must(engine, "project.close", {{"project", project}});
  return double(seconds) / wall;
}

int bench_export() {
  const fs::path dir = fs::temp_directory_path() / "attome-bench-export"; // kept: the test clips take a while to write
  fs::create_directories(dir);
  std::printf("Export (F1 §7.5 scenes, hardware H.264, wall time including encoder start)\n");
  const std::string hd = test_clip(dir, 1920, 1080, 20), uhd = test_clip(dir, 3840, 2160, 10);
  { // the encoder alone: one still frame, 600 times
    std::vector<uint8_t> px(atm::media::nv12_size(1920, 1080), 128);
    const auto t0 = Clock::now();
    auto enc = atm::media::Encoder::create({(dir / "enc.mp4").string(), 1920, 1080, 30, 1, 12'000'000, false});
    for (int f = 0; enc && f < 600; ++f)
      (void)(*enc)->video(px.data(), f);
    if (enc)
      (void)(*enc)->finish();
    std::printf("  %-52s %12.0f fps (%s)\n", "encoder only, 1080p, incl. start and finish", 600.0 / (ms_since(t0) / 1000.0),
                enc ? (*enc)->name().c_str() : "failed");
  }
  Engine engine({.fsync = false});
  atm::prof::reset();
  const double x1080 = export_scene(engine, dir, hd, 1920, 1080, 60);
  std::printf("  %-52s %12.2f x real time   target >= 4\n", "1080p30, 60 s, 2 tracks", x1080);
  const double x2160 = export_scene(engine, dir, uhd, 3840, 2160, 30);
  std::printf("  %-52s %12.2f x real time   target >= 1.5\n", "2160p30, 30 s, 2 tracks", x2160);
  std::printf("\nZone profile of the exports\n%s", atm::prof::format_report(atm::prof::snapshot()).c_str());
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  int clip_count = 10000, patch_count = 2000;
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], "--export")) {
      atm::prof::set_thread_name("atm-bench");
      return bench_export();
    }
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--clips"))
      clip_count = std::atoi(argv[i + 1]);
    if (!std::strcmp(argv[i], "--patches"))
      patch_count = std::atoi(argv[i + 1]);
  }
  atm::prof::set_thread_name("atm-bench");
  const fs::path dir = fs::temp_directory_path() / atm::new_id("attome-bench");
  fs::create_directories(dir);
  const std::string project = (dir / "Bench.attome").string();

  bench_rational();
  bench_profiler();
  bench_effects();
  atm::prof::reset();

  std::vector<std::string> clips;
  Series apply, dry, undo, redo, durable;
  {
    Engine engine({.fsync = false});
    const std::string seq = must(engine, "project.create", {{"path", project}, {"rate", "30000/1001"}})["sequence"];

    std::printf("\nDocument of %d clips on 20 tracks (journal without fsync)\n", clip_count);
    auto t0 = Clock::now();
    json track_ops = json::array();
    for (int t = 0; t < 20; ++t)
      track_ops.push_back({{"op", "add"}, {"path", seq + "/tracks/$new:t" + std::to_string(t)},
                           {"value", {{"kind", "video"}, {"name", "V" + std::to_string(t + 1)}}}});
    const json tracks = must(engine, "project.patch", {{"project", project}, {"patch", {{"ops", track_ops}}}})["id_map"];
    for (int first = 0; first < clip_count; first += 500) { // 500 clips per patch, spread over the tracks
      json ops = json::array();
      for (int i = first; i < std::min(first + 500, clip_count); ++i)
        ops.push_back({{"op", "add"},
                       {"path", tracks["$new:t" + std::to_string(i % 20)].get<std::string>() + "/clips/$new:c" +
                                    std::to_string(i)},
                       {"value", clip_value(i / 20)}});
      const json r = must(engine, "project.patch", {{"project", project}, {"patch", {{"ops", std::move(ops)}}}});
      for (const auto &id : r["id_map"])
        clips.push_back(id);
    }
    std::printf("  built in %.1f ms (%zu objects)\n", ms_since(t0),
                must(engine, "project.inspect", {{"project", project}})["data"]["objects"].get<size_t>());

    atm::prof::reset(); // measure the steady state, not the build
    for (int i = 0; i < patch_count; ++i) {
      json params = edit_patch(project, clips, size_t(i));
      apply.time([&] { must(engine, "project.patch", params); });
    }
    for (int i = 0; i < patch_count; ++i) {
      json params = edit_patch(project, clips, size_t(i) + 50000);
      params["dry_run"] = true;
      dry.time([&] { must(engine, "project.patch", params); });
    }
    const json steps = {{"project", project}};
    for (int i = 0; i < 500; ++i)
      undo.time([&] { must(engine, "project.undo", steps); });
    for (int i = 0; i < 500; ++i)
      redo.time([&] { must(engine, "project.redo", steps); });
    report("10-op patch, p50", apply.pct(0.50), "us", 1000.0);
    report("10-op patch, p99", apply.pct(0.99), "us", 5000.0);
    report("10-op dry run, p50", dry.pct(0.50), "us", 1000.0);
    report("undo one ChangeSet, p50", undo.pct(0.50), "us", 1000.0);
    report("redo one ChangeSet, p50", redo.pct(0.50), "us", 1000.0);

    // A timing edit runs the track-overlap rule over the whole track.
    Series retime;
    for (int i = 0; i < 500; ++i) {
      const json params = {{"project", project},
                           {"patch", {{"ops", json::array({{{"op", "replace"},
                                                            {"path", clips[size_t(i) * 13 % clips.size()] +
                                                                         "/timing/duration"},
                                                            {"value", i % 2 ? "1.5s" : "2s"}}})}}}};
      retime.time([&] { must(engine, "project.patch", params); });
    }
    report("retime one clip (overlap check on its track), p50", retime.pct(0.50), "us", 1000.0);

    t0 = Clock::now();
    must(engine, "project.save", {{"project", project}});
    report("save (serialize + hash + fsync + rename)", ms_since(t0), "ms", 1000.0);
  } // the engine goes away; edits after the save stay in the journal only

  {
    std::printf("\nDurability and recovery\n");
    Engine engine({.fsync = true});
    auto t0 = Clock::now();
    const json info = must(engine, "project.inspect", {{"project", project}});
    report("open + journal replay", ms_since(t0), "ms", 2000.0);
    std::printf("  (%zu objects, %zu records replayed)\n", info["data"]["objects"].get<size_t>(),
                info["data"]["recovered"].get<size_t>());
    for (int i = 0; i < 200; ++i) {
      json params = edit_patch(project, clips, size_t(i) + 90000);
      durable.time([&] { must(engine, "project.patch", params); });
    }
    report("10-op patch acknowledged after fsync, p50", durable.pct(0.50), "us", 20000.0);

    std::printf("\nDaemon round trip (named pipe / Unix socket, writer thread)\n");
#if defined(_WIN32)
    const std::string endpoint = "\\\\.\\pipe\\" + atm::new_id("attome-bench");
#else
    const std::string endpoint = (dir / "bench.sock").string();
#endif
    atm::api::Server server(engine);
    auto listener = atm::api::Listener::listen(endpoint);
    if (!listener) {
      std::fprintf(stderr, "listen failed: %s\n", listener.error().message.c_str());
      return 1;
    }
    std::thread accept([&] { server.serve(*listener); });
    {
      auto stream = atm::api::connect(endpoint);
      atm::api::FrameReader reader(*stream);
      std::string body;
      const std::string get = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "project.get"},
                                   {"params", {{"project", project}, {"id", clips[42 % clips.size()]}}}}
                                  .dump();
      Series trip;
      for (int i = 0; i < 5000; ++i)
        trip.time([&] {
          atm::api::write_frame(*stream, get);
          reader.read(body);
        });
      report("project.get round trip, p50", trip.pct(0.50), "us", 1000.0);
      report("project.get round trip, p99", trip.pct(0.99), "us", 5000.0);
    }
    listener->close();
    accept.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // let the connection thread finish its last frame
  }

  std::printf("\nZone profile of this run (steady state; the build of the document is excluded)\n%s",
              atm::prof::format_report(atm::prof::snapshot()).c_str());
  std::error_code ec;
  fs::remove_all(dir, ec);
  std::printf("\n%s\n", g_failed ? "Some numbers miss their target; the profile above shows where the time goes."
                                 : "Every number meets its target.");
  return 0;
}
