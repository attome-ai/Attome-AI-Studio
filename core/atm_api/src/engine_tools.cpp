#include "engine_impl.hpp"

namespace atm::api {
// Parameter schemas are JSON Schema (the MCP inputSchema); "project" is the .attome folder or a prj_ ID.

const Engine::Impl::Tool Engine::Impl::kTools[] = {
    {"project.create", "core", true, "Create a new .attome project folder with one empty Sequence.",
     R"({"type":"object","properties":{
       "path":{"type":"string","description":"Folder to create; \".attome\" is added when missing"},
       "name":{"type":"string"},
       "rate":{"type":"string","description":"Frame rate, e.g. \"30\", \"25\" or \"30000/1001\". Default 30"},
       "canvas":{"type":"object","properties":{"width":{"type":"integer"},"height":{"type":"integer"}},
                 "description":"Default 1920 x 1080"}},
       "required":["path"]})",
     &Impl::project_create},
    {"project.inspect", "core", false,
     "Summary of a project: sequences, tracks and (level \"tracks\") every clip with its ID, start and duration.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "level":{"type":"string","enum":["summary","tracks"],"description":"\"tracks\" also lists the clips"},
       "max_items":{"type":"integer","description":"Clips listed per track, default 200"}},
       "required":["project"]})",
     &Impl::project_inspect},
    {"project.get", "core", false, "One object (sequence, track, clip …) by Stable ID, with all its fields.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"id":{"type":"string"}},"required":["project","id"]})",
     &Impl::project_get},
    {"project.patch", "core", true,
     "Low-level edit with an ID-addressed Patch; prefer timeline.edit, and use this for what it does not cover. "
     "All ops apply or none do. guide.get shows the "
     "exact shapes of tracks, clips, text, dissolves, keyframes, effects and sound (topics: clips, text, dissolves, "
     "keyframes, effects, audio, times).\n"
     "Ops: add, remove, replace, move, insert_order, remove_order, test. A path is \"<StableID>/<field>[/…]\": add "
     "{\"op\":\"add\",\"path\":\"<track_id>/clips/$new:c1\",\"value\":{…}}, change {\"op\":\"replace\",\"path\":"
     "\"<clip_id>/timing/duration\",\"value\":\"3s\"}, delete {\"op\":\"remove\",\"path\":\"<clip_id>\"}. "
     "$new:<name> placeholders become Stable IDs (returned in id_map) and later ops of the same patch may use them. "
     "A refused patch says which rule failed and how to fix it. dry_run checks without changing anything.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "patch":{"type":"object","properties":{
         "ops":{"type":"array","items":{"type":"object","properties":{
           "op":{"type":"string","enum":["add","remove","replace","move","insert_order","remove_order","test"]},
           "path":{"type":"string"},"value":{},"to":{"type":"string"}},"required":["op","path"]}},
         "label":{"type":"string","description":"Short description shown in the history"},
         "base_revision":{"type":"integer","description":"Refuse the patch when the project moved past this revision"}},
         "required":["ops"]},
       "dry_run":{"type":"boolean"},
       "task_id":{"type":"string","description":"Groups several edits into one task"}},
       "required":["project","patch"]})",
     &Impl::project_patch},
    {"timeline.edit", "core", true,
     "Edit the timeline with high-level ops, all applied together as one undoable step: add_track, add_clip, add_text, "
     "add_adjustment, add_transition, delete, ripple_delete, move, trim, split, slip, roll, slide, add_effect, "
     "remove_effect, set_effect_enabled, link, unlink, set_property. A video with sound becomes linked picture and "
     "sound clips that edits keep together. "
     "Clip defaults are "
     "worked out for you (append to the track, the rest of the file, a Titles track for text, an Effects track under "
     "it for blur). Give an op \"id\": \"$new:name\" and later ops can use that name. guide.get topic \"timeline\" "
     "has every op's fields and a complete example. Times accept \"2.5s\", \"75@30\" or timecode.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "ops":{"type":"array","items":{"type":"object","properties":{
         "op":{"type":"string","enum":["add_track","add_clip","add_text","add_captions","sync_captions","add_adjustment","add_transition","delete",
                                       "ripple_delete","move","trim","split","duplicate","slip","roll","slide","set_speed","freeze_frame","set_reverse","detach_audio","fade","set_keyframe","remove_keyframe","fit_clip","delete_track","add_marker","remove_marker","add_effect","remove_effect",
                                       "set_effect_enabled","link","unlink","set_property"]},
         "id":{"type":"string","description":"$new:name for what this op creates"}},"required":["op"]}},
       "sequence":{"type":"string"},"label":{"type":"string"},"dry_run":{"type":"boolean"},
       "task_id":{"type":"string","description":"Groups several calls into one task"}},
       "required":["project","ops"]})",
     &Impl::timeline_edit},
    {"media.import", "core", true,
     "Add media files to the project as assets (probed for size, length and sound). Returns their asset IDs for "
     "timeline.edit add_clip. Importing a path again returns the same asset.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "paths":{"type":"array","items":{"type":"string"},"description":"Absolute paths of video, image or sound files"},
       "task_id":{"type":"string"}},
       "required":["project","paths"]})",
     &Impl::media_import},
    {"fonts.list", "core", false,
     "The font families a text clip can use (content.font, or font on add_text): the ones installed on this computer, or the bundled ones. A name not in "
     "the list falls back to the default font.",
     R"({"type":"object","properties":{}})", &Impl::fonts_list},
    {"library.add", "library", false,
     "Keep clips in the user's clip library, which every project can use: the clips (and their linked sound or picture) become one item, "
     "with copies of their files. Clips linked to the ones named come along unless linked is false (only the clips named: the music without its picture). "
     "part sound or picture keeps only that part of a video with its sound inside it. Returns the item's id.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clips":{"type":"array","items":{"type":"string"}},"name":{"type":"string"},"linked":{"type":"boolean","description":"Default true: the linked picture or sound comes too"},
       "part":{"type":"string","enum":["both","sound","picture"],"description":"For a video with its sound inside it: keep only its sound, or only its picture"}},"required":["project","clips"]})",
     &Impl::library_add},
    {"library.list", "library", false, "The items of the user's clip library, the newest first.", R"({"type":"object","properties":{}})", &Impl::library_list},
    {"library.get", "library", false,
     "One library item with its clips: each {clip, audio, offset}. To put it in a project, pass them as snapshots to timeline.edit's duplicate "
     "op ({\"snapshot\": clip, \"track\", \"at\": start + offset}).",
     R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})", &Impl::library_get},
    {"library.insert", "library", true,
     "Put a library item into a project: its clips go in with their files, the first at `at` (default the end of the film), the others where the "
     "item had them, each on a track that is free there (a new one when none is). One edit: Undo takes the item out again.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "id":{"type":"string","description":"The library item, as library.list names it"},"at":{"type":"string","description":"A time like 2.5s, 75@30 or timecode"},
       "track":{"type":"string","description":"A track to try first for the clips of its kind"},"sequence":{"type":"string"}},"required":["project","id"]})",
     &Impl::library_insert},
    {"library.rename", "library", false, "Give a library item another name.",
     R"({"type":"object","properties":{"id":{"type":"string"},"name":{"type":"string"}},"required":["id","name"]})", &Impl::library_rename},
    {"library.remove", "library", false, "Take an item out of the clip library (its copies of the files go with it; projects that used it keep their clips).",
     R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})", &Impl::library_remove},
    {"skill.list", "skill", false,
     "START HERE for a kind of video you have not made before (a Short, ...): the skills available, built in and the project's own, each "
     "with what it is for. Read one with skill.get; it says which Tools to call in which order and what to ask the user.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID: adds the project's own skills"}}})",
     &Impl::skill_list},
    {"skill.get", "skill", false,
     "Read a skill: its instructions (body) and the files that come with it. With file: the text of one file (a recipe script).",
     R"({"type":"object","properties":{"id":{"type":"string"},"file":{"type":"string","description":"A path from the skill's files, e.g. scripts/sync_captions_to_voice.ps1"},
       "project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID: its own skills are found first"}},"required":["id"]})",
     &Impl::skill_get},
    {"skill.save", "skill", true,
     "Keep a skill in the project (name it with title, say when it is for in description, write the steps in body, markdown). With id of one the project "
     "has: change the fields given. One undoable edit. A project skill with the id of a built-in skill replaces it for this project.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "id":{"type":"string"},"title":{"type":"string"},"description":{"type":"string"},"body":{"type":"string"}},"required":["project"]})",
     &Impl::skill_save},
    {"skill.delete", "skill", true, "Remove a skill the project keeps (built-in skills stay).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"id":{"type":"string"}},"required":["project","id"]})",
     &Impl::skill_delete},
    {"guide.get", "core", false,
     "How to write the project: the shapes of tracks and clips, text, dissolves, keyframe animation, effects, sound "
     "and times, with examples ready to adapt. Read it before the first project.patch.",
     R"({"type":"object","properties":{"topic":{"type":"string","enum":["timeline","clips","text","dissolves","keyframes","effects","audio","times"],
       "description":"Leave out to get every topic"}}})",
     &Impl::guide_get},
    {"project.undo", "core", true, "Undo the last edit (steps: N).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"steps":{"type":"integer"}},"required":["project"]})",
     &Impl::project_undo},
    {"project.redo", "project", true, "Redo along the most recent branch of the undo tree.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"steps":{"type":"integer"}},"required":["project"]})",
     &Impl::project_redo},
    {"project.validate", "core", false, "Check the whole project against the semantic rules.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"}},"required":["project"]})", &Impl::project_validate},
    {"project.save", "project", true, "Write project.json now.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"}},"required":["project"]})", &Impl::project_save},
    {"project.close", "project", true, "Save and close a project.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"}},"required":["project"]})", &Impl::project_close},
    {"history.list", "project", false, "Edits (ChangeSets) of the current epoch and the HEAD.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"limit":{"type":"integer"}},"required":["project"]})",
     &Impl::history_list},
    {"time.parse", "project", false, "Convert any accepted time spelling to its canonical forms.",
     R"({"type":"object","properties":{"value":{"type":["string","object"],"description":"\"12.5s\", \"375@30\" (frames@rate), SMPTE \"00:00:12:15\" or {num,den} seconds"},"rate":{"type":"string"}},"required":["value"]})",
     &Impl::time_parse},
    {"edl.export", "edl", true,
     "Write a track's clips as a CMX 3600 cut list, which other editors read: one cut event for each clip made from a file, with its reel (from the "
     "file's name), source range, record range and clip name; a clip with a speed gets an M2 line. Timecode is non-drop-frame, or drop-frame for 29.97 and "
     "59.94; the film starts at `start` (01:00:00:00). tracks (names or IDs) default to the lowest picture track with clips; an audio track is written as AA "
     "events. Text, adjustment layers and generated clips are left out (named in left_out), and transitions are not written.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "output":{"type":"string","description":"The .edl file to write"},"tracks":{"type":"array","items":{"type":"string"}},"title":{"type":"string"},
       "start":{"type":"string","description":"Timecode of the first frame of the film, default 01:00:00:00"},"drop":{"type":"boolean"},"sequence":{"type":"string"}},
       "required":["project","output"]})",
     &Impl::edl_export},
    {"edl.import", "edl", true,
     "Read a CMX 3600 cut list into the project: each event becomes a clip of its file at its record time, with its source range and speed, all in one edit "
     "(Undo takes them out). The file of an event is found by its clip name (the `* FROM CLIP NAME:` line) or reel in `media` {name: file}, in the "
     "project's imported media, or in media_dir. If any file is missing nothing is changed and the answer names them. A list that starts at start "
     "(01:00:00:00) starts the film at zero. Sound-only events for a file that has a picture are left out and counted (its sound comes with it).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "path":{"type":"string","description":"The .edl file"},"media":{"type":"object","additionalProperties":{"type":"string"}},"media_dir":{"type":"string"},
       "start":{"type":"string"},"source_start":{"type":"string","description":"Timecode of the first frame of the files, default 00:00:00:00"},
       "drop":{"type":"boolean"},"fps":{"type":"integer"},"sequence":{"type":"string"}},"required":["project","path"]})",
     &Impl::edl_import},
    {"subtitles.export", "subtitles", true,
     "Write the text clips of a track as a subtitle file (SRT or WebVTT, by the extension or format): one cue for each clip, from its start to its end. "
     "Without track it takes the one named Subtitles, else Captions. offset (seconds) shifts every cue.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "output":{"type":"string","description":"The .srt or .vtt file to write"},"format":{"type":"string","enum":["srt","vtt"]},
       "track":{"type":"string","description":"A track's name or ID"},"offset":{"type":"number"},"sequence":{"type":"string"}},"required":["project","output"]})",
     &Impl::subtitles_export},
    {"subtitles.import", "subtitles", true,
     "Read an SRT or WebVTT file into the project: each cue becomes a text clip (white with a dark outline, at the bottom) on a track named "
     "Subtitles (or `track`), in one edit that Undo takes out. A cue that runs into the next is cut short; the answer counts them. offset (seconds) "
     "shifts every cue; placement, size and color change the look (timeline.edit's add_text values).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "path":{"type":"string","description":"The .srt or .vtt file"},"format":{"type":"string","enum":["srt","vtt"]},"track":{"type":"string"},
       "offset":{"type":"number"},"placement":{"type":"string","enum":["center","lower_third","top","bottom"]},"size":{"type":"number"},
       "color":{"type":"string"},"sequence":{"type":"string"}},"required":["project","path"]})",
     &Impl::subtitles_import},
    {"script.plan", "script", false,
     "Check and time the script of a Short before anything is made; no project is touched. Give scenes: [{say (the narration), label?, seconds?, "
     "start?, prompt? (the picture prompt, with {character} and {style})}], key_words?, character?, style?, target_words? [lo, hi]. Returns each scene "
     "with its start, end, word count, narration pace and the time the voice starts (voice_at, for voice.make), the cuts, the Variables the prompts "
     "need, and warnings (a line too fast or too slow for its scene, no closing . ! ?, a key word that is never said, a length outside target_seconds).",
     R"({"type":"object","properties":{"scenes":{"type":"array","items":{"type":"object","properties":{"say":{"type":"string"},"label":{"type":"string"},
       "seconds":{"type":"number"},"start":{"type":"number"},"prompt":{"type":"string"}},"required":["say"]}},
       "key_words":{"type":"array","items":{"type":"string"}},"character":{"type":"string"},"style":{"type":"string"},
       "target_words":{"type":"array","items":{"type":"integer"}},"target_seconds":{"type":"array","items":{"type":"number"}},"hook_max_seconds":{"type":"number"},"words_per_second":{"type":"number"},"voice_lead":{"type":"number"},"tail":{"type":"number"}},
       "required":["scenes"]})",
     &Impl::script_plan},
    {"script.scenes", "script", true,
     "Make the picture clips of a planned script: the Variables its prompts use ({character}, {style}: those the plan has a value for) become project Variables, then one "
     "generative clip for each scene with a prompt, from the scene's start and as long as the scene, named by its label, made with `model` (gen.models) or a project's "
     "`workflow`. transitions {types, seconds} adds a transition on each cut, the types in turn. Nothing is generated yet: run gen.run.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "plan":{"type":"object","description":"The answer of script.plan"},"model":{"type":"string"},"workflow":{"type":"string"},
       "transitions":{"type":"object","properties":{"types":{"type":"array","items":{"type":"string"}},"seconds":{"type":"number"}}},"sequence":{"type":"string"}},
       "required":["project","plan"]})",
     &Impl::script_scenes},
    {"script.apply", "script", true,
     "Put the on-screen text of a planned script on the timeline in one edit (Undo takes it out): a caption clip for each scene (one word at a time, key words in "
     "colour; timed from the scene's voice_clip when it has one), a label at each scene's start, an opening title (hook) and a closing line (cta). Pass plan "
     "(the answer of script.plan) and look: data for how it is drawn, all optional (captions {style, size, y, color, emphasis_color}, labels {size, y, color, "
     "seconds, extra}, hook {text, seconds, size, y, color, extra}, cta {text, at, seconds, size, y, color, extra}; extra takes what add_text takes: font, shadow, "
     "outline, background). dry_run returns the ops without changing the project.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "plan":{"type":"object","description":"The answer of script.plan"},"look":{"type":"object"},"dry_run":{"type":"boolean"},"sequence":{"type":"string"}},
       "required":["project","plan"]})",
     &Impl::script_apply},
    {"voice.make", "voice", true,
     "Make the voice clips of a script: one clip for each line {text, at}, starting at `at` (seconds, or a time like 3.1s) on the first audio track "
     "that is free there, made with `model` (a speech model from gen.models) and, when given, `voice` and `speed`. Nothing is spoken yet: "
     "run gen.run, then call voice.fit.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "model":{"type":"string"},"lines":{"type":"array","items":{"type":"object","properties":{"text":{"type":"string"},
       "at":{"description":"Seconds, or a time like 3.1s","type":["number","string"]},"voice":{"type":"string"}},"required":["text","at"]}},
       "voice":{"type":"string"},"speed":{"type":"number"},"sequence":{"type":"string"}},"required":["project","model","lines"]})",
     &Impl::voice_make},
    {"voice.fit", "voice", true,
     "After the voices are spoken: fit each to the time it has (up to the next voice, less `gap`) by its speed, push voices that overlap the one "
     "before, and set each one's gain so its loudest sample reaches target_peak. A voice whose speed changed has to be spoken again: the answer "
     "lists it in `rerun`; run gen.run and call this again. Safe to call any number of times: it changes nothing once the voices fit.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clips":{"type":"array","items":{"type":"string"},"description":"Default: every voice clip"},
       "speed_min":{"type":"number","description":"Default 1.0"},"speed_max":{"type":"number","description":"Default 1.4"},
       "gap":{"type":"number","description":"Seconds between voices, default 0.1"},"last_budget":{"type":"number","description":"Seconds for the last voice, default 3.75"},
       "budgets":{"type":"array","items":{"type":"number"},"description":"Seconds each voice may take, in order; default: up to the next voice"},
       "target_peak":{"type":"number","description":"0..1, default 0.9"}},"required":["project"]})",
     &Impl::voice_fit},
    {"asr.transcribe", "core", false,
     "Hear what a clip says, word by word, as a background job (jobs.get follows it, jobs.cancel stops it). Give project and clip (the part of the "
     "file it plays) or a path (a sound or video file, optionally from and duration in seconds). The result is {words: [{text, start, end}], "
     "language, model, cached}, times in seconds from the clip's start (or `from`). Runs on this computer with a speech model that models.fetch "
     "downloads once (whisper.small, or the more exact whisper.large-v3-turbo-q5: a third of the mistakes in Arabic); the better one installed is used. "
     "language auto finds the language, or give a code (en, ar). With project the transcript is kept in the project, in the file's own time: asked "
     "again for a clip of that file (trimmed, split, at another speed) it is answered at once, cached true; again true listens anew. For captions: "
     "add_captions {clip, words: <result.words>} (guide.get topic \"timeline\").",
     R"({"type":"object","properties":{"project":{"type":"string","description":"With clip: path of the .attome project folder, or its prj_ ID"},
       "clip":{"type":"string","description":"A clip made from a sound or video file"},
       "path":{"type":"string","description":"A sound or video file, instead of a clip"},
       "from":{"type":"number","description":"With path: seconds into the file, default 0"},
       "duration":{"type":"number","description":"With path: seconds to listen to, default to the end"},
       "language":{"type":"string","description":"auto (default), or a code such as en, ar, fr"},
       "again":{"type":"boolean","description":"Listen anew even when the project already has what the file says"},
       "model":{"type":"string","enum":["best","small","turbo"],"description":"best (default): the more exact one that is installed; small or turbo to choose"},
       "device":{"type":"string","enum":["auto","gpu","cpu"],"description":"auto (default): the graphics card, unless a generation is using it; the result says which ran"}}})",
     &Impl::asr_transcribe},
    {"audio.analyze", "core", false,
     "Where the beat is and how loud a sound is: bpm, the beat grid (first_beat and beats, in seconds of the file), confidence (3 and more is "
     "clear; has_beat says whether there is one), peak and RMS in dB, and the integrated loudness in LUFS (-23 for EBU R128 broadcast, about -14 for streaming). Pass path (the first two minutes, or from..to in seconds) or project "
     "and clip (the part the clip plays). Use it to cut to the beat, to set a clip's speed to a song, and to check a mix's levels.",
     R"({"type":"object","properties":{"path":{"type":"string","description":"A sound or video file"},
       "project":{"type":"string","description":"With clip: path of the .attome project folder, or its prj_ ID"},"clip":{"type":"string"},
       "from":{"type":"number","description":"Seconds"},"to":{"type":"number","description":"Seconds"},
       "min_bpm":{"type":"number","description":"Default 80"},"max_bpm":{"type":"number","description":"Default 180"}}})",
     &Impl::audio_analyze},
    {"video.extract_audio", "core", true,
     "The sound of a video (or sound) file, as its own WAV: a niche often fits one piece of music or one voice, not just its pace, so this "
     "keeps the actual sound to reuse. With project and no output it is imported as the project's own asset (asset_id, ready for "
     "timeline.edit add_clip); with output it is written there instead. At most 10 minutes; from..to (seconds) take a part of it.",
     R"({"type":"object","properties":{"path":{"type":"string","description":"A video or sound file"},
       "project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID; imports the sound as an asset"},
       "output":{"type":"string","description":"A .wav path to write instead of importing"},
       "from":{"type":"number","description":"Seconds"},"to":{"type":"number","description":"Seconds"}},"required":["path"]})",
     &Impl::video_extract_audio},
    {"ytdlp.status", "core", false,
     "Whether the optional video downloader (yt-dlp, open source, Unlicense) is on this computer: found, path, tools_dir, and what ytdlp.install would fetch. "
     "It is needed only to save a video from a link (video.fetch).",
     R"({"type":"object","properties":{}})", &Impl::ytdlp_status},
    {"ytdlp.install", "core", false,
     "Fetch the video downloader (yt-dlp, about 18 MB) once from its own GitHub release into Attome's tools folder, checked against the release's checksum list. "
     "Ask the user first: it is optional, and what a link may be used for is theirs to check. A job (jobs.get).",
     R"({"type":"object","properties":{}})", &Impl::ytdlp_install},
    {"video.fetch", "core", false,
     "Save the video of a web link (at most 720 p, one video, no playlist) to a folder as a job; the result has path and title. Needs ytdlp.install first. "
     "Meant for studying a style (video.analyze, niche-from-video) of a video the user may use; it is not for copying or re-uploading someone else's work.",
     R"({"type":"object","properties":{"url":{"type":"string","description":"https:// link of one video"},"folder":{"type":"string","description":"Where to save it; default Attome's downloads folder"},"cookies_file":{"type":"string","description":"A cookies.txt exported from the browser, when its own sign-in file cannot be read. Ask the user first."},"cookies_from_browser":{"type":"string","enum":["chrome","edge","firefox","brave","opera","vivaldi"],"description":"For a site that asks for a sign-in (YouTube: not a bot): use the sign-in of this browser. Ask the user first."}},"required":["url"]})",
     &Impl::video_fetch},
    {"video.analyze", "core", false,
     "The numbers of a finished video's style: shot cuts (cut_times, in seconds of the file) and their rhythm (cuts per second, average, shortest and longest shot), "
     "brightness and contrast (0 to 1), the five main colours with their share, and its sound (loudness LUFS, peak, bpm and what share of the cuts fall on the beat). "
     "Reads at most 180 s (from..to in seconds). The words come from asr.transcribe; the look and the tone are read from see.contact_sheet. The niche-from-video skill "
     "turns all of it into a draft niche.",
     R"({"type":"object","properties":{"path":{"type":"string","description":"A video file"},
       "from":{"type":"number","description":"Seconds"},"to":{"type":"number","description":"Seconds"}},"required":["path"]})",
     &Impl::video_analyze},
    {"music.fit", "core", true,
     "Fit a music clip to the film: find its tempo and beat (audio.analyze), set its speed so the tempo becomes bpm (within half and double), cut its start to the "
     "first beat at or after `from` (seconds of the file), put that beat at `at` (seconds of the film; default where the clip is now), and with `until` end the clip "
     "there. One edit; Undo takes it back. Fails with E_NO_BEAT when the music has no clear beat.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"clip":{"type":"string"},
       "bpm":{"type":"number"},"at":{"type":"number"},"until":{"type":"number"},"from":{"type":"number"}},"required":["project","clip"]})",
     &Impl::music_fit},
    {"clip.motion", "core", true,
     "Put the same movement on clips: `keys` is [{property (scale | position | rotation | opacity), at (seconds from the clip's start) | at_end (seconds before its end), value "
     "(a number, or [x, y] for scale and position), relative (the value changes the clip's own: a factor for scale and opacity, an offset for position and rotation), ease "
     "(ease_out_expo, ease_out_back, ease_in_out_quad ...), hold}]. A punch-in at a cut is scale 1.2 relative at 0 with ease_out_expo, then 1.0 at 0.33; a shake is a run "
     "of small position offsets. A key where the clip has one takes the new value. One edit; Undo takes it back. Keep the keys in a look file and pass them again to repeat it.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"clips":{"type":"array","items":{"type":"string"}},
       "keys":{"type":"array","items":{"type":"object","properties":{"property":{"type":"string","enum":["scale","position","rotation","opacity"]},"at":{"type":"number"},"at_end":{"type":"number"},
       "value":{},"relative":{"type":"boolean"},"ease":{"type":"string"},"hold":{"type":"boolean"}},"required":["property","value"]}}},"required":["project","clips","keys"]})",
     &Impl::clip_motion},
    {"text.pop", "core", true,
     "Sound words that burst out at a moment (POOF!, BOING!): `words` is [{at (seconds), say, color?, y?}]. Each is a bold text clip of `seconds` (default 0.87) with a dark "
     "shadow under it, on the tracks Pop words and Pop shadow, and bounces in from small, wobbles, settles and fades out. `keys` replaces that pop with your own clip.motion "
     "keys; `shadow` {color, offset} changes the shadow. Two edits (the words, then their motion); Undo twice takes them back.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "words":{"type":"array","items":{"type":"object","properties":{"at":{"type":"number"},"say":{"type":"string"},"color":{"type":"string"},"y":{"type":"number"}},"required":["at","say"]}},
       "seconds":{"type":"number"},"size":{"type":"number"},"keys":{"type":"array"},"shadow":{"type":"object"}},"required":["project","words"]})",
     &Impl::text_pop},
    {"render.devices", "core", false,
     "Where rendering runs: every GPU here (name, driver, discrete or integrated, memory, whether it can render and why not), the choice (auto, cpu, or a "
     "GPU's name) and in_use: the device that renders now and why. Automatic takes the discrete GPU; an integrated one only when a timing check shows it "
     "beats the CPU; and the CPU while a generation is running (the GPU is left to it). check: true measures every GPU again (a 1080p blur on the GPU "
     "and on the CPU). Today the GPU makes the effects (blur, sharpen, grade, vignette, grain, LUT) of clips and adjustment layers; keys and the "
     "drawing of the layers are still made on the CPU. jobs.get of an export says rendered_on.",
     R"({"type":"object","properties":{"check":{"type":"boolean"},"refresh":{"type":"boolean","description":"false: the GPUs listed before, without asking Vulkan again"}}})", &Impl::render_devices},
    {"render.set_device", "core", true,
     "Choose where rendering runs: \"auto\" (the default), \"cpu\", or a GPU by its name or index from render.devices. Remembered in the user's settings. "
     "Returns what render.devices returns.",
     R"({"type":"object","properties":{"device":{"type":["string","integer"]}},"required":["device"]})", &Impl::render_set_device},
    {"media.codecs", "core", false,
     "The formats this machine can write: mp4 (H.264, the operating system's encoder), wav, jpeg, png_sequence, and prores and dnxhr, which are written by an FFmpeg program of your own "
     "(Attome ships none: it looks on the PATH, in ATTOME_FFMPEG and at the saved ffmpeg_path). With ffmpeg_path that program is remembered (\"\" forgets it). The answer says which "
     "FFmpeg was found, its version and its license (lgpl, gpl or nonfree: only the build's own; Attome uses its prores_ks and dnxhd encoders and no GPL library) and the formats it allows. "
     "Then render.sequence format prores (profile proxy | lt | standard | hq | 4444 | 4444xq) or dnxhr (profile lb | sq | hq | hqx | 444).",
     R"({"type":"object","properties":{"ffmpeg_path":{"type":"string"}}})", &Impl::media_codecs},
    {"flash.cuts", "core", true,
     "Flash on every cut: an adjustment layer (default 0.23 s) at the start of each clip of `track` but the first (or at each clip of `clips`), whose brightness starts at "
     "`brightness` (default 0.65) and falls to nothing, shaped by `ease`. Adjustment layers on the Effects track: remove one with delete. One edit; Undo takes them all back.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"track":{"type":"string"},
       "clips":{"type":"array","items":{"type":"string"}},"seconds":{"type":"number"},"brightness":{"type":"number"},"saturation":{"type":"number"},"ease":{"type":"string"}},"required":["project"]})",
     &Impl::flash_cuts},
    {"sequence.create", "core", true,
     "Add another sequence (an empty timeline) to the project: a cut for another platform or ratio, a reel of a film. `canvas` {width, height} and `rate` default to the first "
     "sequence's. Returns its ID: pass it as `sequence` to the Tools that edit or read a sequence (a Tool given a clip works in that clip's sequence by itself). One edit; Undo takes it back.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"name":{"type":"string"},
       "canvas":{"type":"object","properties":{"width":{"type":"integer"},"height":{"type":"integer"}}},"rate":{"type":"string"}},"required":["project"]})",
     &Impl::sequence_create},
    {"music.cuts", "core", true,
     "Cut a clip on the beats of a music clip: every `every`-th beat (default 1) that falls inside the clip (and inside from..until, seconds of the film). Beats are found with "
     "audio.analyze and placed where the music plays (its speed and start), so it works after music.fit. Linked sound is cut with the picture. One edit; Undo takes it back. "
     "Fails with E_NO_BEAT when the music has no clear beat or none falls inside the clip.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"music":{"type":"string"},"clip":{"type":"string"},
       "every":{"type":"integer"},"from":{"type":"number"},"until":{"type":"number"}},"required":["project","music","clip"]})",
     &Impl::music_cuts},
    {"audio.duck", "core", true,
     "Lower a music clip's level under other clips (the voice) and bring it back after them: `over` lists clip IDs, or a track ID for all its clips; `db` is how far "
     "it goes down (default 12), `ramp` the seconds it takes each way (default 0.12). Writes audio.keyframes.duck_db on the music clip (replacing its ducking keys), added "
     "to its level, which stays its own (gain_db, or its gain_db keys); timeline.edit remove_keyframe {property duck_db} takes the ducking away. One edit; Undo takes it back.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"clip":{"type":"string"},
       "over":{"type":"array","items":{"type":"string"}},"db":{"type":"number"},"ramp":{"type":"number"}},"required":["project","clip","over"]})",
     &Impl::audio_duck},
    {"sfx.make", "core", true,
     "Make a sound effect: whoosh (builds and lands on a cut), click, pop, riser (builds up to its end; seconds sets the length), impact. "
     "Writes a 48 kHz stereo WAV: pass output (a file), or project to put it in the project and import it (asset_id comes back for "
     "timeline.edit add_clip). seed makes the noise differ.",
     R"({"type":"object","properties":{"kind":{"type":"string","enum":["whoosh","click","pop","riser","impact"]},"output":{"type":"string"},
       "project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},"seed":{"type":"integer"},"seconds":{"type":"number"}},
       "required":["kind"]})",
     &Impl::sfx_make},
    {"media.remove", "core", true,
     "Take a file out of the project: every clip made from it (and the clips linked to them) is deleted, then the asset itself. "
     "Pass asset (its ast_ ID) or the file's path. A clip on a locked track stops it. Undo takes two steps: the clips, then the asset.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "asset":{"type":"string"},"path":{"type":"string"}},"required":["project"]})",
     &Impl::media_remove},
    {"media.probe", "core", false,
     "Size, frame rate, duration and audio format of a media file. Still pictures (PNG, JPEG, BMP, GIF, TGA) report "
     "image: true and their size, and have no duration.",
     R"({"type":"object","properties":{"path":{"type":"string","description":"Absolute path of the file"}},
       "required":["path"]})",
     &Impl::media_probe},
    {"see.frames", "core", false,
     "Render frames of the sequence as JPEG pictures, to check an edit by eye. Times past the end show the last "
     "frame. The files are replaced by the next see.* call.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "times":{"type":"array","items":{"type":["string","object"],"description":"\"12.5s\", \"375@30\" (frames@rate), SMPTE \"00:00:12:15\" or {num,den} seconds"},"minItems":1,"maxItems":16},
       "height":{"type":"integer","description":"Picture height in pixels, default 540"},
       "sequence":{"type":"string","description":"Sequence ID, default the first"}},
       "required":["project","times"]})",
     &Impl::see_frames},
    {"see.contact_sheet", "core", false,
     "One JPEG with evenly spaced frames of the whole sequence in a grid, each labelled with its timecode. The "
     "quickest way to review a cut before rendering it.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "count":{"type":"integer","description":"Number of frames, 1 to 48, default 12"},
       "columns":{"type":"integer","description":"Default 4"},
       "tile_height":{"type":"integer","description":"Pixels per frame, default 180"},
       "sequence":{"type":"string"}},
       "required":["project"]})",
     &Impl::see_contact_sheet},
    {"render.sequence", "core", false,
     "Export a sequence to an H.264 + AAC .mp4 as a background job (with format prores or dnxhr a .mov made by your own FFmpeg, see media.codecs, `profile` picks the flavour: ProRes proxy | lt | standard | hq | 4444 | 4444xq, DNxHR lb | sq | hq | hqx | 444; with format wav its sound only, with jpeg one frame as a picture, with png_sequence a numbered PNG for each frame in the folder `output`: lossless 8-bit pictures for a colour or effects pipeline, to be paired with a wav of the sound); from and to export only a part; rate makes it at another frame rate (24, 25, 30, 50, 60, \"30000/1001\": the sources play at their own times). Returns job_id at once; follow it with jobs.get. `not_made` lists the generative clips of the part that are not made yet (nothing of their own is drawn) or out of date (their last Take is drawn): gen.run makes them.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "output":{"type":"string","description":"Absolute path of the .mp4 to write; for png_sequence the folder"},
       "height":{"type":"integer","description":"Output height, default the canvas height"},
       "bitrate":{"type":"integer"},"audio":{"type":"boolean"},
       "profile":{"type":"string","description":"For prores and dnxhr: the profile, default hq"},
       "format":{"type":"string","enum":["mp4","prores","dnxhr","wav","jpeg","png_sequence"],"description":"mp4 video (the default), prores or dnxhr a .mov through your own FFmpeg (media.codecs says if there is one), wav the sound only, jpeg one frame: the one at from, png_sequence a PNG for each frame in the folder output name_000000.png, numbered by frame"},
       "from":{"type":"string","description":"Where the export starts, such as 2.5s or 00:00:02:15; default the start"},
       "to":{"type":"string","description":"Where the export ends, the frame there not included; default the end"},
       "overwrite":{"type":"boolean","description":"Default true"},"sequence":{"type":"string"}},
       "required":["project","output"]})",
     &Impl::render_sequence},
    {"gen.status", "gen", false,
     "The generative clips of a project, in the order they would run: each one's state (clean, dirty with the reason, empty, locked), "
     "its Takes, and whether it can run on this computer: a clip is not ready while a node of its "
     "workflow has no model chosen (G_MODEL_UNSET), a model this version does not know (G_MODEL_UNKNOWN) or one that is not "
     "installed (G_MODEL_MISSING, with the model's title and the bytes still to download: start it with models.fetch).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"}},"required":["project"]})", &Impl::gen_status},
    {"gen.run", "gen", true,
     "Generate clips as a background job (units are steps; follow it with jobs.get, stop it with jobs.cancel). scope: \"dirty\" "
     "(default: every clip whose Take is not what its inputs ask for, or that has none), \"all\", \"selected\" (clips: [...], plus what "
     "they need upstream that is itself dirty) or \"selected_and_after\" (and every clip that starts from them). Clips run in "
     "dependency order; a step whose result is in the cache is not run again; a locked clip never runs. new_take: true makes "
     "another Take of the named clips with the next seed. dry_run: true returns the plan only: for every clip its state (clean, "
     "dirty, empty, locked), the reason, whether it runs, and the number of steps. A finished clip gets a Take and selects it.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "scope":{"type":"string","enum":["dirty","all","selected","selected_and_after"]},
       "clips":{"type":"array","items":{"type":"string"}},"new_take":{"type":"boolean"},"dry_run":{"type":"boolean"}},
       "required":["project"]})",
     &Impl::gen_run},
    {"gen.engines", "gen", false,
     "The engines that can run models here (the user's ComfyUI when its address is set), each with whether it answers, its version and device.",
     "", &Impl::gen_engines},
    {"gen.set_comfyui", "gen", false,
     "Set the address of the user's own ComfyUI (for example http://127.0.0.1:8188), used as an engine; \"\" stops using it. "
     "The address is remembered. Returns whether ComfyUI answers there, its version and device.",
     R"({"type":"object","properties":{"address":{"type":"string"}},"required":["address"]})", &Impl::gen_set_comfyui},
    {"gen.models", "gen", false,
     "The models a generative clip can be made with: each with its title, whether its files are installed, whether an engine here "
     "runs it, the optional inputs it accepts and the clip lengths it makes.",
     "", &Impl::gen_models},
    {"gen.nodes", "gen", false,
     "What a Clip Workflow is built from: the node kinds with their input and output ports (name, type, required), and every "
     "model with the kinds it runs, its settings (type, range, options, default), the optional inputs it accepts, whether "
     "its files are installed and whether an engine here runs each kind.",
     "", &Impl::gen_nodes},
    {"gen.create_clip", "gen", true,
     "Add a generative clip: a prompt and a model (or a workflow of the project's library). It goes at the end of the picture track (or at: a time), gets its own copy "
     "of the built-in Shot workflow for that model (its Instance: change it and no other clip changes), and is not generated yet: run gen.run. start_from: \"previous\" (or a clip ID) makes "
     "it start on the last frame of that clip, with a Clip Reference and a Get Frame node. The size is the canvas's shape at about 0.9 megapixels on the model's grid (a Project node), the length is the clip's Duration (a Clip node).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "prompt":{"type":"string"},"model":{"type":"string","description":"An id from gen.models"},
       "workflow":{"type":"string","description":"Instead of a model: the ID of a Clip Workflow of the project's library, a key of its workflows"},"seconds":{"type":"number"},
       "seed":{"type":"integer"},"name":{"type":"string"},
       "start_from":{"type":"string","description":"\"previous\" or a generative clip's ID"},"at":{"type":"string"},
       "track":{"type":"string"},"sequence":{"type":"string"}},"required":["project","prompt"]})",
     &Impl::gen_create_clip},
    {"gen.save_to_library", "gen", true,
     "Publish a generative clip's own workflow to the project's library as a new Clip Workflow (a card in the Generate panel). "
     "The clip keeps its own; its input values are not part of the copy. name: the library's name for it (default: the workflow's).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clip":{"type":"string"},"name":{"type":"string"}},"required":["project","clip"]})",
     &Impl::gen_save_to_library},
    {"gen.reset_clip", "gen", true,
     "Put a generative clip's workflow back to the Clip Workflow it was copied from (its source): the built-in Shot, or the library "
     "workflow as it is now. One undoable edit; the clip's values for inputs the original does not have are dropped.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clip":{"type":"string"}},"required":["project","clip"]})",
     &Impl::gen_reset_clip},
    {"gen.save_preset", "gen", true,
     "Keep a generative clip's input values under a name as a Preset of the Clip Workflow it was made from (a Preset of the same name "
     "and source is replaced). The project's \"presets\" hold them.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clip":{"type":"string"},"name":{"type":"string"}},"required":["project","clip","name"]})",
     &Impl::gen_save_preset},
    {"gen.apply_preset", "gen", true,
     "Put a Preset's values on a generative clip, for the inputs its workflow has, in one undoable edit. Returns how many were set and "
     "which were skipped (inputs the clip's workflow lacks).",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clip":{"type":"string"},"preset":{"type":"string","description":"A pre_ ID"}},"required":["project","clip","preset"]})",
     &Impl::gen_apply_preset},
    {"gen.node_results", "gen", false,
     "What each node of a generative clip's workflow made last, for the nodes whose result is in the cache and is what the clip's inputs "
     "ask for now: {node ID: {key, files: {output port: file}}}. An editor shows the picture of a node from it.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clip":{"type":"string"}},"required":["project","clip"]})",
     &Impl::gen_node_results},
    {"gen.export_workflow", "gen", false,
     "Write a Clip Workflow to a file: a workflow of the project's library (workflow: cwf_ ID) or a clip's own (clip). The file is "
     "Attome's own format; gen.import_workflow reads it back, in this project or another, and the workflow runs the same.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "workflow":{"type":"string"},"clip":{"type":"string"},"path":{"type":"string"}},"required":["project","path"]})",
     &Impl::gen_export_workflow},
    {"gen.import_workflow", "gen", true,
     "Read a Clip Workflow from a file into the project's library: Attome's own export, or a ComfyUI workflow saved as API format. From "
     "ComfyUI, the nodes Attome has (CLIPTextEncode, KSampler, SamplerCustomAdvanced, VAEDecode) come with their links, text and seed; "
     "every other node is named in `unmatched`. name: what to call it.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "path":{"type":"string"},"name":{"type":"string"}},"required":["project","path"]})",
     &Impl::gen_import_workflow},
    {"gen.select_take", "gen", true,
     "Choose which Take of a generative clip plays. The clip's inputs go back to what made that Take; clips that start from it become dirty.",
     R"({"type":"object","properties":{"project":{"type":"string","description":"Path of the .attome project folder, or its prj_ ID"},
       "clip":{"type":"string"},"take":{"type":"string"}},"required":["project","clip","take"]})",
     &Impl::gen_select_take},
    {"models.list", "models", false,
     "The models this version can download, with their size, licence and what is already on disk (installed, partial, downloading, missing); "
     "the folder downloads go to with its free space, and the other folders that are looked in.",
     "", &Impl::models_list},
    {"models.fetch", "models", false,
     "Download a model from the catalog as a background job (units are bytes; follow it with jobs.get, stop it with jobs.cancel). "
     "A stopped or interrupted download continues from where it was; every file is checked against its SHA-256 before it is put in place.",
     R"({"type":"object","properties":{"id":{"type":"string","description":"An id from models.list"}},"required":["id"]})", &Impl::models_fetch},
    {"models.locate", "models", false,
     "Use model files that are already on this computer instead of downloading them: give the folder they are in (a ComfyUI "
     "folder, its models folder, or a folder of the files themselves). Returns how many of the catalog's files were found there "
     "(of one model with id) and, with id, the bytes still to download. A folder that holds some is remembered and looked in from then on.",
     R"({"type":"object","properties":{"folder":{"type":"string"},"id":{"type":"string","description":"An id from models.list"}},"required":["folder"]})",
     &Impl::models_locate},
    {"models.forget_folder", "models", false, "Stop looking for model files in a folder given to models.locate. Nothing is deleted.",
     R"({"type":"object","properties":{"folder":{"type":"string"}},"required":["folder"]})", &Impl::models_forget_folder},
    {"models.set_folder", "models", false,
     "Choose the folder downloads go to (for example on a drive with more room); \"\" goes back to the default one. Models in the "
     "folder used before stay usable. The choice is remembered.",
     R"({"type":"object","properties":{"folder":{"type":"string"}},"required":["folder"]})", &Impl::models_set_folder},
    {"jobs.get", "core", false, "State and progress of a job.",
     R"({"type":"object","properties":{"job_id":{"type":"string"}},"required":["job_id"]})", &Impl::jobs_get},
    {"jobs.cancel", "core", false, "Stop a running job.",
     R"({"type":"object","properties":{"job_id":{"type":"string"}},"required":["job_id"]})", &Impl::jobs_cancel},
    {"tools.list", "core", false, "The Tools this engine offers, with their parameter schemas.", "",
     &Impl::tools_list},
    {"daemon.hello", "daemon", false, "Protocol handshake.", "", &Impl::daemon_hello},
    {"daemon.status", "daemon", false, "Open projects, uptime and settings.", "", &Impl::daemon_status},
    {"daemon.shutdown", "daemon", true, "Save everything and stop the daemon.", "", &Impl::daemon_shutdown},
    {"profile.get", "daemon", false, "Zone timings of every engine thread. reset: true zeroes them afterwards.",
     R"({"type":"object","properties":{"reset":{"type":"boolean"}}})", &Impl::profile_get},
    {"profile.reset", "daemon", false, "Zero the profiler statistics.", "", &Impl::profile_reset},
    {"profile.set", "daemon", false, "Switch the profiler on or off at run time.",
     R"({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":["enabled"]})", &Impl::profile_set},
};


std::span<const Engine::Impl::Tool> Engine::Impl::tools() { return kTools; }

} // namespace atm::api
