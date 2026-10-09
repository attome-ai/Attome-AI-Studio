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
#endif
  std::string model, language = "auto";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--model"))
      model = argv[i + 1];
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
  cparams.use_gpu = false; // the processor: the first version runs the same everywhere
  whisper_context *ctx = whisper_init_from_file_with_params(model.c_str(), cparams);
  if (!ctx)
    return fail("the model could not be loaded: " + model);

  whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.n_threads = int(std::max(1u, std::thread::hardware_concurrency() / 2));
  params.print_progress = params.print_realtime = params.print_timestamps = params.print_special = false;
  params.token_timestamps = true; // a segment of one word each (the same as whisper-cli -ml 1 -sow)
  params.max_len = 1;
  params.split_on_word = true;
  params.no_context = true;
  params.language = language.c_str();
  params.detect_language = false;
  params.progress_callback = on_progress;
  if (whisper_full(ctx, params, pcm.data(), int(pcm.size())) != 0) {
    whisper_free(ctx);
    return fail("the speech could not be read");
  }

  nlohmann::json words = nlohmann::json::array();
  for (int i = 0, n = whisper_full_n_segments(ctx); i < n; ++i) {
    std::string text = whisper_full_get_segment_text(ctx, i);
    const size_t a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
    text = a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
    if (is_annotation(text))
      continue;
    const double start = double(whisper_full_get_segment_t0(ctx, i)) / 100.0, end = double(whisper_full_get_segment_t1(ctx, i)) / 100.0;
    words.push_back({{"t", text}, {"s", start}, {"e", std::max(end, start)}});
  }
  const char *heard = whisper_lang_str(whisper_full_lang_id(ctx));
  say({{"words", std::move(words)}, {"language", heard ? heard : language}});
  whisper_free(ctx);
  return 0;
}
