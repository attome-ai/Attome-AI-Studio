// gen.check_take: the checks every Take of a film shot must pass (the skill "film"), measured by the engine so that an agent
// needs no scripts of its own: a cut inside the shot, the spoken words against the script, a voice still speaking at the end,
// a shot that leaves its place, and speech in a shot that has no lines.
#include "engine_impl.hpp"

#include <regex>

namespace atm::api {

namespace {

  // Words to compare: lower case, without ASCII punctuation; what a listener writes for a noise ("[MUSIC]", "*sigh*", "(wind)") is left out.
std::vector<std::string> plain_words(const std::string &text) {
  std::vector<std::string> out;
  std::string word;
  bool noise = false;
  const auto flush = [&] {
    if (!word.empty() && !noise)
      out.push_back(word);
    word.clear();
  };
  for (const char raw : text) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (c == '[' || c == '(' || c == '*') {
      flush();
      noise = c == '*' ? !noise : true;
    } else if (c == ']' || c == ')') {
      word.clear();
      noise = false;
    } else if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      flush();
    } else if (c < 128 && !std::isalnum(c)) {
      // punctuation: dropped ("that's" and "thats" are one word)
    } else {
      word.push_back(c < 128 ? char(std::tolower(c)) : raw);
    }
  }
  flush();
  return out;
}

  // How alike two lists of words are, 0..1: twice the longest common run of words in order, over both lengths.
double alike(const std::vector<std::string> &a, const std::vector<std::string> &b) {
  if (a.empty() && b.empty())
    return 1.0;
  std::vector<std::vector<int>> t(a.size() + 1, std::vector<int>(b.size() + 1, 0));
  for (size_t i = 1; i <= a.size(); ++i)
    for (size_t j = 1; j <= b.size(); ++j)
      t[i][j] = a[i - 1] == b[j - 1] ? t[i - 1][j - 1] + 1 : std::max(t[i - 1][j], t[i][j - 1]);
  return 2.0 * double(t[a.size()][b.size()]) / double(a.size() + b.size());
}

double round2(double v) { return std::round(v * 100.0) / 100.0; }

} // namespace

  // gen.check_take {project, clip, lines?, hold?, cut_limit?, language?, listen?}. The Take that is selected is read from the first frame the
  // clip shows (a clip that carries on from another skips the 22 frames that repeat it, and the word that ends under them).
Result<json> Engine::Impl::gen_check_take(const json &params) {
  ATM_PROFILE_SCOPE("api.gen_check_take");
  ATM_TRY(Project *pr, project(params));
  ATM_TRY(const std::string *clip, string_param(params, "clip"));
  const doc::NodeRef *ref = pr->doc.find(*clip);
  const json media = ref && ref->node ? ref->node->value("media_ref", json::object()) : json::object();
  if (media.value("type", std::string()) != "workflow")
    return fail(ErrorCode::NotFound, "G_CLIP", "\"" + *clip + "\" is not a generative clip of this project.", {}, "Pass the clip ID gen.create_clip returned.");
  const std::string selected = media.value("selected", std::string());
  const json takes = media.value("takes", json::object());
  const json outputs = takes.contains(selected) ? takes[selected].value("outputs", json::object()) : json::object();
  if (!outputs.contains("video") || !outputs["video"].is_object() || !outputs["video"].contains("path"))
    return fail(ErrorCode::InvalidArgument, "G_NOT_MADE", "This clip has no Take with a picture yet, so there is nothing to check.", {}, "Make it first with gen.run.");
  fs::path file = to_path(outputs["video"]["path"].get<std::string>());
  if (file.is_relative())
    file = pr->dir / file;
  const std::string path = to_utf8(file);
  ATM_TRY(json info, media_probe({{"path", path}}));
  const double seconds = info.value("seconds", 0.0);
  const json timing = ref->node->value("timing", json::object());
  const double skip = std::clamp(Rational::parse(timing.value("source_in", std::string("0"))).value_or(Rational()).to_seconds_lossy(), 0.0, std::max(0.0, seconds - 0.2));

  // The script: the lines given, or the ones written in the clip's prompt (<d>[English] ...</d>, as gen.dialogue_prompt writes them).
  const std::string prompt = media.value("inputs", json::object()).value("prompt", std::string());
  std::vector<std::string> lines;
  std::string language = params.value("language", std::string());
  if (params.contains("lines")) {
    if (!params["lines"].is_array())
      return bad_param("lines", "must be a list of the lines spoken in the shot (an empty list: nobody speaks)");
    for (const json &l : params["lines"])
      if (l.is_string())
        lines.push_back(l.get<std::string>());
  } else {
    static const std::regex said(R"(<d>\s*(?:\[([^\]]*)\])?\s*([\s\S]*?)</d>)");
    for (auto it = std::sregex_iterator(prompt.begin(), prompt.end(), said); it != std::sregex_iterator(); ++it) {
      lines.push_back((*it)[2].str());
      if (language.empty() && (*it)[1].matched) {
        std::string name = (*it)[1].str();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        static const std::map<std::string, std::string> codes = {{"english", "en"}, {"arabic", "ar"}, {"french", "fr"}, {"german", "de"}, {"spanish", "es"},
                                                                 {"italian", "it"}, {"japanese", "ja"}, {"chinese", "zh"}, {"korean", "ko"}, {"portuguese", "pt"},
                                                                 {"russian", "ru"}, {"turkish", "tr"}, {"hindi", "hi"}};
        if (const auto c = codes.find(name); c != codes.end())
          language = c->second;
      }
    }
  }
  if (language.empty())
    language = "auto";
  std::string want_text;
  for (const std::string &l : lines)
    want_text += (want_text.empty() ? "" : " ") + l;
  const std::vector<std::string> want = plain_words(want_text);

  json problems = json::array();
  const auto problem = [&](const char *check, std::string message, const char *hint) {
    problems.push_back({{"check", check}, {"message", std::move(message)}, {"hint", hint}});
  };
  json out = {{"clip", *clip}, {"take", selected}, {"path", path}, {"seconds", round2(seconds - skip)}, {"skipped", round2(skip)}};

  // The picture: every frame as a small grey copy. A cut is a jump in how much the picture changes (the measure of a scene detector:
  // the mean step of a pixel between two frames, and how much that differs from the step before; the smaller of the two, 0..1).
  // Leaving its place is the first frame no longer resembling a later one (1 - their correlation; the largest).
  if (info.value("has_video", false)) {
    const double rate = std::clamp(Rational::parse(info.value("rate", std::string("24"))).value_or(Rational::from_int(24)).to_seconds_lossy(), 1.0, 120.0);
    ATM_TRY(auto reader, media::VideoReader::open(path, 96, 96));
    std::vector<uint8_t> previous, grey;
    std::vector<double> first;
    double first_norm = 0.0, last_step = -1.0, cut = 0.0, cut_at = 0.0, drift = 0.0;
    const int count = int((seconds - skip) * rate);
    for (int i = 0; i < count; ++i) {
      const double t = skip + double(i) / rate;
      ATM_TRY(media::FrameView f, reader->frame_at(int64_t(t * double(media::kHnsPerSecond))));
      grey.assign(size_t(f.width) * size_t(f.height), 0);
      for (int y = 0; y < f.height; ++y)
        std::memcpy(&grey[size_t(y) * size_t(f.width)], f.y + size_t(y) * size_t(f.y_pitch), size_t(f.width));
      if (previous.size() == grey.size()) {
        uint64_t acc = 0;
        for (size_t p = 0; p < grey.size(); ++p)
          acc += uint64_t(std::abs(int(grey[p]) - int(previous[p])));
        const double step = double(acc) / double(grey.size());
        if (last_step >= 0.0) {
          const double score = std::clamp(std::min(step, std::abs(step - last_step)) / 100.0, 0.0, 1.0);
          if (score > cut) {
            cut = score;
            cut_at = t - skip;
          }
        }
        last_step = step;
      }
      if (i == 0 || i % 12 == 0) {
        double mean = 0.0;
        for (uint8_t v : grey)
          mean += v;
        mean /= double(grey.size());
        std::vector<double> centred(grey.size());
        double norm = 0.0;
        for (size_t p = 0; p < grey.size(); ++p) {
          centred[p] = double(grey[p]) - mean;
          norm += centred[p] * centred[p];
        }
        if (i == 0) {
          first = std::move(centred);
          first_norm = norm;
        } else if (first.size() == centred.size()) {
          double dot = 0.0;
          for (size_t p = 0; p < first.size(); ++p)
            dot += first[p] * centred[p];
          drift = std::max(drift, 1.0 - dot / (std::sqrt(first_norm * norm) + 1e-9));
        }
      }
      previous = grey;
    }
    const double cut_limit = params.value("cut_limit", 0.15);
    out["cut"] = {{"score", round2(cut)}, {"at", round2(cut_at)}, {"limit", cut_limit}};
    out["drift"] = round2(drift);
    if (cut > cut_limit)
      problem("cut", "The picture jumps inside the shot at " + std::to_string(round2(cut_at)).substr(0, 4) + " s (score " + std::to_string(round2(cut)).substr(0, 4) +
                         "): a cut, often to a copy of a reference picture.",
              "Make another take (gen.run new_take). If every take cuts, look at the cast's reference picture (a tight crop of the whole face) and at the prompt. "
              "For fast action with flashes pass cut_limit 0.7 and look at the frames.");
    if (params.value("hold", false) && drift > 0.85)
      problem("hold", "The shot leaves the picture it started on (drift " + std::to_string(round2(drift)).substr(0, 4) + ").",
              "Make another take; if it keeps leaving, start on a part of the frame with one person and at most a sliver of the other.");
  }

  // The sound: is someone still speaking in the last 0.15 s (speech about -20 dB, a quiet place about -45), and what is said.
  if (info.value("has_audio", false)) {
    ATM_TRY(std::vector<float> tail, media::read_audio(path, int64_t(std::max(0.0, seconds - 0.15) * double(media::kHnsPerSecond)), int64_t(0.15 * double(media::kHnsPerSecond))));
    double sum = 0.0;
    for (float v : tail)
      sum += double(v) * double(v);
    const double end_db = tail.empty() ? -90.0 : 20.0 * std::log10(std::sqrt(sum / double(tail.size())) + 1e-9);
    out["end_db"] = std::round(end_db * 10.0) / 10.0;
    if (!want.empty() && end_db > -34.0)
      problem("end", "Someone is still speaking when the clip ends (" + std::to_string(int(std::lround(end_db))) + " dB in the last 0.15 s).",
              "Make another take, or give the clip a second more: the model paces the speech to the end of the clip.");
    if (params.value("listen", true)) {
      std::string model_id;
      auto options = asr_options("best", language, "auto", model_id);
      if (!options) {
        out["heard"] = nullptr;
        out["not_heard"] = options.error().message + " " + options.error().hint;
      } else {
        ATM_TRY(asr::Transcript said, asr::transcribe(*options, path, skip, seconds - skip, {}, nullptr));
        std::string heard_text;
        for (const asr::Word &w : said.words)
          heard_text += (heard_text.empty() ? "" : " ") + w.text;
        const std::vector<std::string> heard = plain_words(heard_text);
        out["heard"] = heard_text;
        out["lines"] = lines;
        if (want.empty()) {
          size_t real = 0; // a stray syllable is not speech
          for (const std::string &w : heard)
            real += w.size() > 1 ? 1 : 0;
          if (real >= 2)
            problem("silent", "Nobody has a line in this shot, but words are heard: \"" + heard_text + "\".",
                    "Make another take with \"their mouths are closed, nobody speaks, no voice is heard\" in the action, or use the sound of an empty shot of the place.");
        } else {
          const double match = alike(want, heard);
          out["match"] = round2(match);
          if (match < 0.9)
            problem("words", "What is said is not the script (" + std::to_string(int(std::lround(match * 100.0))) + " % alike): \"" + heard_text + "\".",
                    "Make another take. Lines that are too long for the clip are cut or rushed: about 2 words a second.");
          else if (heard.size() > want.size())
            problem("extra_words", "Words were added to the script: \"" + heard_text + "\".", "Make another take.");
        }
      }
    }
  } else if (!want.empty()) {
    problem("words", "The Take has no sound, but the shot has lines.", "Create the clip with with_audio true.");
  }
  out["ok"] = problems.empty();
  out["problems"] = std::move(problems);
  return out;
}

} // namespace atm::api
