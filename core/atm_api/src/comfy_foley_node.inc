// The ComfyUI node Attome brings for HunyuanVideo-Foley (custom_nodes/attome_foley/__init__.py), written out by comfyui.install.
// It is Attome's own code around Tencent's package hunyuanvideo_foley. Every file is read from Attome's models folder: nothing is fetched
// while it runs (the package would ask the Hugging Face hub for its two encoders by name; they are given from the folder instead).
R"py("""HunyuanVideo-Foley for Attome: the sound of what happens in a video, from the video and a few words.

Weights: <models>/foley/hunyuanvideo-foley/{hunyuanvideo_foley.pth or hunyuanvideo_foley_xl.pth, synchformer_state_dict.pth,
vae_128d_48k.pth, config.yaml or config_xl.yaml}, <models>/foley/siglip2-base-patch16-512/ and <models>/foley/larger_clap_general/,
where <models> is Attome's models folder (ATTOME_MODELS_DIR, set by Attome when it starts ComfyUI) or ComfyUI's own.
"""
import os

import torch
import folder_paths

_IDS = {"google/siglip2-base-patch16-512": "siglip2-base-patch16-512", "laion/larger_clap_general": "larger_clap_general"}


def _root():
    # Attome's models folder, ComfyUI's own, then the folders ComfyUI is set to read models from (extra_model_paths.yaml): the one that has it.
    bases = [os.environ.get("ATTOME_MODELS_DIR", ""), folder_paths.models_dir]
    for paths, _ in getattr(folder_paths, "folder_names_and_paths", {}).values():
        bases += [os.path.dirname(os.path.normpath(p)) for p in paths]
    for base in bases:
        d = os.path.join(base, "foley") if base else ""
        if d and os.path.isdir(os.path.join(d, "hunyuanvideo-foley")):
            return d
    return os.path.join(folder_paths.models_dir, "foley")


_loaded = {}


def _transformers_compat():
    # Tencent's code was written for transformers 4.49; transformers 5 dropped a pruning helper it imports (pruning is never used here).
    import transformers.pytorch_utils as pu
    if hasattr(pu, "find_pruneable_heads_and_indices"):
        return

    def find_pruneable_heads_and_indices(heads, n_heads, head_size, already_pruned_heads):
        mask = torch.ones(n_heads, head_size)
        heads = set(heads) - already_pruned_heads
        for head in heads:
            head = head - sum(1 if h < head else 0 for h in already_pruned_heads)
            mask[head] = 0
        mask = mask.view(-1).contiguous().eq(1)
        return heads, torch.arange(len(mask))[mask].long()

    pu.find_pruneable_heads_and_indices = find_pruneable_heads_and_indices


def _features_compat(model_dict):
    # In transformers 5 get_image_features returns an output object; the package wants the pooled tensor itself.
    m = model_dict.siglip2_model
    if getattr(m, "_attome_features", False):
        return
    original = m.get_image_features

    def get_image_features(*a, **k):
        out = original(*a, **k)
        return out if torch.is_tensor(out) else getattr(out, "pooler_output", out[0])

    m.get_image_features = get_image_features
    m._attome_features = True


def _local_encoders(root):
    # The package names its two encoders by their Hugging Face ids: they come from the folder, and the hub is never asked.
    os.environ["HF_HUB_OFFLINE"] = "1"
    import transformers
    for cls in (transformers.AutoModel, transformers.AutoTokenizer, transformers.ClapTextModelWithProjection):
        if getattr(cls.from_pretrained, "_attome", False):
            continue
        original = cls.from_pretrained.__func__

        def local(c, name, *a, _original=original, **k):
            if isinstance(name, str) and name in _IDS:
                name = os.path.join(root, _IDS[name])
            return _original(c, name, *a, **k)

        local._attome = True
        cls.from_pretrained = classmethod(local)


def _load(size, offload):
    key = (size, offload)
    if key not in _loaded:
        _loaded.clear()
        torch.cuda.empty_cache()
        root = _root()
        _local_encoders(root)
        _transformers_compat()
        from hunyuanvideo_foley.utils.model_utils import load_model
        weights = os.path.join(root, "hunyuanvideo-foley")
        config = os.path.join(weights, "config.yaml" if size == "xxl" else "config_xl.yaml")
        device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        _loaded[key] = load_model(weights, config, device, enable_offload=offload, model_size=size)
    return _loaded[key]


class AttomeFoley:
    @classmethod
    def INPUT_TYPES(cls):
        return {"required": {
            "video": ("STRING", {"default": ""}),
            "prompt": ("STRING", {"multiline": True, "default": ""}),
            "negative": ("STRING", {"default": "speech, talking, voices, words, mumbling, babble, shouting, singing, vocals, music, noisy, harsh, muffled"}),
            "seed": ("INT", {"default": 1, "min": 0, "max": 2 ** 31 - 1}),
            "steps": ("INT", {"default": 50, "min": 10, "max": 100}),
            "guidance": ("FLOAT", {"default": 4.5, "min": 1.0, "max": 10.0, "step": 0.1}),
            "size": (["xxl", "xl"],),
            "offload": ("BOOLEAN", {"default": False}),
        }}

    RETURN_TYPES = ("AUDIO",)
    FUNCTION = "make"
    CATEGORY = "Attome"

    def make(self, video, prompt, negative, seed, steps, guidance, size, offload):
        if not os.path.isfile(video):
            raise ValueError("HunyuanVideo-Foley: there is no video at " + video)
        _transformers_compat()
        from hunyuanvideo_foley.utils.feature_utils import feature_process
        from hunyuanvideo_foley.utils.model_utils import denoise_process
        model_dict, cfg = _load(size, offload)
        _features_compat(model_dict)
        torch.manual_seed(seed)
        visual, text, seconds = feature_process(video, prompt, model_dict, cfg, neg_prompt=negative or None)
        audio, rate = denoise_process(visual, text, seconds, model_dict, cfg, guidance_scale=guidance, num_inference_steps=steps)
        wave = audio[0].float().cpu()
        if wave.dim() == 1:
            wave = wave.unsqueeze(0)
        return ({"waveform": wave.unsqueeze(0), "sample_rate": int(rate)},)


NODE_CLASS_MAPPINGS = {"AttomeFoley": AttomeFoley}
NODE_DISPLAY_NAME_MAPPINGS = {"AttomeFoley": "HunyuanVideo-Foley (Attome)"}
)py"
