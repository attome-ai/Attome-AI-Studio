#include "atm/asr/asr.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "atm/base/profiler.hpp"
#include "atm/media/media.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace atm::asr {
namespace fs = std::filesystem;

namespace {
constexpr int kTaps = 95;        // odd: the filter is centred on a sample
constexpr double kCutoff = 7300; // Hz, at 48 kHz: with a Hamming window of this length the response is 6 dB down here and 50 dB down by 8.2 kHz
} // namespace

Downsampler::Downsampler() : taps_(kTaps), pending_(size_t(kTaps / 2), 0.0f) { // silence before the start, so the first output sits at the first input
  const double pi = 3.14159265358979323846, fc = kCutoff / 48000.0;
  double sum = 0.0;
  for (int i = 0; i < kTaps; ++i) {
    const int m = i - kTaps / 2;
    const double sinc = m == 0 ? 2.0 * fc : std::sin(2.0 * pi * fc * m) / (pi * m);
    const double window = 0.54 - 0.46 * std::cos(2.0 * pi * i / (kTaps - 1));
    taps_[size_t(i)] = float(sinc * window);
    sum += sinc * window;
  }
  for (float &t : taps_)
    t = float(t / sum); // a steady level passes unchanged
}

void Downsampler::run(std::vector<float> &mono16k) {
  size_t at = skip_;
  for (; at + size_t(kTaps) <= pending_.size(); at += 3) {
    const float *p = pending_.data() + at;
    float v = 0.0f;
    for (int i = 0; i < kTaps; ++i)
      v += p[i] * taps_[size_t(i)];
    mono16k.push_back(v);
  }
  const size_t used = std::min(at, pending_.size());
  skip_ = at - used;
  pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(used));
}

void Downsampler::push(const float *stereo48k, size_t frames, std::vector<float> &mono16k) {
  pending_.reserve(pending_.size() + frames);
  for (size_t i = 0; i < frames; ++i)
    pending_.push_back((stereo48k[i * 2] + stereo48k[i * 2 + 1]) * 0.5f);
  mono16k.reserve(mono16k.size() + frames / 3 + 1);
  run(mono16k);
}

void Downsampler::finish(std::vector<float> &mono16k) {
  pending_.insert(pending_.end(), size_t(kTaps / 2), 0.0f); // silence after the end, for the samples the filter still looks ahead to
  run(mono16k);
}

Result<Line> parse_line(std::string_view text) {
  const nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
  if (!j.is_object())
    return fail(ErrorCode::CorruptData, "E_ASR_PROTOCOL", "The speech program printed something that is not a JSON object.", {},
                "Its output must be one JSON object a line (asr.hpp).");
  Line line;
  if (j.contains("error")) {
    line.kind = Line::Kind::error;
    line.message = j["error"].is_string() ? j["error"].get<std::string>() : j["error"].dump();
  } else if (j.contains("words")) {
    line.kind = Line::Kind::result;
    line.transcript.language = j.value("language", std::string());
    line.transcript.device = j.value("device", std::string("cpu"));
    if (!j["words"].is_array())
      return fail(ErrorCode::CorruptData, "E_ASR_PROTOCOL", "\"words\" must be a list.");
    for (const nlohmann::json &w : j["words"]) {
      if (!w.is_object() || !w.contains("t") || !w["t"].is_string() || !w.contains("s") || !w["s"].is_number() || !w.contains("e") ||
          !w["e"].is_number())
        return fail(ErrorCode::CorruptData, "E_ASR_PROTOCOL", "A word must be {\"t\": text, \"s\": start, \"e\": end}.");
      line.transcript.words.push_back({w["t"].get<std::string>(), w["s"].get<double>(), w["e"].get<double>()});
    }
  } else if (j.contains("progress") && j["progress"].is_number()) {
    line.kind = Line::Kind::progress;
    line.progress = std::clamp(j["progress"].get<double>(), 0.0, 1.0);
  } else {
    return fail(ErrorCode::CorruptData, "E_ASR_PROTOCOL", "A line of the speech program has none of progress, words or error.");
  }
  return line;
}

fs::path find_runtime() {
  if (const char *env = std::getenv("ATTOME_WHISPER_EXE"); env && *env) {
    std::error_code ec;
    return fs::exists(fs::path(env), ec) ? fs::path(env) : fs::path();
  }
#if defined(_WIN32)
  wchar_t self[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, self, DWORD(sizeof self / sizeof self[0]));
  if (n > 0 && n < sizeof self / sizeof self[0]) {
    const fs::path beside = fs::path(self).parent_path() / "attome-whisper.exe";
    std::error_code ec;
    if (fs::exists(beside, ec))
      return beside;
  }
#endif
  return {};
}

#if defined(_WIN32)
namespace {

std::wstring quoted(const std::wstring &a) { // one argument as the Windows command line wants it
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
  out += L'"';
  return out;
}

// The child process with a pipe to its input and one from its output, closed with it.
struct Child {
  HANDLE process = nullptr, in_write = nullptr, out_read = nullptr;
  ~Child() {
    if (in_write)
      CloseHandle(in_write);
    if (out_read)
      CloseHandle(out_read);
    if (process) {
      if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
        TerminateProcess(process, 1);
      WaitForSingleObject(process, INFINITE);
      CloseHandle(process);
    }
  }
  bool running() const { return WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
  void kill() { TerminateProcess(process, 1); }
  void close_input() {
    if (in_write)
      CloseHandle(in_write);
    in_write = nullptr;
  }
};

bool start_child(const Options &options, Child &child) {
  SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
  HANDLE in_read = nullptr, out_write = nullptr, nul = nullptr;
  if (!CreatePipe(&in_read, &child.in_write, &inherit, 0))
    return false;
  SetHandleInformation(child.in_write, HANDLE_FLAG_INHERIT, 0); // the child gets the reading end only
  if (!CreatePipe(&child.out_read, &out_write, &inherit, 0)) {
    CloseHandle(in_read);
    return false;
  }
  SetHandleInformation(child.out_read, HANDLE_FLAG_INHERIT, 0);
  nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr); // its log text goes nowhere
  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = in_read;
  si.hStdOutput = out_write;
  si.hStdError = nul;
  const std::wstring language(options.language.begin(), options.language.end());
  std::wstring command = quoted(options.exe.wstring()) + L" --model " + quoted(options.model.wstring()) + L" --language " + quoted(language) + (options.gpu ? L" --gpu 1" : L"");
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(options.exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  for (HANDLE h : {in_read, out_write, nul})
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  if (!ok)
    return false;
  CloseHandle(pi.hThread);
  child.process = pi.hProcess;
  return true;
}

} // namespace
#endif

Result<Transcript> transcribe_pcm(const Options &options, const std::vector<float> &mono16k, const std::function<void(double)> &progress,
                                  const std::atomic<bool> *cancel) {
  ATM_PROFILE_SCOPE("asr.transcribe_pcm");
#if !defined(_WIN32)
  (void)options, (void)mono16k, (void)progress, (void)cancel;
  return fail(ErrorCode::Unsupported, "E_ASR_UNSUPPORTED", "Speech to text runs on Windows in this version.");
#else
  const auto cancelled = [&] { return cancel && cancel->load(); };
  if (options.exe.empty() || !fs::exists(options.exe))
    return fail(ErrorCode::WorkerUnavailable, "E_ASR_NOT_INSTALLED", "The speech program (attome-whisper) is not installed.", {},
                "It is built with Attome when whisper.cpp is in .deps/whisper.cpp.");
  if (!fs::exists(options.model))
    return fail(ErrorCode::ModelMissing, "E_ASR_MODEL", "The speech model is not on this computer.", {}, "models.fetch whisper.small downloads it.");
  if (mono16k.empty())
    return fail(ErrorCode::InvalidArgument, "E_ASR_EMPTY", "There is no sound to listen to.");
  if (cancelled())
    return fail(ErrorCode::Cancelled, "E_CANCELLED", "Cancelled.");

  Child child;
  if (!start_child(options, child))
    return fail(ErrorCode::WorkerUnavailable, "E_ASR_START", "The speech program could not be started.", {}, "Check that " + options.exe.string() + " runs.");

  // The sound goes in first, in 1 MB pieces so a cancel is noticed; the program reads it all before it says anything, so neither pipe fills.
  const uint32_t count = uint32_t(mono16k.size());
  bool written = [&] {
    DWORD done = 0;
    if (!WriteFile(child.in_write, &count, sizeof count, &done, nullptr))
      return false;
    const char *p = reinterpret_cast<const char *>(mono16k.data());
    size_t left = mono16k.size() * sizeof(float);
    while (left > 0) {
      if (cancelled())
        return true; // the loop below sees the flag
      if (!WriteFile(child.in_write, p, DWORD(std::min<size_t>(left, size_t(1) << 20)), &done, nullptr) || done == 0)
        return false;
      p += done;
      left -= done;
    }
    return true;
  }();
  child.close_input();

  std::string pending, error_text;
  Transcript result;
  bool have_result = false;
  const auto take_lines = [&](bool last) -> Result<void> {
    size_t at;
    while ((at = pending.find('\n')) != std::string::npos || (last && !pending.empty())) {
      const std::string text = at == std::string::npos ? pending : pending.substr(0, at);
      pending.erase(0, at == std::string::npos ? pending.size() : at + 1);
      if (text.find_first_not_of(" \r\t") == std::string::npos)
        continue;
      ATM_TRY(Line line, parse_line(text));
      switch (line.kind) {
      case Line::Kind::progress:
        if (progress)
          progress(line.progress);
        break;
      case Line::Kind::result:
        result = std::move(line.transcript);
        have_result = true;
        break;
      case Line::Kind::error:
        error_text = std::move(line.message);
        break;
      }
    }
    return {};
  };
  for (;;) {
    if (cancelled()) {
      child.kill();
      return fail(ErrorCode::Cancelled, "E_CANCELLED", "Cancelled.");
    }
    DWORD available = 0;
    if (!PeekNamedPipe(child.out_read, nullptr, 0, nullptr, &available, nullptr))
      break; // the program closed its output
    if (available > 0) {
      char buffer[4096];
      DWORD n = 0;
      if (!ReadFile(child.out_read, buffer, DWORD(std::min<size_t>(sizeof buffer, available)), &n, nullptr) || n == 0)
        break;
      pending.append(buffer, n);
      ATM_CHECK(take_lines(false));
    } else if (!child.running()) {
      break;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  ATM_CHECK(take_lines(true));
  if (!error_text.empty())
    return fail(ErrorCode::ProviderError, "E_ASR_FAILED", "The speech program says: " + error_text);
  if (!have_result)
    return fail(ErrorCode::WorkerCrashed, "E_ASR_CRASH", written ? "The speech program stopped without a result." : "The speech program stopped before it took the sound.");
  return result;
#endif
}

Result<Transcript> transcribe(const Options &options, const std::string &media_path, double from_s, double duration_s,
                              const std::function<void(double)> &progress, const std::atomic<bool> *cancel) {
  ATM_PROFILE_SCOPE("asr.transcribe");
  ATM_TRY(media::MediaInfo info, media::probe(media_path));
  if (!info.has_audio)
    return fail(ErrorCode::InvalidArgument, "E_ASR_NO_SOUND", "The file has no sound.", media_path);
  const int64_t file_hns = info.duration_hns;
  const int64_t from_hns = std::clamp<int64_t>(int64_t(from_s * double(media::kHnsPerSecond)), 0, file_hns);
  int64_t length_hns = duration_s > 0.0 ? int64_t(duration_s * double(media::kHnsPerSecond)) : file_hns - from_hns;
  length_hns = std::clamp<int64_t>(length_hns, 0, file_hns - from_hns);
  if (length_hns <= 0)
    return fail(ErrorCode::InvalidArgument, "E_ASR_EMPTY", "There is no sound in that stretch.");

  std::vector<float> mono;
  mono.reserve(size_t(double(length_hns) / double(media::kHnsPerSecond) * kRate) + 16);
  const int64_t chunk_hns = 30 * media::kHnsPerSecond;
  Downsampler down;
  for (int64_t done = 0; done < length_hns; done += chunk_hns) {
    if (cancel && cancel->load())
      return fail(ErrorCode::Cancelled, "E_CANCELLED", "Cancelled.");
    ATM_TRY(std::vector<float> chunk, media::read_audio(media_path, from_hns + done, std::min(chunk_hns, length_hns - done)));
    down.push(chunk.data(), chunk.size() / 2, mono);
    if (progress)
      progress(0.1 * double(std::min(done + chunk_hns, length_hns)) / double(length_hns));
  }
  down.finish(mono);
  if (progress)
    progress(0.1);
  const auto child_progress = [&](double p) {
    if (progress)
      progress(0.1 + 0.9 * p);
  };
  return transcribe_pcm(options, mono, child_progress, cancel);
}

} // namespace atm::asr
