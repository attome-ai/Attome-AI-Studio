// Formats the operating system's encoders do not make (ProRes, DNxHR) are written by the user's own FFmpeg program, run as a separate process that
// the frames are piped into. Attome links no FFmpeg code and ships none (ADR-025: no GPL in the core; the only encoders used here are FFmpeg's own
// prores_ks and dnxhd, which are in its LGPL part), so what the user installs, and under which license, is the user's own matter.

#include "atm/media/media.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "atm/base/profiler.hpp"

namespace atm::media {
namespace {

namespace fs = std::filesystem;

fs::path to_path(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
std::string to_utf8(const fs::path &p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

// A program run as a child process: its input can be fed through a pipe, its output read, and its error text sent to a log file. No shell is
// involved, so a path needs no quoting rules of its own. Arguments are UTF-8; args[0] is the program.
class Process {
public:
  static std::unique_ptr<Process> start(const std::vector<std::string> &args, bool feed, bool capture_output, const fs::path &log);
  ~Process();
  bool write(const void *data, size_t bytes);
  void close_input();
  std::string read_all(); // everything the program prints (stdout and stderr) when started with capture_output
  int wait();             // the exit code
  Process(const Process &) = delete;
  Process &operator=(const Process &) = delete;

private:
  Process() = default;
#if defined(_WIN32)
  void *process = nullptr, *in_write = nullptr, *out_read = nullptr;
#else
  int pid = -1, in_write = -1, out_read = -1;
#endif
  bool waited = false;
  int code = -1;
};

#if defined(_WIN32)

std::wstring widen(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())).wstring(); }

// One argument as the Windows command line wants it: quoted when it has a space, with backslashes before a quote doubled.
std::wstring command_arg(const std::string &utf8) {
  const std::wstring a = widen(utf8);
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
    return a;
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (const wchar_t c : a) {
    if (c == L'\\') {
      ++slashes;
    } else if (c == L'"') {
      out.append(slashes * 2 + 1, L'\\');
      out += c;
      slashes = 0;
    } else {
      out.append(slashes, L'\\');
      out += c;
      slashes = 0;
    }
  }
  out.append(slashes * 2, L'\\');
  return out + L"\"";
}

std::unique_ptr<Process> Process::start(const std::vector<std::string> &args, bool feed, bool capture_output, const fs::path &log) {
  SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
  HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr, nul = nullptr, logfile = nullptr;
  const auto close_all = [&] {
    for (HANDLE h : {in_read, in_write, out_read, out_write, nul, logfile})
      if (h && h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
  };
  if (feed) {
    if (!CreatePipe(&in_read, &in_write, &inherit, 0))
      return nullptr;
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0); // the child gets the reading end only
  }
  if (capture_output) {
    if (!CreatePipe(&out_read, &out_write, &inherit, 0)) {
      close_all();
      return nullptr;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
  } else if (!log.empty()) {
    logfile = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
  const HANDLE child_in = in_read ? in_read : nul;
  const HANDLE child_out = out_write ? out_write : (logfile && logfile != INVALID_HANDLE_VALUE) ? logfile : nul;
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = child_in;
  si.hStdOutput = child_out;
  si.hStdError = child_out;
  std::wstring command;
  for (const std::string &a : args)
    command += (command.empty() ? L"" : L" ") + command_arg(a);
  PROCESS_INFORMATION pi{};
  const BOOL started = CreateProcessW(widen(args[0]).c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (!started) {
    close_all();
    return nullptr;
  }
  CloseHandle(pi.hThread);
  // The parent keeps its own ends only.
  for (HANDLE h : {in_read, out_write, nul, logfile})
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  auto self = std::unique_ptr<Process>(new Process());
  self->process = pi.hProcess;
  self->in_write = in_write;
  self->out_read = out_read;
  return self;
}

bool Process::write(const void *data, size_t bytes) {
  const char *p = static_cast<const char *>(data);
  while (bytes > 0) {
    DWORD done = 0;
    if (!WriteFile(in_write, p, DWORD(std::min<size_t>(bytes, 1 << 20)), &done, nullptr) || done == 0)
      return false;
    p += done;
    bytes -= done;
  }
  return true;
}

void Process::close_input() {
  if (in_write)
    CloseHandle(in_write);
  in_write = nullptr;
}

std::string Process::read_all() {
  std::string text;
  char buffer[4096];
  DWORD n = 0;
  while (out_read && ReadFile(out_read, buffer, sizeof buffer, &n, nullptr) && n > 0)
    text.append(buffer, n);
  return text;
}

int Process::wait() {
  if (!waited && process) {
    WaitForSingleObject(process, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process, &exit_code);
    code = int(exit_code);
    waited = true;
  }
  return code;
}

Process::~Process() {
  close_input();
  if (out_read)
    CloseHandle(out_read);
  if (process) {
    wait();
    CloseHandle(process);
  }
}

#else

std::unique_ptr<Process> Process::start(const std::vector<std::string> &args, bool feed, bool capture_output, const fs::path &log) {
  signal(SIGPIPE, SIG_IGN); // a program that stops reading is an error from write(), not the end of this one
  int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1};
  if ((feed && pipe(in_pipe) != 0) || (capture_output && pipe(out_pipe) != 0))
    return nullptr;
  const pid_t pid = fork();
  if (pid < 0)
    return nullptr;
  if (pid == 0) {
    const int null = open("/dev/null", O_RDWR);
    dup2(feed ? in_pipe[0] : null, 0);
    int out = capture_output ? out_pipe[1] : null;
    if (!capture_output && !log.empty())
      if (const int f = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); f >= 0)
        out = f;
    dup2(out, 1);
    dup2(out, 2);
    for (int fd = 3; fd < 256; ++fd)
      close(fd);
    std::vector<char *> argv;
    for (const std::string &a : args)
      argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    execv(argv[0], argv.data());
    _exit(127);
  }
  if (feed)
    close(in_pipe[0]);
  if (capture_output)
    close(out_pipe[1]);
  auto self = std::unique_ptr<Process>(new Process());
  self->pid = pid;
  self->in_write = feed ? in_pipe[1] : -1;
  self->out_read = capture_output ? out_pipe[0] : -1;
  return self;
}

bool Process::write(const void *data, size_t bytes) {
  const char *p = static_cast<const char *>(data);
  while (bytes > 0) {
    const ssize_t done = ::write(in_write, p, bytes);
    if (done <= 0)
      return false;
    p += done;
    bytes -= size_t(done);
  }
  return true;
}

void Process::close_input() {
  if (in_write >= 0)
    close(in_write);
  in_write = -1;
}

std::string Process::read_all() {
  std::string text;
  char buffer[4096];
  for (ssize_t n; out_read >= 0 && (n = read(out_read, buffer, sizeof buffer)) > 0;)
    text.append(buffer, size_t(n));
  return text;
}

int Process::wait() {
  if (!waited && pid > 0) {
    int status = 0;
    waitpid(pid, &status, 0);
    code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    waited = true;
  }
  return code;
}

Process::~Process() {
  close_input();
  if (out_read >= 0)
    close(out_read);
  wait();
}

#endif

// The text a program prints, and whether it ended well.
bool capture(const std::vector<std::string> &args, std::string &text) {
  auto process = Process::start(args, false, true, {});
  if (!process)
    return false;
  text = process->read_all();
  return process->wait() == 0;
}

std::string read_text(const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The first lines of an error log: FFmpeg prints the cause first.
std::string first_lines(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
    text.pop_back();
  if (text.size() > 600)
    text.resize(600);
  return text;
}

bool is_file(const fs::path &p) {
  std::error_code ec;
  return fs::is_regular_file(p, ec);
}

} // namespace

Result<FfmpegInfo> find_ffmpeg(const std::string &configured) {
  ATM_PROFILE_SCOPE("media.find_ffmpeg");
  std::vector<fs::path> candidates;
  if (!configured.empty())
    candidates.push_back(to_path(configured));
  if (const char *env = std::getenv("ATTOME_FFMPEG"); env && *env)
    candidates.push_back(to_path(env));
  if (const char *path = std::getenv("PATH"); path && *path) {
#if defined(_WIN32)
    constexpr char kSeparator = ';';
    const char *exe = "ffmpeg.exe";
#else
    constexpr char kSeparator = ':';
    const char *exe = "ffmpeg";
#endif
    std::stringstream parts(path);
    for (std::string dir; std::getline(parts, dir, kSeparator);)
      if (!dir.empty())
        candidates.push_back(to_path(dir) / exe);
  }
  for (const fs::path &candidate : candidates) {
    if (!is_file(candidate))
      continue;
    std::string version_text;
    if (!capture({to_utf8(candidate), "-hide_banner", "-version"}, version_text) || version_text.find("ffmpeg version") == std::string::npos)
      continue;
    FfmpegInfo info;
    info.path = to_utf8(candidate);
    const size_t at = version_text.find("ffmpeg version ");
    info.version = version_text.substr(at + 15, version_text.find_first_of(" \r\n", at + 15) - (at + 15));
    // The license of this build, from its configuration line: nonfree (not redistributable), GPL (x264, x265 ...) or LGPL.
    info.license = version_text.find("--enable-nonfree") != std::string::npos ? "nonfree" : version_text.find("--enable-gpl") != std::string::npos ? "gpl" : version_text.find("configuration:") != std::string::npos ? "lgpl" : "unknown";
    std::string encoders;
    capture({to_utf8(candidate), "-hide_banner", "-encoders"}, encoders);
    for (const char *name : {"prores_ks", "dnxhd"})
      if (encoders.find(std::string(" ") + name + " ") != std::string::npos)
        info.encoders.push_back(name);
    return info;
  }
  return fail(ErrorCode::NotFound, "E_NO_FFMPEG", "No FFmpeg program was found on this machine.",
              {},
              "ProRes and DNxHR are written by your own FFmpeg: install one (ffmpeg.org lists builds), then put it on the PATH, set ATTOME_FFMPEG, or pass \"ffmpeg_path\" to media.codecs.");
}

namespace {

struct Spec {
  std::vector<std::string> video_args; // what the video is encoded with
  const char *encoder;                 // the FFmpeg encoder it needs
};

// The FFmpeg arguments of a codec and profile: what the video is encoded with and as which pixel format.
Result<Spec> spec_of(const std::string &codec, const std::string &profile) {
  if (codec == "prores") {
    static const std::pair<const char *, int> kProfiles[] = {{"proxy", 0}, {"lt", 1}, {"standard", 2}, {"hq", 3}, {"4444", 4}, {"4444xq", 5}};
    const std::string wanted = profile.empty() ? "hq" : profile;
    for (const auto &[name, number] : kProfiles)
      if (wanted == name)
        return Spec{{"-c:v", "prores_ks", "-profile:v", std::to_string(number), "-pix_fmt", number >= 4 ? "yuv444p10le" : "yuv422p10le"}, "prores_ks"};
    return fail(ErrorCode::InvalidArgument, "E_PARAM", "The ProRes profile \"" + wanted + "\" is not known.", {}, "Use proxy, lt, standard, hq (the default), 4444 or 4444xq.");
  }
  if (codec == "dnxhr") {
    static const std::pair<const char *, const char *> kProfiles[] = {{"lb", "yuv422p"}, {"sq", "yuv422p"}, {"hq", "yuv422p"}, {"hqx", "yuv422p10le"}, {"444", "yuv444p10le"}};
    const std::string wanted = profile.empty() ? "hq" : profile;
    for (const auto &[name, pix] : kProfiles)
      if (wanted == name)
        return Spec{{"-c:v", "dnxhd", "-profile:v", "dnxhr_" + wanted, "-pix_fmt", pix}, "dnxhd"};
    return fail(ErrorCode::InvalidArgument, "E_PARAM", "The DNxHR profile \"" + wanted + "\" is not known.", {}, "Use lb, sq, hq (the default), hqx or 444.");
  }
  return fail(ErrorCode::InvalidArgument, "E_PARAM", "The codec \"" + codec + "\" is not one FFmpeg writes here.", {}, "Use prores or dnxhr.");
}

} // namespace

struct PipeEncoder::Impl {
  EncodeSettings settings;
  FfmpegInfo ffmpeg;
  fs::path output, video_tmp, audio_tmp, log;
  std::unique_ptr<Process> process;
  std::ofstream audio_out;
  bool wrote_audio = false;
  std::string name;
  std::vector<std::string> video_args;
};

Result<std::unique_ptr<PipeEncoder>> PipeEncoder::create(const EncodeSettings &settings) {
  ATM_TRY(Spec spec, spec_of(settings.codec, settings.profile));
  ATM_TRY(FfmpegInfo found, find_ffmpeg(settings.ffmpeg));
  if (std::find(found.encoders.begin(), found.encoders.end(), spec.encoder) == found.encoders.end())
    return fail(ErrorCode::EncoderUnavailable, "E_NO_ENCODER", "This FFmpeg (" + found.path + ") has no " + spec.encoder + " encoder.", {},
                "Use a full FFmpeg build (the encoder is part of FFmpeg itself, not an add-on library).");
  auto self = std::unique_ptr<PipeEncoder>(new PipeEncoder());
  self->impl_ = std::make_unique<Impl>();
  Impl &m = *self->impl_;
  m.settings = settings;
  m.ffmpeg = std::move(found);
  m.output = to_path(settings.path);
  m.video_tmp = m.output;
  m.video_tmp += ".video.tmp.mov";
  m.audio_tmp = m.output;
  m.audio_tmp += ".audio.tmp.f32";
  m.log = m.output;
  m.log += ".ffmpeg.log";
  m.name = "FFmpeg " + std::string(spec.encoder) + " (" + m.ffmpeg.version + ")";
  m.video_args = spec.video_args;
  // Video frames come in as NV12 on the pipe; BT.709, limited range, as the renderer makes them.
  std::vector<std::string> args = {m.ffmpeg.path, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "nv12", "-video_size",
                                   std::to_string(settings.width) + "x" + std::to_string(settings.height), "-framerate",
                                   std::to_string(settings.rate_num) + "/" + std::to_string(settings.rate_den), "-i", "-"};
  args.insert(args.end(), m.video_args.begin(), m.video_args.end());
  for (const char *a : {"-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709"})
    args.push_back(a);
  args.push_back(to_utf8(m.video_tmp));
  m.process = Process::start(args, true, false, m.log);
  if (!m.process)
    return fail(ErrorCode::IoError, "E_FFMPEG_START", "FFmpeg could not be started (" + m.ffmpeg.path + ").");
  if (settings.audio) {
    m.audio_out.open(m.audio_tmp, std::ios::binary);
    if (!m.audio_out)
      return fail(ErrorCode::IoError, "E_FFMPEG_TEMP", "A temporary file could not be written next to \"" + settings.path + "\".");
  }
  return self;
}

PipeEncoder::~PipeEncoder() {
  if (!impl_)
    return;
  impl_->process.reset(); // closes its input and waits for it
  impl_->audio_out.close();
  std::error_code ec;
  fs::remove(impl_->video_tmp, ec);
  fs::remove(impl_->audio_tmp, ec);
  fs::remove(impl_->log, ec);
}

const std::string &PipeEncoder::name() const { return impl_->name; }

Result<void> PipeEncoder::video(const uint8_t *nv12, int64_t) {
  ATM_PROFILE_SCOPE("encode.pipe.video");
  Impl &m = *impl_;
  const size_t bytes = nv12_size(m.settings.width, m.settings.height);
  if (!m.process->write(nv12, bytes))
    return fail(ErrorCode::IoError, "E_FFMPEG_WRITE", "FFmpeg stopped taking frames: " + first_lines(read_text(m.log)), {}, "The log is next to the output file (.ffmpeg.log).");
  return {};
}

Result<void> PipeEncoder::audio(const float *stereo, size_t frames) {
  Impl &m = *impl_;
  m.audio_out.write(reinterpret_cast<const char *>(stereo), std::streamsize(frames * 2 * sizeof(float)));
  m.wrote_audio = true;
  return m.audio_out ? Result<void>() : fail(ErrorCode::IoError, "E_FFMPEG_TEMP", "The temporary sound file could not be written.");
}

Result<void> PipeEncoder::finish() {
  ATM_PROFILE_SCOPE("encode.pipe.finish");
  Impl &m = *impl_;
  m.process->close_input();
  const int status = m.process->wait();
  m.process.reset();
  m.audio_out.close();
  if (status != 0)
    return fail(ErrorCode::IoError, "E_FFMPEG_FAILED", "FFmpeg could not write the video: " + first_lines(read_text(m.log)), {}, "The log is next to the output file (.ffmpeg.log).");
  std::error_code ec;
  fs::remove(m.output, ec);
  if (!m.wrote_audio) {
    fs::rename(m.video_tmp, m.output, ec);
    return ec ? Result<void>(fail(ErrorCode::IoError, "E_FFMPEG_MOVE", "The video could not be moved to \"" + m.settings.path + "\".")) : Result<void>();
  }
  // The sound is joined to the video without encoding the picture again; ProRes and DNxHR files carry 24-bit PCM.
  const std::vector<std::string> join = {m.ffmpeg.path, "-hide_banner", "-loglevel", "error", "-y", "-i", to_utf8(m.video_tmp), "-f", "f32le", "-ar", "48000", "-ac", "2", "-i",
                                         to_utf8(m.audio_tmp), "-map", "0:v:0", "-map", "1:a:0", "-c:v", "copy", "-c:a", "pcm_s24le", to_utf8(m.output)};
  auto joiner = Process::start(join, false, false, m.log);
  if (!joiner || joiner->wait() != 0)
    return fail(ErrorCode::IoError, "E_FFMPEG_FAILED", "FFmpeg could not join the sound to the video: " + first_lines(read_text(m.log)));
  return {};
}

} // namespace atm::media
