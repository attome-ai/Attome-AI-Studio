// The ComfyUI node Attome brings for Kokoro-82M speech (custom_nodes/attome_kokoro/__init__.py), written out by comfyui.install.
// It is Attome's own code. The weights are looked for in Attome's models folder first, then in ComfyUI's own.
R"py("""Kokoro-82M text to speech for Attome: English voices, one node, the model kept in memory.

Weights: <models>/Kokorotts/Kokoro-82M/{config.json, kokoro-v1_0.pth, voices/*.pt}, where <models> is Attome's models folder
(ATTOME_MODELS_DIR, set by Attome when it starts ComfyUI) or ComfyUI's own models folder.
"""
import os

import numpy as np
import torch
import folder_paths


def _find():
    places = [os.environ.get("ATTOME_MODELS_DIR", ""), folder_paths.models_dir]
    for base in places:
        d = os.path.join(base, "Kokorotts", "Kokoro-82M") if base else ""
        if d and os.path.isfile(os.path.join(d, "kokoro-v1_0.pth")):
            return d
    return os.path.join(folder_paths.models_dir, "Kokorotts", "Kokoro-82M")


_DIR = _find()
_VOICES = sorted(f[:-3] for f in os.listdir(os.path.join(_DIR, "voices")) if f.endswith(".pt")) if os.path.isdir(os.path.join(_DIR, "voices")) else []
_model = None
_pipes = {}


def _load():
    global _model
    if _model is None:
        from kokoro import KModel
        dev = "cuda" if torch.cuda.is_available() else "cpu"
        _model = KModel(config=os.path.join(_DIR, "config.json"), model=os.path.join(_DIR, "kokoro-v1_0.pth")).to(dev).eval()
    return _model


class AttomeKokoroTTS:
    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {
            "text": ("STRING", {"multiline": True, "default": ""}),
            "voice": (_VOICES or ["am_michael"],),
            "speed": ("FLOAT", {"default": 1.0, "min": 0.5, "max": 2.0, "step": 0.05}),
        }}

    RETURN_TYPES = ("AUDIO",)
    FUNCTION = "speak"
    OUTPUT_NODE = True  # so that the word times come back in ComfyUI's history, next to the saved sound
    CATEGORY = "Attome"

    def speak(self, text, voice, speed):
        from kokoro import KPipeline
        model = _load()
        lang = voice[0]  # a: American, b: British
        if lang not in _pipes:
            _pipes[lang] = KPipeline(lang_code=lang, model=model)
        tensor = torch.load(os.path.join(_DIR, "voices", voice + ".pt"), weights_only=True)
        words = []   # [{"text", "start", "end"}] in seconds from the start of the sound
        audio_parts = []
        offset = 0.0
        cur = None
        for r in _pipes[lang](text, voice=tensor, speed=speed, split_pattern=r"\n+"):
            for t in (r.tokens or []):
                if cur is None:
                    cur = {"text": "", "start": None, "end": None}
                cur["text"] += t.text
                if t.start_ts is not None and cur["start"] is None:
                    cur["start"] = offset + float(t.start_ts)
                if t.end_ts is not None:
                    cur["end"] = offset + float(t.end_ts)
                if t.whitespace:
                    words.append(cur)
                    cur = None
            if r.audio is not None:
                audio_parts.append(r.audio)
                offset += len(r.audio) / 24000.0
        if cur is not None:
            words.append(cur)
        if not audio_parts:
            raise ValueError("Kokoro made no speech for this text.")
        audio = np.concatenate([p.detach().cpu().numpy() if hasattr(p, "detach") else np.asarray(p) for p in audio_parts])
        wave = torch.from_numpy(audio.astype(np.float32)).unsqueeze(0).unsqueeze(0)  # (batch, channels, samples)
        import json
        return {"ui": {"text": [json.dumps([w for w in words if w["start"] is not None and w["end"] is not None])]},
                "result": ({"waveform": wave, "sample_rate": 24000},)}


NODE_CLASS_MAPPINGS = {"AttomeKokoroTTS": AttomeKokoroTTS}
NODE_DISPLAY_NAME_MAPPINGS = {"AttomeKokoroTTS": "Kokoro TTS (Attome)"}
)py"
