// A stand-in for attome-whisper in the tests: the same protocol (core/atm_asr/include/atm/asr/asr.hpp), no model. What it does depends on the
// text in the "model" file it is given:
//   (anything else)  progress, then one word for each second of sound, "w0" at 0 s, "w1" at 1 s ..., each 0.8 s long
//   crash            exits at once with a code and no output
//   error            prints {"error": ...}
//   slow             prints progress and then waits half a minute (a test cancels it)
//   noise            prints a line that is not JSON

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::string model, language = "auto";
  bool gpu = false;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--model"))
      model = argv[i + 1];
    else if (!std::strcmp(argv[i], "--language"))
      language = argv[i + 1];
    else if (!std::strcmp(argv[i], "--gpu"))
      gpu = std::strcmp(argv[i + 1], "0") != 0;
  }
  uint32_t count = 0;
  if (std::fread(&count, sizeof count, 1, stdin) != 1)
    return 4;
  std::vector<float> pcm(count);
  if (std::fread(pcm.data(), sizeof(float), count, stdin) != count)
    return 4;
  std::ifstream in(model, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  const std::string mode = text.str();

  if (mode.rfind("crash", 0) == 0)
    return 3;
  if (mode.rfind("error", 0) == 0) {
    std::puts("{\"error\":\"the model is broken\"}");
    return 2;
  }
  if (mode.rfind("noise", 0) == 0) {
    std::puts("this is not json");
    return 0;
  }
  std::puts("{\"progress\":0.25}");
  std::fflush(stdout);
  if (mode.rfind("slow", 0) == 0) {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 0;
  }
  std::puts("{\"progress\":0.9}");
  const int seconds = int(count / 16000);
  std::string words;
  for (int i = 0; i < seconds; ++i)
    words += std::string(i ? "," : "") + "{\"t\":\"w" + std::to_string(i) + "\",\"s\":" + std::to_string(i) + ",\"e\":" + std::to_string(i + 0.8) + "}";
  std::printf("{\"words\":[%s],\"language\":\"%s\",\"device\":\"%s\"}\n", words.c_str(), language == "auto" ? "en" : language.c_str(), gpu ? "gpu" : "cpu");
  return 0;
}
