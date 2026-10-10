// Getting a video from a link, as an option the user turns on: ytdlp.status says whether the downloader (yt-dlp, an open-source program)
// is on this computer, ytdlp.install fetches it once, from its own GitHub release, checked against that release's SHA2-256SUMS, and
// video.fetch saves one video of a link to a folder of the user. Attome never ships the program and never runs it unasked.
#include "engine_impl.hpp"

#include <fstream>
#include <iterator>
#include <sstream>

#include "atm/net/http.hpp"
#include "atm/models/models.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace atm::api {

namespace {

constexpr const char *kReleaseBase = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/";

fs::path tools_dir_of(const fs::path &models_dir) { return models_dir.parent_path() / "tools"; }

// ATTOME_YTDLP, then the tools folder (where ytdlp.install puts it), then PATH.
fs::path find_ytdlp(const fs::path &tools) {
  std::vector<fs::path> candidates;
  if (const char *env = std::getenv("ATTOME_YTDLP"); env && *env)
    candidates.push_back(to_path(env));
#if defined(_WIN32)
  const char *exe = "yt-dlp.exe";
  constexpr char kSeparator = ';';
#else
  const char *exe = "yt-dlp";
  constexpr char kSeparator = ':';
#endif
  candidates.push_back(tools / exe);
  if (const char *path = std::getenv("PATH"); path && *path) {
    std::string all = path;
    size_t from = 0;
    while (from <= all.size()) {
      size_t to = all.find(kSeparator, from);
      if (to == std::string::npos)
        to = all.size();
      if (to > from)
        candidates.push_back(to_path(all.substr(from, to - from)) / exe);
      from = to + 1;
    }
  }
  std::error_code ec;
  for (const fs::path &c : candidates)
    if (fs::is_regular_file(c, ec))
      return c;
  return {};
}

#if defined(_WIN32)
std::wstring quote(const std::wstring &arg) {
  std::wstring out = L"\"";
  for (wchar_t ch : arg) {
    if (ch == L'"')
      out += L'\\';
    out += ch;
  }
  return out + L"\"";
}

// Runs a program with its output (both streams) handed to on_line, line by line. Stops it when `cancel` is set. Returns its exit code, or -1.
int run_process(const fs::path &exe, const std::vector<std::wstring> &args, const std::atomic<bool> &cancel,
                const std::function<void(const std::string &)> &on_line) {
  SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
  HANDLE out_read = nullptr, out_write = nullptr;
  if (!CreatePipe(&out_read, &out_write, &inherit, 0))
    return -1;
  SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = out_write;
  si.hStdError = out_write;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  std::wstring command = quote(exe.wstring());
  for (const std::wstring &a : args)
    command += L" " + quote(a);
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(out_write);
  if (!ok) {
    CloseHandle(out_read);
    return -1;
  }
  CloseHandle(pi.hThread);
  std::string line;
  const auto drain = [&] {
    for (;;) {
      DWORD available = 0;
      if (!PeekNamedPipe(out_read, nullptr, 0, nullptr, &available, nullptr) || available == 0)
        return;
      char buffer[4096];
      DWORD got = 0;
      if (!ReadFile(out_read, buffer, DWORD(std::min<size_t>(sizeof buffer, available)), &got, nullptr) || got == 0)
        return;
      for (DWORD i = 0; i < got; ++i) {
        if (buffer[i] == '\n' || buffer[i] == '\r') {
          if (!line.empty())
            on_line(line);
          line.clear();
        } else {
          line += buffer[i];
        }
      }
    }
  };
  int code = -1;
  for (;;) {
    drain();
    if (cancel.load()) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 2000);
      break;
    }
    if (WaitForSingleObject(pi.hProcess, 80) != WAIT_TIMEOUT) {
      drain();
      DWORD exit_code = 0;
      GetExitCodeProcess(pi.hProcess, &exit_code);
      code = int(exit_code);
      break;
    }
  }
  if (!line.empty())
    on_line(line);
  CloseHandle(pi.hProcess);
  CloseHandle(out_read);
  return code;
}
#endif

// Text from a program that may not be UTF-8: bad bytes become '?', and a cut never lands inside a character.
std::string clean_utf8(const std::string &text, size_t max_bytes = std::string::npos) {
  std::string out;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    bool ok = n > 0 && i + n <= text.size();
    for (size_t k = 1; ok && k < n; ++k)
      ok = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
    const size_t add = ok ? n : 1;
    if (out.size() + add > max_bytes)
      break;
    if (ok)
      out.append(text, i, n);
    else
      out += '?';
    i += add;
  }
  return out;
}

std::string read_text(const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    text.pop_back();
  return text;
}

void finish(const std::shared_ptr<Job> &job, Job::State state, std::string detail, const Error *error = nullptr) {
  std::lock_guard lock(job->mutex);
  job->seconds = std::chrono::duration<double>(Clock::now() - job->started).count();
  job->detail = std::move(detail);
  if (error)
    job->error = *error;
  job->state.store(state);
}

void run_install_body(const std::shared_ptr<Job> &job, const fs::path &tools, const std::shared_ptr<net::Transport> &transport);

void run_install(const std::shared_ptr<Job> &job, fs::path tools, std::shared_ptr<net::Transport> transport) {
  prof::set_thread_name("atm-ytdlp");
  try {
    run_install_body(job, tools, transport);
  } catch (const std::exception &e) {
    const Error err{ErrorCode::Internal, "Y_INTERNAL", std::string("The download stopped: ") + e.what(), {}, {}};
    finish(job, Job::failed, "", &err);
  }
}

void run_install_body(const std::shared_ptr<Job> &job, const fs::path &tools, const std::shared_ptr<net::Transport> &transport) {
  const std::string name = "yt-dlp.exe";
  {
    std::lock_guard lock(job->mutex);
    job->detail = "Reading the checksum of the latest release";
  }
  const auto sums = net::fetch(*transport, std::string(kReleaseBase) + "SHA2-256SUMS");
  if (!sums || sums->status != 200) {
    const Error e = sums ? Error{ErrorCode::IoError, "Y_SUMS", "GitHub did not give the checksum list (status " + std::to_string(sums->status) + ").", {}, {}} : sums.error();
    return finish(job, Job::failed, "", &e);
  }
  std::string hash;
  std::istringstream lines(sums->body);
  for (std::string row; std::getline(lines, row);) {
    while (!row.empty() && (row.back() == '\r' || row.back() == ' '))
      row.pop_back();
    if (row.size() > 66 && row.compare(row.size() - name.size(), name.size(), name) == 0 && row[row.size() - name.size() - 1] == ' ')
      hash = row.substr(0, 64);
  }
  if (hash.size() != 64) {
    const Error e{ErrorCode::IoError, "Y_SUMS", "The release's checksum list has no line for " + name + ".", {}, {}};
    return finish(job, Job::failed, "", &e);
  }
  for (char &c : hash)
    c = char(std::tolower(static_cast<unsigned char>(c)));
  int64_t size = 0;
  const auto head = transport->get({std::string(kReleaseBase) + name, 0, {}, {}},
                                   [&](const net::Response &r) {
                                     size = r.content_length;
                                     return false; // only the size is wanted here
                                   },
                                   [](const uint8_t *, size_t) { return false; });
  if (!head || size <= 0) {
    const Error e = head ? Error{ErrorCode::IoError, "Y_SIZE", "GitHub did not say how large the program is.", {}, {}} : head.error();
    return finish(job, Job::failed, "", &e);
  }
  job->units_total.store(size);
  models::CatalogFile file;
  file.url = std::string(kReleaseBase) + name;
  file.path = name;
  file.sha256 = hash;
  file.size = size;
  models::FetchProgress progress;
  progress.done = &job->units_done;
  progress.fetched = &job->fetched;
  progress.cancel = &job->cancel;
  progress.on_phase = [&](const std::string &text) {
    std::lock_guard lock(job->mutex);
    job->detail = text;
  };
  const auto result = models::fetch_file(*transport, file, tools, progress);
  if (result)
    return finish(job, Job::done, "Installed");
  if (result.error().code == ErrorCode::Cancelled)
    return finish(job, Job::cancelled, "Stopped; start it again to continue");
  const Error e = result.error();
  finish(job, Job::failed, "", &e);
}

void run_fetch_video(const std::shared_ptr<Job> &job, fs::path exe, std::string url, fs::path folder, std::string browser) {
  prof::set_thread_name("atm-video-fetch");
#if !defined(_WIN32)
  (void)exe, (void)url, (void)folder, (void)browser;
  const Error e{ErrorCode::Unsupported, "Y_UNSUPPORTED", "Getting a video from a link runs on Windows in this version.", {}, {}};
  finish(job, Job::failed, "", &e);
#else
  const fs::path path_file = folder / "path.txt", title_file = folder / "title.txt";
  std::error_code ec;
  fs::remove(path_file, ec);
  fs::remove(title_file, ec);
  // One file of at most 720 p (a style is read from that), joined by FFmpeg when it has to, no playlist.
  std::vector<std::wstring> args = {L"--no-playlist", L"--no-warnings", L"--newline", L"--progress", L"--no-simulate", L"-f",
                                          L"bv*[height<=720]+ba/b[height<=720]/b", L"--merge-output-format", L"mp4", L"-P", folder.wstring(),
                                          L"-o", L"%(id)s.%(ext)s", L"--print-to-file", L"after_move:filepath", path_file.wstring(),
                                          L"--print-to-file", L"after_move:%(title)s", title_file.wstring(), L"--", std::wstring(url.begin(), url.end())};
  if (!browser.empty()) { // the sign-in of a browser the user chose, so a site that asks for one can answer
    args.insert(args.begin(), L"--cookies-from-browser");
    args.insert(args.begin() + 1, std::wstring(browser.begin(), browser.end()));
  }
  std::string last_error;
  const int code = run_process(exe, args, job->cancel, [&](const std::string &line) {
    const size_t pct = line.find('%');
    if (line.rfind("[download]", 0) == 0 && pct != std::string::npos && pct > 0) {
      size_t start = pct;
      while (start > 0 && (std::isdigit(static_cast<unsigned char>(line[start - 1])) || line[start - 1] == '.'))
        --start;
      const double value = std::atof(line.substr(start, pct - start).c_str());
      job->units_done.store(int64_t(std::clamp(value, 0.0, 100.0) * 10.0));
    }
    if (line.rfind("ERROR", 0) == 0)
      last_error = clean_utf8(line, 400);
    std::lock_guard lock(job->mutex);
    job->detail = clean_utf8(line, 160);
  });
  if (job->cancel.load())
    return finish(job, Job::cancelled, "Stopped");
  const std::string path = fs::exists(path_file) ? read_text(path_file) : std::string();
  if (code != 0 || path.empty() || !fs::exists(to_path(path))) {
    const Error e{ErrorCode::IoError, "Y_FETCH", last_error.empty() ? "The video could not be fetched (the program ended with code " + std::to_string(code) + ")." : last_error,
                  {}, last_error.find("cookie database") != std::string::npos || last_error.find("DPAPI") != std::string::npos
                          ? "The browser keeps its sign-in file locked or encrypted. Close that browser completely (also in the tray) and try again, or choose Firefox, "
                            "which can be read while it is open. Chrome and Edge often cannot be read at all by newer versions."
                          : last_error.find("not a bot") != std::string::npos || last_error.find("Sign in") != std::string::npos
                          ? "The site asks for a sign-in. In the Niches panel pick the browser you are signed in with (\"Sign-in from\"), then try again."
                          : "Check the link, and that the video is public. yt-dlp changes often: a newer one may be needed (Download yt-dlp again)."};
    return finish(job, Job::failed, "", &e);
  }
  {
    std::lock_guard lock(job->mutex);
    job->result = {{"path", clean_utf8(path)}, {"title", fs::exists(title_file) ? clean_utf8(read_text(title_file)) : std::string()}, {"url", url}};
  }
  job->units_done.store(1000);
  finish(job, Job::done, "Saved");
#endif
}

} // namespace

  // ytdlp.status {}: whether the downloader is on this computer, and where ytdlp.install would put it.
Result<json> Engine::Impl::ytdlp_status(const json &) {
  const fs::path tools = tools_dir_of(models_dir());
  const fs::path found = find_ytdlp(tools);
  json out = {{"found", !found.empty()}, {"tools_dir", to_utf8(tools)}, {"program", "yt-dlp"}, {"licence", "Unlicense (public domain)"},
              {"source", "https://github.com/yt-dlp/yt-dlp"}, {"approx_mb", 18}};
  if (!found.empty())
    out["path"] = to_utf8(found);
  for (const auto &[job_id, job] : jobs)
    if (job->kind == "ytdlp.install" && job->state.load() == Job::running)
      out["job_id"] = job_id;
  return out;
}

  // ytdlp.install {}: fetches yt-dlp from its latest GitHub release into the tools folder, checked against the release's checksum list. A job.
Result<json> Engine::Impl::ytdlp_install(const json &) {
  for (const auto &[job_id, job] : jobs)
    if (job->kind == "ytdlp.install" && job->state.load() == Job::running)
      return json{{"job_id", job_id}, {"already_running", true}};
  const fs::path tools = tools_dir_of(models_dir());
  std::error_code ec;
  fs::create_directories(tools, ec);
  auto job = std::make_shared<Job>();
  job->id = new_id("job");
  job->kind = "ytdlp.install";
  job->output = to_utf8(tools);
  job->units_total.store(1);
  jobs[job->id] = job;
  job->thread = std::thread(run_install, job, tools, cfg.transport ? cfg.transport : std::shared_ptr<net::Transport>(net::system_transport()));
  return json{{"job_id", job->id}, {"tools_dir", to_utf8(tools)}};
}

  // video.fetch {url, folder?}: saves the video of a link (at most 720 p, one video, no playlist) to a folder, as a job; the result has path and title.
  // For analysing a style (video.analyze) and for a reference the user is allowed to keep; what a link may be used for is the user's to check.
Result<json> Engine::Impl::video_fetch(const json &params) {
  ATM_TRY(const std::string *url, string_param(params, "url"));
  if (url->rfind("https://", 0) != 0 && url->rfind("http://", 0) != 0)
    return bad_param("url", "must be a web link that starts with https://");
  if (url->size() > 2000 || url->find_first_of(" \t\r\n\"") != std::string::npos)
    return bad_param("url", "is not a plain link");
  const fs::path exe = find_ytdlp(tools_dir_of(models_dir()));
  if (exe.empty())
    return fail(ErrorCode::WorkerUnavailable, "Y_NOT_INSTALLED", "The video downloader (yt-dlp) is not on this computer.", {},
                "ytdlp.install fetches it (about 18 MB) once; it is optional.");
  const std::string browser = params.value("cookies_from_browser", std::string());
  if (!browser.empty() && browser != "chrome" && browser != "edge" && browser != "firefox" && browser != "brave" && browser != "opera" && browser != "vivaldi")
    return bad_param("cookies_from_browser", "must be chrome, edge, firefox, brave, opera or vivaldi");
  fs::path folder = params.contains("folder") ? to_path(params.value("folder", std::string())) : tools_dir_of(models_dir()).parent_path() / "downloads";
  std::error_code ec;
  fs::create_directories(folder, ec);
  if (ec)
    return fail(ErrorCode::IoError, "Y_FOLDER", "The folder " + to_utf8(folder) + " cannot be made.");
  auto job = std::make_shared<Job>();
  job->id = new_id("job");
  job->kind = "video.fetch";
  job->output = *url;
  job->units_total.store(1000);
  jobs[job->id] = job;
  job->thread = std::thread(run_fetch_video, job, exe, *url, folder, browser);
  return json{{"job_id", job->id}, {"url", *url}, {"folder", to_utf8(folder)}};
}

} // namespace atm::api
