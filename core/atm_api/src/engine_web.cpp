// Web motion graphics: web.status, web.preview, web.render. An agent writes a page (HTML, CSS, JavaScript); a headless Chromium draws it frame by
// frame and the frames become a clip.
//
// The browser is the one the computer has (Edge is on every Windows; Chrome, or ATTOME_BROWSER): nothing is downloaded. Attome talks to it over the
// DevTools protocol through two pipes (--remote-debugging-pipe), never a network port. The page does not run in real time: before each frame the
// engine sets the page's clock (requestAnimationFrame, performance.now, Date.now) and every CSS and Web Animations animation to that frame's time,
// so a frame is the same however slow the page is, and a render is repeatable. The page has no network unless asked for.
#include "engine_impl.hpp"

#include <cstdio>
#include <fstream>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace atm::api {

namespace {

const char *kUnsupported = "Web graphics are drawn from Attome on Windows in this version.";

// Run in the page before its own scripts: a clock the engine moves, and window.__attomeSeek(ms) to move it.
const char *kClock = R"JS(
(() => {
  let now = 0;
  const start = Date.now();
  let queue = new Map();
  let next = 1;
  performance.now = () => now;
  Date.now = () => start + now;
  window.requestAnimationFrame = (cb) => { const id = next++; queue.set(id, cb); return id; };
  window.cancelAnimationFrame = (id) => { queue.delete(id); };
  window.__attomeSeek = async (ms) => {
    now = ms;
    for (const a of document.getAnimations()) {
      try { a.pause(); a.currentTime = ms; } catch (e) {}
    }
    for (const v of document.querySelectorAll('video')) { try { v.pause(); v.currentTime = ms / 1000; } catch (e) {} }
    // run what asked for a frame; what they ask for now waits for the next frame
    const run = queue; queue = new Map();
    for (const cb of run.values()) { try { cb(now); } catch (e) { console.error(e); } }
    if (typeof window.attomeFrame === 'function') { try { await window.attomeFrame(now / 1000); } catch (e) { console.error(e); } }
    return true;
  };
})();
)JS";

#if defined(_WIN32)

std::wstring quote(const std::wstring &s) { return L"\"" + s + L"\""; }

// The browser: ATTOME_BROWSER, then Edge, then Chrome. {} when none.
fs::path find_browser() {
  std::error_code ec;
  if (const char *env = std::getenv("ATTOME_BROWSER"); env && *env) {
    if (std::string(env) == "none")
      return {};
    if (fs::is_regular_file(to_path(env), ec))
      return to_path(env);
  }
  std::vector<fs::path> candidates;
  for (const char *base : {"ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"})
    if (const char *b = std::getenv(base); b && *b) {
      candidates.push_back(to_path(b) / "Microsoft" / "Edge" / "Application" / "msedge.exe");
      candidates.push_back(to_path(b) / "Google" / "Chrome" / "Application" / "chrome.exe");
    }
  for (const fs::path &c : candidates)
    if (fs::is_regular_file(c, ec))
      return c;
  return {};
}

// A headless browser with the DevTools protocol on two pipes: the child reads commands on its file 3 and writes answers on its file 4 (the C runtime
// hands them over through STARTUPINFO's reserved block, as Node's child_process does).
class Browser {
public:
  ~Browser() { close(); }

  Result<void> start(const fs::path &exe, const fs::path &profile, bool network) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE in_r = nullptr, out_w = nullptr;
    if (!CreatePipe(&in_r, &to_child_, &sa, 1 << 20) || !CreatePipe(&from_child_, &out_w, &sa, 1 << 20))
      return fail(ErrorCode::IoError, "W_START", "The pipes to the browser could not be made.");
    SetHandleInformation(to_child_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(from_child_, HANDLE_FLAG_INHERIT, 0);
    const HANDLE null_out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    // the C runtime's table of inherited files: count, a flag byte each, a handle each
    constexpr int n = 5;
    std::vector<unsigned char> table(sizeof(int) + n + n * sizeof(HANDLE), 0);
    const int count = n;
    std::memcpy(table.data(), &count, sizeof(int));
    unsigned char *flags = table.data() + sizeof(int);
    HANDLE handles[n] = {INVALID_HANDLE_VALUE, null_out, null_out, in_r, out_w};
    constexpr unsigned char kOpen = 0x01, kPipe = 0x08, kDev = 0x40;
    flags[0] = 0;
    flags[1] = flags[2] = kOpen | kDev;
    flags[3] = flags[4] = kOpen | kPipe;
    std::memcpy(flags + n, handles, sizeof handles);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = null_out;
    si.hStdError = null_out;
    si.cbReserved2 = WORD(table.size());
    si.lpReserved2 = table.data();
    std::wstring cmd = quote(exe.wstring()) + L" --headless=new --remote-debugging-pipe --no-first-run --no-default-browser-check --disable-extensions"
                                              L" --disable-background-networking --disable-sync --disable-component-update --mute-audio --hide-scrollbars"
                                              L" --allow-file-access-from-files --force-device-scale-factor=1 --disable-gpu-vsync --run-all-compositor-stages-before-draw"
                                              L" --user-data-dir=" + quote(profile.wstring());
    if (!network)
      cmd += L" --host-resolver-rules=\"MAP * ~NOTFOUND\"";
    cmd += L" about:blank";
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (null_out != INVALID_HANDLE_VALUE)
      CloseHandle(null_out);
    if (!ok)
      return fail(ErrorCode::IoError, "W_START", "The browser could not be started (Windows error " + std::to_string(GetLastError()) + ").");
    process_ = pi.hProcess;
    CloseHandle(pi.hThread);
    return {};
  }

  // One command; the answer's result. Events met on the way are kept for wait_event.
  Result<json> call(const std::string &method, json params = json::object(), const std::string &session = {}, double timeout = 30.0) {
    const int id = ++last_id_;
    json msg = {{"id", id}, {"method", method}, {"params", std::move(params)}};
    if (!session.empty())
      msg["sessionId"] = session;
    std::string text = msg.dump();
    text.push_back('\0');
    DWORD wrote = 0;
    if (!WriteFile(to_child_, text.data(), DWORD(text.size()), &wrote, nullptr) || wrote != text.size())
      return fail(ErrorCode::IoError, "W_PIPE", "The browser stopped answering (" + method + ").");
    const auto began = Clock::now();
    for (;;) {
      ATM_TRY(std::optional<json> got, read_one(began, timeout));
      if (!got)
        return fail(ErrorCode::WorkerTimeout, "W_TIMEOUT", "The browser did not answer " + method + " in time.", {}, "Make the page lighter, or give a longer timeout.");
      if (got->value("id", 0) == id) {
        if (got->contains("error"))
          return fail(ErrorCode::InvalidArgument, "W_CDP", method + ": " + (*got)["error"].value("message", std::string("error")));
        return got->value("result", json::object());
      }
      if (got->contains("method"))
        events_.push_back(std::move(*got));
    }
  }

  // Waits for an event (Page.loadEventFired ...); true when it came.
  bool wait_event(const std::string &method, double timeout) {
    const auto began = Clock::now();
    for (;;) {
      for (auto it = events_.begin(); it != events_.end(); ++it)
        if (it->value("method", std::string()) == method) {
          events_.erase(it);
          return true;
        }
      auto got = read_one(began, timeout);
      if (!got || !*got)
        return false;
      if ((*got)->contains("method"))
        events_.push_back(std::move(**got));
    }
  }

  std::vector<json> take_events() { return std::exchange(events_, {}); }

  void close() {
    if (process_) {
      if (to_child_) { // ask nicely, then end it
        json bye = {{"id", ++last_id_}, {"method", "Browser.close"}};
        std::string text = bye.dump();
        text.push_back('\0');
        DWORD wrote = 0;
        WriteFile(to_child_, text.data(), DWORD(text.size()), &wrote, nullptr);
      }
      if (WaitForSingleObject(process_, 3000) != WAIT_OBJECT_0)
        TerminateProcess(process_, 0);
      CloseHandle(process_);
      process_ = nullptr;
    }
    for (HANDLE *h : {&to_child_, &from_child_})
      if (*h) {
        CloseHandle(*h);
        *h = nullptr;
      }
  }

private:
  Result<std::optional<json>> read_one(Clock::time_point began, double timeout) {
    for (;;) {
      if (const size_t z = buffer_.find('\0'); z != std::string::npos) {
        json j = json::parse(buffer_.substr(0, z), nullptr, false);
        buffer_.erase(0, z + 1);
        if (!j.is_discarded())
          return std::optional<json>(std::move(j));
        continue;
      }
      DWORD available = 0;
      if (!PeekNamedPipe(from_child_, nullptr, 0, nullptr, &available, nullptr))
        return fail(ErrorCode::IoError, "W_PIPE", "The browser closed its pipe (it may have crashed).");
      if (available == 0) {
        if (std::chrono::duration<double>(Clock::now() - began).count() > timeout)
          return std::optional<json>();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      std::string chunk(std::min<DWORD>(available, 1 << 20), '\0');
      DWORD got = 0;
      if (!ReadFile(from_child_, chunk.data(), DWORD(chunk.size()), &got, nullptr) || got == 0)
        return fail(ErrorCode::IoError, "W_PIPE", "The browser closed its pipe.");
      buffer_.append(chunk.data(), got);
    }
  }

  HANDLE process_ = nullptr, to_child_ = nullptr, from_child_ = nullptr;
  int last_id_ = 0;
  std::string buffer_;
  std::vector<json> events_;
};

std::string base64_decode(const std::string &in) {
  static int table[256];
  static bool init = false;
  if (!init) {
    for (int &v : table)
      v = -1;
    const char *abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i)
      table[static_cast<unsigned char>(abc[i])] = i;
    init = true;
  }
  std::string out;
  out.reserve(in.size() * 3 / 4);
  int val = 0, bits = -8;
  for (unsigned char c : in) {
    if (table[c] < 0)
      continue;
    val = (val << 6) + table[c];
    bits += 6;
    if (bits >= 0) {
      out.push_back(char((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  return out;
}

// What a page is: html (written into a work folder as index.html) or a file / a folder with index.html. The URL to open.
Result<std::string> page_url(const json &params, const fs::path &work) {
  std::error_code ec;
  fs::path file;
  if (params.contains("html") && params["html"].is_string() && !params["html"].get<std::string>().empty()) {
    fs::create_directories(work, ec);
    file = work / "index.html";
    if (!storage::atomic_write(file, params["html"].get<std::string>()))
      return fail(ErrorCode::IoError, "W_WORK", "The page could not be written to " + to_utf8(work) + ".");
  } else if (params.contains("path") && params["path"].is_string()) {
    file = to_path(params["path"].get<std::string>());
    if (fs::is_directory(file, ec))
      file /= "index.html";
    if (!fs::is_regular_file(file, ec))
      return fail(ErrorCode::NotFound, "W_PAGE", "There is no page at " + to_utf8(file) + ".", {}, "Give html, or the path of an .html file (or a folder with index.html).");
  } else {
    return fail(ErrorCode::InvalidArgument, "E_PARAM", "The parameter \"html\" or \"path\" is required: the page to draw.");
  }
  std::string url = "file:///" + to_utf8(fs::absolute(file, ec));
  std::replace(url.begin(), url.end(), '\\', '/');
  std::string escaped;
  for (char c : url)
    escaped += c == ' ' ? std::string("%20") : std::string(1, c);
  return escaped;
}

struct Page {
  Browser browser;
  std::string session;
  std::vector<std::string> console; // what the page wrote with console.error and its exceptions
};

// Opens the page at width x height with the engine's clock, waits until it and its fonts are loaded.
Result<void> open_page(Page &pg, const fs::path &exe, const fs::path &profile, const std::string &url, int width, int height, bool transparent, bool network,
                       double timeout) {
  ATM_CHECK(pg.browser.start(exe, profile, network));
  ATM_TRY(json target, pg.browser.call("Target.createTarget", {{"url", "about:blank"}}, {}, timeout));
  ATM_TRY(json attached, pg.browser.call("Target.attachToTarget", {{"targetId", target.value("targetId", std::string())}, {"flatten", true}}, {}, timeout));
  pg.session = attached.value("sessionId", std::string());
  const std::string &s = pg.session;
  ATM_CHECK(pg.browser.call("Page.enable", json::object(), s, timeout));
  ATM_CHECK(pg.browser.call("Runtime.enable", json::object(), s, timeout));
  ATM_CHECK(pg.browser.call("Emulation.setDeviceMetricsOverride", {{"width", width}, {"height", height}, {"deviceScaleFactor", 1}, {"mobile", false}}, s, timeout));
  if (transparent)
    ATM_CHECK(pg.browser.call("Emulation.setDefaultBackgroundColorOverride", {{"color", {{"r", 0}, {"g", 0}, {"b", 0}, {"a", 0}}}}, s, timeout));
  ATM_CHECK(pg.browser.call("Page.addScriptToEvaluateOnNewDocument", {{"source", kClock}}, s, timeout));
  ATM_CHECK(pg.browser.call("Page.navigate", {{"url", url}}, s, timeout));
  if (!pg.browser.wait_event("Page.loadEventFired", timeout))
    return fail(ErrorCode::WorkerTimeout, "W_LOAD", "The page did not finish loading in time.", {}, "Check that it does not wait for something from the network.");
  ATM_CHECK(pg.browser.call("Runtime.evaluate", {{"expression", "document.fonts ? document.fonts.ready.then(() => true) : true"}, {"awaitPromise", true}}, s, timeout));
  return {};
}

void collect_console(Page &pg) {
  for (const json &e : pg.browser.take_events()) {
    const std::string m = e.value("method", std::string());
    if (m == "Runtime.exceptionThrown") {
      const json d = e.value("params", json::object()).value("exceptionDetails", json::object());
      std::string text = d.value("text", std::string());
      if (d.contains("exception"))
        text += " " + d["exception"].value("description", std::string());
      pg.console.push_back(text.substr(0, 400));
    } else if (m == "Runtime.consoleAPICalled") {
      const json p = e.value("params", json::object());
      if (p.value("type", std::string()) == "error" && p.contains("args") && !p["args"].empty())
        pg.console.push_back(p["args"][0].value("value", p["args"][0].value("description", std::string())).substr(0, 400));
    }
  }
  if (pg.console.size() > 20)
    pg.console.resize(20);
}

// Moves the page's clock to `seconds` and returns the frame as PNG bytes.
Result<std::string> frame_at(Page &pg, double seconds, double timeout) {
  char expr[96];
  std::snprintf(expr, sizeof expr, "window.__attomeSeek(%.6f)", seconds * 1000.0);
  ATM_TRY(json r, pg.browser.call("Runtime.evaluate", {{"expression", expr}, {"awaitPromise", true}}, pg.session, timeout));
  if (r.contains("exceptionDetails"))
    pg.console.push_back(r["exceptionDetails"].value("text", std::string("an error in the page")));
  ATM_TRY(json shot, pg.browser.call("Page.captureScreenshot", {{"format", "png"}, {"fromSurface", true}, {"captureBeyondViewport", false}}, pg.session, timeout));
  collect_console(pg);
  return base64_decode(shot.value("data", std::string()));
}

void finish_job(const std::shared_ptr<Job> &job, Job::State state, std::string detail, const Error *error = nullptr) {
  std::lock_guard lock(job->mutex);
  job->seconds = std::chrono::duration<double>(Clock::now() - job->started).count();
  job->detail = std::move(detail);
  if (error)
    job->error = *error;
  job->state.store(state);
}

#endif

} // namespace

Result<json> Engine::Impl::web_status(const json &) {
#if defined(_WIN32)
  const fs::path exe = find_browser();
  json out = {{"supported", true}, {"found", !exe.empty()}};
  if (!exe.empty())
    out["browser"] = to_utf8(exe);
  else
    out["hint"] = "Install Microsoft Edge or Google Chrome, or set ATTOME_BROWSER to a Chromium browser.";
  return out;
#else
  return json{{"supported", false}, {"found", false}, {"hint", kUnsupported}};
#endif
}

#if defined(_WIN32)
namespace {
struct WebSpec {
  int width = 1920, height = 1080;
  bool transparent = false, network = false;
  double timeout = 60.0;
};
} // namespace
#endif

  // web.preview {html | path, at?, output, width?, height?, transparent?, network?}: one frame of the page at `at` seconds, as a PNG, to look at.
Result<json> Engine::Impl::web_preview(const json &params) {
#if !defined(_WIN32)
  (void)params;
  return fail(ErrorCode::Unsupported, "W_UNSUPPORTED", kUnsupported);
#else
  const fs::path exe = find_browser();
  if (exe.empty())
    return fail(ErrorCode::NotFound, "W_NO_BROWSER", "There is no Chromium browser on this computer.", {}, "Install Microsoft Edge or Google Chrome, or set ATTOME_BROWSER.");
  ATM_TRY(const std::string *output, string_param(params, "output"));
  const int width = params.value("width", 1920), height = params.value("height", 1080);
  if (width < 16 || height < 16 || width > 4096 || height > 4096)
    return bad_param("width", "and height must be 16 to 4096");
  const fs::path work = models_dir().parent_path() / "tools" / "web" / ("run_" + new_id("web"));
  ATM_TRY(std::string url, page_url(params, work));
  Page pg;
  std::error_code ec;
  const auto cleanup = [&] {
    pg.browser.close();
    fs::remove_all(work, ec);
  };
  if (auto opened = open_page(pg, exe, work / "profile", url, width, height, params.value("transparent", false), params.value("network", false), params.value("timeout", 60.0));
      !opened) {
    cleanup();
    return tl::unexpected(opened.error());
  }
  auto png = frame_at(pg, params.value("at", 0.0), params.value("timeout", 60.0));
  const std::vector<std::string> console = pg.console;
  cleanup();
  if (!png)
    return tl::unexpected(png.error());
  fs::create_directories(to_path(*output).parent_path(), ec);
  if (!storage::atomic_write(to_path(*output), *png))
    return fail(ErrorCode::IoError, "W_WRITE", "The picture could not be written to " + *output + ".");
  json out = {{"path", *output}, {"width", width}, {"height", height}, {"at", params.value("at", 0.0)}};
  if (!console.empty())
    out["page_errors"] = console;
  return out;
#endif
}

  // web.render {html | path, output, seconds | frames, fps?, width?, height?, transparent?, network?, project?, at?, track?}: a job that draws every
  // frame. output: a folder (a PNG sequence that keeps transparency, frame_000000.png ...) or an .mp4 (opaque). With project, the result is put on
  // the timeline at `at` (a sequence on a track over the picture), as add_clip would.
Result<json> Engine::Impl::web_render(const json &params) {
#if !defined(_WIN32)
  (void)params;
  return fail(ErrorCode::Unsupported, "W_UNSUPPORTED", kUnsupported);
#else
  const fs::path exe = find_browser();
  if (exe.empty())
    return fail(ErrorCode::NotFound, "W_NO_BROWSER", "There is no Chromium browser on this computer.", {}, "Install Microsoft Edge or Google Chrome, or set ATTOME_BROWSER.");
  ATM_TRY(const std::string *output, string_param(params, "output"));
  if (!to_path(*output).is_absolute())
    return bad_param("output", "must be an absolute path: a folder for a PNG sequence, or an .mp4 file");
  std::string ext = to_utf8(to_path(*output).extension());
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  const bool mp4 = ext == ".mp4";
  if (!ext.empty() && !mp4)
    return bad_param("output", "is a folder (PNG sequence) or an .mp4 file");
  const int width = params.value("width", 1920), height = params.value("height", 1080);
  if (width < 16 || height < 16 || width > 4096 || height > 4096 || (mp4 && !media::h264_size_ok(width, height)))
    return bad_param("width", "and height must be 16 to 4096 (an .mp4 at most 4096 x 2304 pixels)");
  const double fps = params.value("fps", 24.0);
  if (fps < 1.0 || fps > 120.0)
    return bad_param("fps", "must be 1 to 120");
  int64_t frames = params.value("frames", int64_t(0));
  if (frames <= 0)
    frames = int64_t(std::llround(params.value("seconds", 0.0) * fps));
  if (frames <= 0 || frames > 20000)
    return bad_param("seconds", "(or frames) is required: how long the animation is, at most 20000 frames");
  const bool transparent = params.value("transparent", !mp4);
  if (mp4 && params.value("transparent", false))
    return bad_param("transparent", "needs a folder output: an .mp4 has no transparency");
  const fs::path work = models_dir().parent_path() / "tools" / "web" / ("run_" + new_id("web"));
  ATM_TRY(std::string url, page_url(params, work));
  std::string project_path;
  if (params.contains("project")) {
    ATM_TRY(Project *pr, project(params));
    project_path = to_utf8(pr->dir);
  }
  auto job = std::make_shared<Job>();
  job->id = new_id("job");
  job->kind = "web.render";
  job->output = *output;
  job->units_total.store(frames);
  jobs[job->id] = job;
  const std::string out = *output;
  const bool network = params.value("network", false);
  const double timeout = params.value("timeout", 60.0);
  const std::string rate_text = std::abs(fps - std::round(fps)) < 1e-9 ? std::to_string(int(std::lround(fps))) : std::to_string(int(std::lround(fps * 1000))) + "/1000";
  job->thread = std::thread([job, exe, work, url, out, mp4, width, height, fps, frames, transparent, network, timeout, rate_text] {
    prof::set_thread_name("atm-web");
    std::error_code ec;
    Page pg;
    const auto cleanup = [&] {
      pg.browser.close();
      fs::remove_all(work, ec);
    };
    try {
      {
        std::lock_guard lock(job->mutex);
        job->detail = "Opening the page";
      }
      if (auto opened = open_page(pg, exe, work / "profile", url, width, height, transparent, network, timeout); !opened) {
        cleanup();
        const Error e = opened.error();
        return finish_job(job, Job::failed, "", &e);
      }
      fs::path folder = mp4 ? work / "frames" : to_path(out);
      fs::create_directories(folder, ec);
      if (!mp4) { // a sequence starts clean, with its rate beside it for media.probe
        for (const auto &e : fs::directory_iterator(folder, ec))
          if (e.path().filename().string().rfind("frame_", 0) == 0)
            fs::remove(e.path(), ec);
        (void)storage::atomic_write(folder / "sequence.json", json{{"rate", rate_text}, {"frames", frames}, {"width", width}, {"height", height}}.dump());
      }
      std::unique_ptr<media::Encoder> encoder;
      if (mp4) {
        media::EncodeSettings es;
        es.path = out;
        es.width = width;
        es.height = height;
        es.rate_num = int64_t(std::llround(fps * 1000));
        es.rate_den = 1000;
        es.audio = false;
        es.bitrate = 16'000'000;
        auto enc = media::Encoder::create(es);
        if (!enc) {
          cleanup();
          const Error e = enc.error();
          return finish_job(job, Job::failed, "", &e);
        }
        encoder = std::move(*enc);
      }
      for (int64_t f = 0; f < frames; ++f) {
        if (job->cancel.load()) {
          cleanup();
          return finish_job(job, Job::cancelled, "Stopped");
        }
        auto png = frame_at(pg, double(f) / fps, timeout);
        if (!png) {
          cleanup();
          const Error e = png.error();
          return finish_job(job, Job::failed, "", &e);
        }
        char name[32];
        std::snprintf(name, sizeof name, "frame_%06lld.png", static_cast<long long>(f));
        const fs::path file = folder / name;
        if (!storage::atomic_write(file, *png)) {
          cleanup();
          const Error e{ErrorCode::IoError, "W_WRITE", "A frame could not be written to " + to_utf8(folder) + ".", {}, {}};
          return finish_job(job, Job::failed, "", &e);
        }
        if (encoder) {
          auto still = media::read_still(to_utf8(file), width, height);
          if (!still || !encoder->video(still->nv12.data(), f)) {
            cleanup();
            const Error e{ErrorCode::IoError, "W_ENCODE", "A frame could not be encoded.", {}, {}};
            return finish_job(job, Job::failed, "", &e);
          }
          fs::remove(file, ec);
        }
        job->units_done.store(f + 1);
      }
      if (encoder)
        if (auto done = encoder->finish(); !done) {
          cleanup();
          const Error e = done.error();
          return finish_job(job, Job::failed, "", &e);
        }
      const std::vector<std::string> console = pg.console;
      cleanup();
      {
        std::lock_guard lock(job->mutex);
        job->result = {{"path", out}, {"frames", frames}, {"fps", fps}, {"width", width}, {"height", height}, {"transparent", transparent}};
        if (!console.empty())
          job->result["page_errors"] = console;
      }
      finish_job(job, Job::done, "Drawn");
    } catch (const std::exception &e) {
      cleanup();
      const Error err{ErrorCode::Internal, "W_INTERNAL", std::string("The render stopped: ") + e.what(), {}, {}};
      finish_job(job, Job::failed, "", &err);
    }
  });
  json res = {{"job_id", job->id}, {"output", out}, {"frames", frames}};
  if (!project_path.empty())
    res["note"] = "When the job is done, put it on the timeline with timeline.edit add_clip {path: output, at, track: \"new\"}.";
  return res;
#endif
}

} // namespace atm::api
