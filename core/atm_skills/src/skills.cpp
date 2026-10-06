#include "atm/skills/skills.hpp"

#include <algorithm>

namespace atm::skills {
namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
    s.remove_suffix(1);
  return s;
}

// "---\nname: x\ndescription: y\n---\nbody": the description and the body. A UTF-8 byte order mark is skipped.
void split_front_matter(std::string_view text, std::string &description, std::string &body) {
  if (text.starts_with("\xEF\xBB\xBF"))
    text.remove_prefix(3);
  if (!text.starts_with("---")) {
    body = std::string(text);
    return;
  }
  const size_t end = text.find("\n---", 3);
  if (end == std::string_view::npos) {
    body = std::string(text);
    return;
  }
  const std::string_view head = text.substr(3, end - 3);
  size_t at = 0;
  while (at < head.size()) {
    size_t eol = head.find('\n', at);
    if (eol == std::string_view::npos)
      eol = head.size();
    const std::string_view line = trim(head.substr(at, eol - at));
    if (line.starts_with("description:"))
      description = std::string(trim(line.substr(12)));
    at = eol + 1;
  }
  size_t after = text.find('\n', end + 1);
  body = after == std::string_view::npos ? std::string() : std::string(trim(text.substr(after + 1)));
}

std::vector<Skill> load() {
  std::vector<Skill> list;
  for (const File &file : files()) {
    const size_t slash = file.path.find('/');
    if (slash == std::string_view::npos)
      continue;
    const std::string id(file.path.substr(0, slash));
    const std::string_view rest = file.path.substr(slash + 1);
    auto skill = std::find_if(list.begin(), list.end(), [&](const Skill &s) { return s.id == id; });
    if (skill == list.end()) {
      list.push_back({});
      list.back().id = id;
      skill = list.end() - 1;
    }
    if (rest == "SKILL.md")
      split_front_matter(file.data, skill->description, skill->body);
    else
      skill->files.emplace_back(rest);
  }
  std::erase_if(list, [](const Skill &s) { return s.body.empty(); });
  std::sort(list.begin(), list.end(), [](const Skill &a, const Skill &b) { return a.id < b.id; });
  for (Skill &s : list)
    std::sort(s.files.begin(), s.files.end());
  return list;
}

} // namespace

const std::vector<Skill> &builtin() {
  static const std::vector<Skill> list = load();
  return list;
}

const Skill *find_builtin(std::string_view id) {
  for (const Skill &s : builtin())
    if (s.id == id)
      return &s;
  return nullptr;
}

const std::string_view *builtin_file(std::string_view id, std::string_view path) {
  static std::vector<std::pair<std::string, std::string_view>> index;
  if (index.empty())
    for (const File &f : files())
      index.emplace_back(std::string(f.path), f.data);
  const std::string wanted = std::string(id) + "/" + std::string(path);
  for (const auto &entry : index)
    if (entry.first == wanted)
      return &entry.second;
  return nullptr;
}

} // namespace atm::skills
