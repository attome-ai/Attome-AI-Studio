#include "atm/gen/dialogue.hpp"

namespace atm::gen {
namespace {

std::string text_of(const json &object, const char *key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string trimmed(std::string s) {
  const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && blank(s.back()))
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && blank(s[i]))
    ++i;
  return s.substr(i);
}

// A sentence ends with a full stop (or ! ?), so the next one can follow it.
std::string sentence(std::string s) {
  s = trimmed(std::move(s));
  if (!s.empty() && s.back() != '.' && s.back() != '!' && s.back() != '?')
    s += '.';
  return s;
}

} // namespace

std::string dialogue_prompt(const json &spec, std::string &problem) {
  problem.clear();
  if (!spec.is_object()) {
    problem = "The description must be an object with subjects, shots and so on.";
    return {};
  }
  const json subjects = spec.value("subjects", json::array());
  const json shots = spec.value("shots", json::array());
  if (!subjects.is_array() || !shots.is_array()) {
    problem = "\"subjects\" and \"shots\" must be lists.";
    return {};
  }
  if (shots.empty()) {
    problem = "Give at least one shot: what happens, and what is said.";
    return {};
  }
  const size_t count = subjects.size();
  std::vector<bool> voiced(count, false);
  std::string out = "subject_definitions:\n";
  int audios = 0;
  std::vector<int> audio_of(count, 0);
  for (size_t i = 0; i < count; ++i) {
    const json &s = subjects[i];
    const std::string who = s.is_string() ? s.get<std::string>() : text_of(s, "description");
    if (trimmed(who).empty()) {
      problem = "Subject " + std::to_string(i + 1) + " has no description.";
      return {};
    }
    const std::string tag = "<Subject " + std::to_string(i + 1) + ">";
    std::string line = tag + " is " + trimmed(who);
    if (!line.empty() && line.back() == '.')
      line.pop_back();
    if (s.is_object() && s.value("voice", false)) {
      voiced[i] = true;
      audio_of[i] = ++audios;
      line += " and uses <Audio " + std::to_string(audio_of[i]) + "> as their speaking voice timbre reference";
    }
    out += line + ".\n";
  }
  if (count == 0)
    out += "N/A\n";

  const std::string style = trimmed(text_of(spec, "style"));
  std::string summary = audios > 0 ? "[reference generation + audio reference]" : "[reference generation]";
  summary += " " + (style.empty() ? std::string("A cinematic scene") : sentence(style));
  if (spec.value("continuous", true))
    summary += " A single continuous shot from beginning to end, no cuts.";
  out += "\nsummary:\n" + summary + "\n";

  out += "\nretention_analysis:\n";
  if (audios == 0) {
    out += "N/A\n";
  } else {
    for (size_t i = 0; i < count; ++i)
      if (voiced[i])
        out += "<Audio " + std::to_string(audio_of[i]) + ">: reference - its vocal timbre guides the vocal delivery of <Subject " +
               std::to_string(i + 1) + "> without copying the original signal.\n";
  }

  out += "\ndetailed_description:\n";
  if (const std::string setting = trimmed(text_of(spec, "setting")); !setting.empty())
    out += sentence(setting) + "\n";
  int shot_number = 0;
  for (const json &shot : shots) {
    ++shot_number;
    if (!shot.is_object()) {
      problem = "Shot " + std::to_string(shot_number) + " must be an object with camera, action and lines.";
      return {};
    }
    std::string text = "\n[Shot " + std::to_string(shot_number) + "]:";
    if (const std::string camera = trimmed(text_of(shot, "camera")); !camera.empty())
      text += " " + sentence(camera);
    if (const std::string action = trimmed(text_of(shot, "action")); !action.empty())
      text += " " + sentence(action);
    if (shot.contains("lines") && shot["lines"].is_array())
      for (const json &line : shot["lines"]) {
        const int who = line.is_object() ? line.value("subject", 0) : 0;
        const std::string words = line.is_object() ? trimmed(text_of(line, "text")) : std::string();
        if (who < 1 || size_t(who) > count) {
          problem = "A line in shot " + std::to_string(shot_number) + " is spoken by subject " + std::to_string(who) +
                    ", but there are " + std::to_string(count) + " subjects.";
          return {};
        }
        if (words.empty()) {
          problem = "A line in shot " + std::to_string(shot_number) + " has no text.";
          return {};
        }
        std::string language = trimmed(text_of(line, "language"));
        if (language.empty())
          language = "English";
        text += " <Subject " + std::to_string(who) + "> says: <d>[" + language + "] " + words + "</d>";
        if (const std::string tone = trimmed(text_of(line, "tone")); !tone.empty())
          text += " their tone is " + tone + ".";
      }
    out += text + "\n";
  }

  const std::string soundscape = trimmed(text_of(spec, "soundscape")), music = trimmed(text_of(spec, "music"));
  out += "\noverall_soundscape:\n" + (soundscape.empty() ? std::string("N/A") : soundscape) + "\n";
  out += "\nnon_diegetic_music:\n" + (music.empty() ? std::string("N/A") : music) + "\n";
  return out;
}

} // namespace atm::gen
