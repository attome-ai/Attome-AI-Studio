// attome-whisper: speech to text for Attome, a thin program around whisper.cpp. It is started by the engine (core/atm_asr), reads mono 16 kHz
// sound from its standard input and prints the words with their times as JSON lines on its standard output. The protocol is in
// core/atm_asr/include/atm/asr/asr.hpp. Nothing here listens on a network port.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "whisper.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

void say(const nlohmann::json &line) {
  const std::string text = line.dump() + "\n";
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);
}

int fail(const std::string &message) {
  say({{"error", message}});
  return 2;
}

void on_progress(whisper_context *, whisper_state *, int percent, void *) { say({{"progress", double(percent) / 100.0}}); }

// What Whisper prints for sounds that are not speech: [BLANK_AUDIO], (music), [Music], *applause* ...
bool is_annotation(const std::string &w) {
  if (w.empty())
    return true;
  const char first = w.front(), last = w.back();
  return (first == '[' && last == ']') || (first == '(' && last == ')') || (first == '*' && last == '*') || (first == '<' && last == '>');
}

} // namespace

int main(int argc, char **argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  // The arguments as UTF-8: argv is in the system's own code page, which loses a model path in another script (whisper.cpp takes its paths
  // as UTF-8).
  std::vector<std::string> utf8_args;
  std::vector<char *> utf8_argv;
  int wide_count = 0;
  if (wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &wide_count)) {
    for (int i = 0; i < wide_count; ++i) {
      const int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
      std::string one(size_t(std::max(n, 1)), '\0');
      WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, one.data(), n, nullptr, nullptr);
      one.resize(std::strlen(one.c_str()));
      utf8_args.push_back(std::move(one));
    }
    LocalFree(wide);
    for (std::string &a : utf8_args)
      utf8_argv.push_back(a.data());
    argc = int(utf8_argv.size());
    argv = utf8_argv.data();
  }
#endif
  std::string model, language = "auto";
  bool gpu = false; // --gpu 1: the graphics card through Vulkan (with flash attention), when the build has it and a device takes the model
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--model"))
      model = argv[i + 1];
    else if (!std::strcmp(argv[i], "--gpu"))
      gpu = std::strcmp(argv[i + 1], "0") != 0;
    else if (!std::strcmp(argv[i], "--language"))
      language = argv[i + 1];
  }
  if (model.empty())
    return fail("no --model given");

  uint32_t count = 0;
  if (std::fread(&count, sizeof count, 1, stdin) != 1 || count == 0)
    return fail("no sound on the input");
  std::vector<float> pcm(count);
  if (std::fread(pcm.data(), sizeof(float), count, stdin) != count)
    return fail("the sound ended before its length");

  whisper_log_set([](ggml_log_level, const char *, void *) {}, nullptr); // nothing but the protocol goes to the output
  whisper_context_params cparams = whisper_context_default_params();
  cparams.use_gpu = gpu;
  cparams.flash_attn = gpu;
  // Word times come from the decoder's attention (dynamic time warping, DTW) when the model's alignment heads are known, which is by its
  // file name: against a voice whose word positions are known, 97-98 % of the words then start within 0.2 s of the truth, and 65-78 % with
  // the other way, one-word segments, which is what a model of another name gets (tools/asr_timing_eval.py).
  const std::string name = model.substr(model.find_last_of("/\\") == std::string::npos ? 0 : model.find_last_of("/\\") + 1);
  const whisper_alignment_heads_preset heads = name.find("large-v3-turbo") != std::string::npos ? WHISPER_AHEADS_LARGE_V3_TURBO
                                               : name.find("large-v3") != std::string::npos     ? WHISPER_AHEADS_LARGE_V3
                                               : name.find("small") != std::string::npos        ? WHISPER_AHEADS_SMALL
                                                                                                : WHISPER_AHEADS_NONE;
  const bool dtw = heads != WHISPER_AHEADS_NONE;
  if (dtw) {
    cparams.dtw_token_timestamps = true;
    cparams.dtw_aheads_preset = heads;
    cparams.flash_attn = false; // DTW reads the attention weights, which flash attention does not keep
  }
  whisper_context *ctx = whisper_init_from_file_with_params(model.c_str(), cparams);
  if (!ctx && gpu) { // no device took it: the processor
    cparams.use_gpu = cparams.flash_attn = false;
    gpu = false;
    ctx = whisper_init_from_file_with_params(model.c_str(), cparams);
  }
  if (!ctx)
    return fail("the model could not be loaded: " + model);

  whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.n_threads = int(std::max(1u, std::thread::hardware_concurrency() / 2));
  params.print_progress = params.print_realtime = params.print_timestamps = params.print_special = false;
  if (!dtw) { // a segment of one word each (the same as whisper-cli -ml 1 -sow)
    params.token_timestamps = true;
    params.max_len = 1;
    params.split_on_word = true;
  }
  params.no_context = true;
  params.language = language.c_str();
  params.detect_language = false;
  params.progress_callback = on_progress;
  if (whisper_full(ctx, params, pcm.data(), int(pcm.size())) != 0) {
    whisper_free(ctx);
    return fail("the speech could not be read");
  }

  nlohmann::json words = nlohmann::json::array();
  // A token's DTW time is about when the decoder put it out, which comes after the word began: 0.18 to 0.21 s later, steadily, for both models and
  // both voices measured. That much is taken off.
  constexpr double kDtwLate = 0.2;
  if (dtw) { // words from tokens: a token that starts with a space starts a word; it starts at its token's DTW time and ends when the next begins
    const whisper_token eot = whisper_token_eot(ctx);
    struct W { std::string text; double start; };
    std::vector<W> found;
    std::vector<double> segment_end;
    for (int i = 0, n = whisper_full_n_segments(ctx); i < n; ++i) {
      for (int k = 0, m = whisper_full_n_tokens(ctx, i); k < m; ++k) {
        if (whisper_full_get_token_id(ctx, i, k) >= eot)
          continue; // timestamps and other special tokens
        const std::string piece = whisper_full_get_token_text(ctx, i, k);
        const double at = double(whisper_full_get_token_data(ctx, i, k).t_dtw) / 100.0;
        if (found.empty() || (!piece.empty() && piece.front() == ' '))
          found.push_back({piece, at});
        else
          found.back().text += piece;
      }
      segment_end.push_back(double(whisper_full_get_segment_t1(ctx, i)) / 100.0);
    }
    const double last_end = segment_end.empty() ? 0.0 : segment_end.back();
    for (size_t w = 0; w < found.size(); ++w) {
      std::string text = found[w].text;
      const size_t a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
      text = a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
      if (is_annotation(text))
        continue;
      const double start = std::max(0.0, found[w].start - kDtwLate);
      const double next = w + 1 < found.size() ? found[w + 1].start - kDtwLate : last_end;
      const double end = std::max(start, next);
      words.push_back({{"t", text}, {"s", start}, {"e", end}});
    }
  }
  for (int i = 0, n = dtw ? 0 : whisper_full_n_segments(ctx); i < n; ++i) {
    std::string text = whisper_full_get_segment_text(ctx, i);
    const size_t a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
    text = a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
    if (is_annotation(text))
      continue;
    const double start = double(whisper_full_get_segment_t0(ctx, i)) / 100.0, end = double(whisper_full_get_segment_t1(ctx, i)) / 100.0;
    words.push_back({{"t", text}, {"s", start}, {"e", std::max(end, start)}});
  }
  const char *heard = whisper_lang_str(whisper_full_lang_id(ctx));
  say({{"words", std::move(words)}, {"language", heard ? heard : language}, {"device", gpu ? "gpu" : "cpu"}});
  whisper_free(ctx);
  return 0;
}
