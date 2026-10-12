#pragma once
// The Cast of a project: the people, places and things that have to look and sound the same in every clip that shows them.
// One entry is kept once, in the project, and read by ID; a clip that reads it follows every change to it.
//
// In the project:
//   "cast": {"chr_…": {"kind": "character" | "location" | "prop",
//                      "name": "Mio",
//                      "description": how it looks, as words for the picture prompt,
//                      "personality"?: how it speaks and behaves (for whoever writes its lines; not for the picture),
//                      "references"?: [{"path": "<picture file>", "label"?: "face"}, …]: pictures the model must reproduce,
//                      "voice"?: {"path": "<sound file>"}: a sample whose timbre the speaker takes,
//                      "attributes"?: {"<name>": text, number or yes/no, …}: anything else worth keeping ("alive": false)}}
//   A path is absolute, or relative to the project's folder (the files the engine keeps are in its "cast" folder).
//
// A Character node (attome.character) is the Input node that reads one entry:
//   {"kind": "attome.character", "character": "chr_…", "settings"?: {"attribute": "<name>"}}
//   outputs  description (text), personality (text), references (the pictures, a list), voice (a sound),
//            attributes (all of them as one text: "alive: no, age: 34"), attribute (the one settings.attribute names, as text)
//   It runs no step and has no key of its own: what it gives is part of the key of the node it feeds, so changing an
//   entry moves the keys of exactly the clips that read it.

#include <string>
#include <string_view>
#include <vector>

#include "atm/gen/graph.hpp"

namespace atm::gen {

// What one generate video step of MiniMax H3 takes at most (its reference-to-video node): voice samples and reference pictures.
constexpr int kMaxVoices = 3;
constexpr int kMaxReferencePictures = 9;

// The entry with this ID, or else with this name (letter case does not matter); null when there is none.
const json *find_cast(const json &cast, std::string_view id_or_name);
// Its ID; empty when there is none.
std::string cast_id(const json &cast, std::string_view id_or_name);

// The picture files of an entry, in the order they are written.
std::vector<std::string> cast_references(const json &entry);
// Its voice sample; empty when it has none.
std::string cast_voice(const json &entry);
// Its attributes as one text, in the order of their names: "age: 34, alive: no". Empty when it has none.
std::string cast_attributes_text(const json &entry);

// What a Character node gives on one of its ports, for this entry; null when the entry has nothing for it.
// `attribute` is the node's settings.attribute, for the port "attribute".
json cast_port_value(const json &entry, std::string_view port, std::string_view attribute);

// What a text means by {token} when it names the cast: "{Sam}" is the entry's name and its description ("Sam, a tall man
// in a grey hoodie"), "{Sam.description}", "{Sam.personality}" and "{Sam.attributes}" one part of it, "{Sam.injured}" the
// attribute of that name. False when the token names nothing of the cast (or the part is empty).
bool cast_text(const json &cast, std::string_view token, std::string &out);

// Rule G_CAST for each entry that is not written as above: no name, an unknown kind, a reference without a path,
// an attribute that is a list or an object, two entries with one name.
void check_cast(const json &cast, std::vector<Problem> &out);

// Makes a workflow use an entry: adds a Character node for it and links its pictures to the "references" of the generate
// video node, and, with `voice`, its voice to that node's "voice" (one speaker's voice at a time: the Exposed Input "voice"
// is then left unlinked). The Exposed Input "references" is left unlinked too: an input is either exposed or linked, so a
// picture of something else in the shot (a product) is a cast entry of its own (a prop), used the same way.
// Node IDs are "$new:" placeholders when the workflow's own are, else fresh ones. False when the workflow has no generate
// video node that takes references.
bool use_character(json &workflow, std::string_view character_id, bool voice);

// The reference pictures a generate video node gets when these entries are used, in the order the node receives them
// (the same order its cache key holds them in): what <Image 1>, <Image 2> in a prompt count through.
std::vector<std::string> reference_order(const std::vector<std::string> &pictures);

} // namespace atm::gen
