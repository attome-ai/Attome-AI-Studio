// Skills: how-to guides for agents, kept as Attome items. Built-in ones ship inside Attome (core/atm_skills/skills/<id>/SKILL.md and the
// files next to it); a project keeps its own in "skills" (skl_ IDs). An agent finds them with the Tools skill.list / skill.get.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atm::skills {

struct File {
  std::string_view path; // "short-video/SKILL.md", "short-video-voice/scripts/sync_captions_to_voice.ps1"
  std::string_view data;
};

// Every file built in (generated at configure time from core/atm_skills/skills).
std::span<const File> files();

struct Skill {
  std::string id;          // the folder name
  std::string description; // the "description:" line of the front matter
  std::string body;        // SKILL.md without its front matter
  std::vector<std::string> files; // paths below the skill's folder, "scripts/x.ps1"
};

// The built-in skills, by id.
const std::vector<Skill> &builtin();
const Skill *find_builtin(std::string_view id);
// One file of a built-in skill ("scripts/x.ps1"), or nullptr.
const std::string_view *builtin_file(std::string_view id, std::string_view path);

} // namespace atm::skills
