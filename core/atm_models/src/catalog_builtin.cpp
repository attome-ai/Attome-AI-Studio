#include "atm/models/models.hpp"

// The catalog compiled into this version. Every file is pinned to one revision of its publisher's repository, with the
// size and SHA-256 that revision reports, so what arrives is what was listed here or it is refused.

namespace atm::models {
namespace {

// Comfy-Org/MiniMax-H3 at revision e5eb578a89295337b8ff433a035929ce0279e0b6 (read 2026-10-04).
#define ATM_H3 "https://huggingface.co/Comfy-Org/MiniMax-H3/resolve/e5eb578a89295337b8ff433a035929ce0279e0b6/"
// FastVideo/FastVideo-FastH3-Comfy at revision ec1e3aa374a91c57b0b94a1623b7e657c0498cf2 (read 2026-10-04).
#define ATM_FASTH3 "https://huggingface.co/FastVideo/FastVideo-FastH3-Comfy/resolve/ec1e3aa374a91c57b0b94a1623b7e657c0498cf2/"
// WarmBloodAban/Minimax-h3_Singularity at revision af671d9214a6e41ab8c2f43e9f871ea56246115f (read 2026-10-11; size and hash are what the
// repository's headers report, and the hash matched the file as downloaded).
#define ATM_H3SINGULARITY "https://huggingface.co/WarmBloodAban/Minimax-h3_Singularity/resolve/af671d9214a6e41ab8c2f43e9f871ea56246115f/"
// Kijai/MiniMax-H3-experimental at revision d8023be02fefbb3633b0cd335c3879f91177299d (read 2026-10-11; the publisher states no licence).
#define ATM_H3KIJAI "https://huggingface.co/Kijai/MiniMax-H3-experimental/resolve/d8023be02fefbb3633b0cd335c3879f91177299d/"
// LBH-123-AI/Minimax_h3_latent_Upscaler at revision 3f941d5d182014dd5c0a5e16330420ee2d4aa0c6 (read 2026-10-11).
#define ATM_H3UPSCALER "https://huggingface.co/LBH-123-AI/Minimax_h3_latent_Upscaler/resolve/3f941d5d182014dd5c0a5e16330420ee2d4aa0c6/"
// fal/MiniMax-H3-Realism-People-LoRA at revision 039cc8579d7aa357a882d7f4111b25da4f72dccc (read 2026-10-14; MiniMax H3 Community License).
#define ATM_H3REALISM "https://huggingface.co/fal/MiniMax-H3-Realism-People-LoRA/resolve/039cc8579d7aa357a882d7f4111b25da4f72dccc/"
// larryvrh/MiniMax-H3-Turbo-Lora at revision 43a74557ac3f6539db8e0f2a959d03feb7a81480 (read 2026-10-14; Apache-2.0).
#define ATM_H3TURBOV4 "https://huggingface.co/larryvrh/MiniMax-H3-Turbo-Lora/resolve/43a74557ac3f6539db8e0f2a959d03feb7a81480/"
// ggerganov/whisper.cpp at revision 5359861c739e955e79d9a303bcbc70fb988958b1 (read 2026-10-09: the revision, size and hash are what the
// repository's own headers report for ggml-small.bin, and the hash matches the file as downloaded).
#define ATM_WHISPER "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/"
// Comfy-Org/ace_step_1.5_ComfyUI_files at revision 6707deb277e9e0907fd9c14ce6b6f1d695c6a3fc (read 2026-10-13: sizes and hashes are what the
// repository reports; the sizes match the files as downloaded).
// hexgrad/Kokoro-82M at revision f3ff3571791e39611d31c381e3a41a3af07b4987 (read 2026-10-13: sizes and hashes are what the repository reports, and
// every file listed here matched the copy on disk).
#define ATM_KOKORO "https://huggingface.co/hexgrad/Kokoro-82M/resolve/f3ff3571791e39611d31c381e3a41a3af07b4987/"
#define ATM_HYFOLEY "https://huggingface.co/tencent/HunyuanVideo-Foley/resolve/3abd4e833b95b8db0fc9c687afc52483a48e9a97/"
#define ATM_SIGLIP2 "https://huggingface.co/google/siglip2-base-patch16-512/resolve/a89f5c5093f902bf39d3cd4d81d2c09867f0724b/"
#define ATM_LCLAP "https://huggingface.co/laion/larger_clap_general/resolve/ada0c23a36c4e8582805bb38fec3905903f18b41/"
#define ATM_ACESTEP "https://huggingface.co/Comfy-Org/ace_step_1.5_ComfyUI_files/resolve/6707deb277e9e0907fd9c14ce6b6f1d695c6a3fc/split_files/"
// Comfy-Org/Krea-2 at revision 1171f4dc7e325bf3734586942e737872506c73e1 (read 2026-10-16: sizes and hashes are what the repository reports).
#define ATM_KREA2 "https://huggingface.co/Comfy-Org/Krea-2/resolve/1171f4dc7e325bf3734586942e737872506c73e1/"
// RudySen/Krea2-realism-V2 at revision ad6f07e426303e7087d215362dbc057b0073dca3 (read 2026-10-16; MIT).
#define ATM_KREA2REALISM "https://huggingface.co/RudySen/Krea2-realism-V2/resolve/ad6f07e426303e7087d215362dbc057b0073dca3/"
// Comfy-Org/z_image_turbo at revision 6fc90a3b1b653e935a0d175e260736de25b84df5 (read 2026-10-16; Apache-2.0).
#define ATM_ZIMAGE "https://huggingface.co/Comfy-Org/z_image_turbo/resolve/6fc90a3b1b653e935a0d175e260736de25b84df5/split_files/"
// suayptalha/Z-Image-Turbo-Realism-LoRA at revision 8dc179cb56844d4ddc6e4527c30e98347eeae971 (read 2026-10-16; Apache-2.0).
#define ATM_ZIMAGEREALISM "https://huggingface.co/suayptalha/Z-Image-Turbo-Realism-LoRA/resolve/8dc179cb56844d4ddc6e4527c30e98347eeae971/"

constexpr const char *kCatalog = R"json({
  "version": 1,
  "entries": [
    {
      "id": "minimax-h3.fl2va.turbo8-int8",
      "title": "MiniMax H3: text and image to video, Turbo 8-step (int8)",
      "kind": "model",
      "licence": "MiniMax H3 Community License: the weights are free to download; commercial use of what you generate needs a paid licence.",
      "licence_url": "https://huggingface.co/MiniMaxAI/MiniMax-H3",
      "notes": "33 B video and sound model, pruned int8 build, with the official 8-step Turbo add-on. Needs a 24 GB GPU or larger.",
      "declares": {
        "kinds": ["generate_video", "encode_prompt", "sample", "decode"],
        "accepts": ["start_image", "end_image", "voice", "continue_from"],
        "seconds": {"min": 1, "max": 15},
        "frames": {"rate": 24, "step": 17, "offset": 5},
        "sizes": {"multiple": 32, "max_pixels": 2088960},
        "settings": {
          "steps": {"type": "integer", "min": 1, "max": 50, "default": 8},
          "sampler": {"type": "choice", "options": ["res_multistep", "euler"], "default": "res_multistep"},
          "scheduler": {"type": "choice", "options": ["simple", "beta"], "default": "simple"},
          "attention": {"type": "choice", "options": ["int8", "default"], "default": "int8"},
          "stage": {"type": "choice", "options": ["preview", "final"], "default": "preview"},
          "final_megapixels": {"type": "number", "min": 0.5, "max": 4.0, "default": 2.0},
          "sparse_keep_percent": {"type": "integer", "min": 0, "max": 100, "default": 0},
          "speed_lora": {"type": "choice", "options": ["standard", "turbo_v4"], "default": "standard"},
          "realism_lora": {"type": "number", "min": 0.0, "max": 1.5, "default": 0.0},
          "low_memory": {"type": "choice", "options": ["off", "on"], "default": "off"}
        }
      },
      "files": [
        {"path": "vae/minimax_h3_audio_vae_fp32.safetensors", "size": 605254808,
         "sha256": "8e505d95dd1561d47abd43d4238fd40d9bb1ae9e147ed0a4cba778d76ae4db48",
         "url": ")json" ATM_H3 R"json(vae/minimax_h3_audio_vae_fp32.safetensors"},
        {"path": "loras/minimax_h3_fl2v_turbo_8step_v1.0_comfyui_bf16.safetensors", "size": 1956193000,
         "sha256": "2339acdf19bfe123f46b971ea35d367a84adb85de43627e1eceafa5a5b2b111e",
         "url": ")json" ATM_H3 R"json(loras/minimax_h3_fl2v_turbo_8step_v1.0_comfyui_bf16.safetensors"},
        {"path": "vae/minimax_h3_video_vae_int8_convrot.safetensors", "size": 2811065184,
         "sha256": "52a2c8c73583c86e4f41cdcce3a6ad0ea562987bc0bf3d60a0cef5f5c8e60c0e",
         "url": ")json" ATM_H3 R"json(vae/minimax_h3_video_vae_int8_convrot.safetensors"},
        {"path": "vae/minimax_h3_video_vae_fp16.safetensors", "size": 5207808496,
         "sha256": "7c1f131492e7eddacaac9069a61b81bdd39de5cc96561e677c5eab1cdce5e522",
         "url": ")json" ATM_H3 R"json(vae/minimax_h3_video_vae_fp16.safetensors"},
        {"path": "text_encoders/qwen3vl_32b_minimax_h3_nvfp4_awq.safetensors", "size": 15687142551,
         "sha256": "35a88d51044231fe332301d7a62aa81e3f2cba62febeb446e2c1e3e0ef76f2c6",
         "url": ")json" ATM_H3 R"json(text_encoders/qwen3vl_32b_minimax_h3_nvfp4_awq.safetensors"},
        {"path": "diffusion_models/minimax_h3_fl2va_pruned_int8_convrot.safetensors", "size": 20970379616,
         "sha256": "e889202c41dafb67b10d67b97f0d8541508036a6090af23425a5c2615d03c47a",
         "url": ")json" ATM_H3 R"json(diffusion_models/minimax_h3_fl2va_pruned_int8_convrot.safetensors"}
      ]
    },
    {
      "id": "fasth3.8step-v2.int8",
      "title": "FastH3 8-step V2: MiniMax H3 distilled by FastVideo (int8)",
      "kind": "model",
      "licence": "MiniMax H3 Community License: the weights are free to download; commercial use of what you generate needs a paid licence.",
      "licence_url": "https://huggingface.co/FastVideo/FastVideo-FastH3-Comfy",
      "notes": "MiniMax H3 distilled to 8 steps as one model, with no add-on. Shares its text encoder and VAEs with MiniMax H3. Needs a 24 GB GPU or larger.",
      "declares": {
        "kinds": ["generate_video", "encode_prompt", "sample", "decode"],
        "accepts": ["start_image", "end_image", "voice", "continue_from"],
        "seconds": {"min": 1, "max": 15},
        "frames": {"rate": 24, "step": 17, "offset": 5},
        "sizes": {"multiple": 32, "max_pixels": 2088960},
        "settings": {
          "steps": {"type": "integer", "min": 1, "max": 50, "default": 8},
          "sampler": {"type": "choice", "options": ["res_multistep", "euler"], "default": "res_multistep"},
          "scheduler": {"type": "choice", "options": ["simple", "beta"], "default": "simple"},
          "attention": {"type": "choice", "options": ["int8", "default"], "default": "int8"},
          "stage": {"type": "choice", "options": ["preview", "final"], "default": "preview"},
          "final_megapixels": {"type": "number", "min": 0.5, "max": 4.0, "default": 2.0},
          "sparse_keep_percent": {"type": "integer", "min": 0, "max": 100, "default": 0},
          "low_memory": {"type": "choice", "options": ["off", "on"], "default": "off"}
        }
      },
      "files": [
        {"path": "vae/minimax_h3_audio_vae_fp32.safetensors", "size": 605254808,
         "sha256": "8e505d95dd1561d47abd43d4238fd40d9bb1ae9e147ed0a4cba778d76ae4db48",
         "url": ")json" ATM_H3 R"json(vae/minimax_h3_audio_vae_fp32.safetensors"},
        {"path": "vae/minimax_h3_video_vae_int8_convrot.safetensors", "size": 2811065184,
         "sha256": "52a2c8c73583c86e4f41cdcce3a6ad0ea562987bc0bf3d60a0cef5f5c8e60c0e",
         "url": ")json" ATM_H3 R"json(vae/minimax_h3_video_vae_int8_convrot.safetensors"},
        {"path": "text_encoders/qwen3vl_32b_minimax_h3_nvfp4_awq.safetensors", "size": 15687142551,
         "sha256": "35a88d51044231fe332301d7a62aa81e3f2cba62febeb446e2c1e3e0ef76f2c6",
         "url": ")json" ATM_H3 R"json(text_encoders/qwen3vl_32b_minimax_h3_nvfp4_awq.safetensors"},
        {"path": "diffusion_models/fastvideo_fasth3_8step_v2_pruned_int8_convrot.safetensors", "size": 22128378696,
         "sha256": "0922785978dc9bfe1adf27d8b291b0ca763f9f165f882e6cb297c72fbb6deda8",
         "url": ")json" ATM_FASTH3 R"json(diffusion_models/fastvideo_fasth3_8step_v2_pruned_int8_convrot.safetensors"}
      ]
    },
    {
      "id": "minimax-h3.singularity-ref2va.int8",
      "title": "MiniMax H3 Singularity: reference to video, fine-tune (int8) with the DMAD add-on",
      "kind": "model",
      "licence": "Apache-2.0 for the fine-tune as published; it is made from MiniMax H3, whose Community License also applies to what you generate.",
      "licence_url": "https://huggingface.co/WarmBloodAban/Minimax-h3_Singularity",
      "notes": "A community fine-tune of MiniMax H3 for scenes with people and references, with a cleaner look than the base model. Run with Kijai's DMAD add-on at 8 steps. Shares its text encoder and VAEs with MiniMax H3. Needs a 24 GB GPU or larger.",
      "declares": {
        "kinds": ["generate_video", "encode_prompt", "sample", "decode"],
        "accepts": ["start_image", "end_image", "references", "voice", "continue_from"],
        "seconds": {"min": 1, "max": 15},
        "frames": {"rate": 24, "step": 17, "offset": 5},
        "sizes": {"multiple": 32, "max_pixels": 2088960},
        "settings": {
          "steps": {"type": "integer", "min": 1, "max": 50, "default": 8},
          "sampler": {"type": "choice", "options": ["res_multistep", "euler"], "default": "res_multistep"},
          "scheduler": {"type": "choice", "options": ["simple", "beta"], "default": "simple"},
          "attention": {"type": "choice", "options": ["int8", "default"], "default": "int8"},
          "stage": {"type": "choice", "options": ["preview", "final"], "default": "preview"},
          "final_megapixels": {"type": "number", "min": 0.5, "max": 4.0, "default": 2.0},
          "sparse_keep_percent": {"type": "integer", "min": 0, "max": 100, "default": 0},
          "speed_lora": {"type": "choice", "options": ["standard", "turbo_v4"], "default": "standard"},
          "realism_lora": {"type": "number", "min": 0.0, "max": 1.5, "default": 0.0},
          "low_memory": {"type": "choice", "options": ["off", "on"], "default": "off"}
        }
      },
      "files": [
        {"path": "vae/minimax_h3_audio_vae_fp32.safetensors", "size": 605254808,
         "sha256": "8e505d95dd1561d47abd43d4238fd40d9bb1ae9e147ed0a4cba778d76ae4db48",
         "url": ")json" ATM_H3 R"json(vae/minimax_h3_audio_vae_fp32.safetensors"},
        {"path": "vae/minimax_h3_video_vae_int8_convrot.safetensors", "size": 2811065184,
         "sha256": "52a2c8c73583c86e4f41cdcce3a6ad0ea562987bc0bf3d60a0cef5f5c8e60c0e",
         "url": ")json" ATM_H3 R"json(vae/minimax_h3_video_vae_int8_convrot.safetensors"},
        {"path": "text_encoders/qwen3vl_32b_minimax_h3_nvfp4_awq.safetensors", "size": 15687142551,
         "sha256": "35a88d51044231fe332301d7a62aa81e3f2cba62febeb446e2c1e3e0ef76f2c6",
         "url": ")json" ATM_H3 R"json(text_encoders/qwen3vl_32b_minimax_h3_nvfp4_awq.safetensors"},
        {"path": "loras/minimax_h3_DMAD_4step_full_lora_avg_rank_39_bf16.safetensors", "size": 366647000,
         "sha256": "ddfdd94f4c9a0f32bf3649bb8e98ca20db572e4db6bcd9cca546a5b33ea32d11",
         "url": ")json" ATM_H3KIJAI R"json(loras/minimax_h3_DMAD_4step_full_lora_avg_rank_39_bf16.safetensors"},
        {"path": "diffusion_models/Minimax-h3_Singularity_ref2va_Pruned_v1.3_int8.safetensors", "size": 20967647456,
         "sha256": "412a7b126595a958964193f3b42513d7cf2df4196ef04223a0f05e3622949cce",
         "url": ")json" ATM_H3SINGULARITY R"json(Minimax-h3_Singularity_ref2va_Pruned_v1.3_int8.safetensors"}
      ]
    },
    {
      "id": "minimax-h3.latent-upscaler",
      "title": "MiniMax H3 latent upscaler: the final stage's enlarger (fp16)",
      "kind": "model",
      "licence": "Apache-2.0.",
      "licence_url": "https://huggingface.co/LBH-123-AI/Minimax_h3_latent_Upscaler",
      "notes": "A 345 M parameter 3D convolution that enlarges a clip's latent about 2.2 times before the short refine pass of stage \"final\". Put in ComfyUI's models/latent_upscale_models folder; the Comfyui_Minimax_h3_latent_Upscaler node pack must be in custom_nodes.",
      "files": [
        {"path": "latent_upscale_models/minimax_h3_latent_upscaler_3d_conv_v1_fp16.safetensors", "size": 690592672,
         "sha256": "043e5a48e161610ef6c3ea974645220354d06fa618abca15f76d084812eb55c2",
         "url": ")json" ATM_H3UPSCALER R"json(minimax_h3_latent_upscaler_3d_conv_v1/minimax_h3_latent_upscaler_3d_conv_v1_fp16.safetensors"}
      ]
    },
    {
      "id": "minimax-h3.realism-people-lora",
      "title": "MiniMax H3 Realism People: an add-on for live-action people (skin, faces, film light)",
      "kind": "model",
      "licence": "The MiniMax H3 Community License, as the model it adds to.",
      "licence_url": "https://huggingface.co/fal/MiniMax-H3-Realism-People-LoRA",
      "notes": "A LoRA trained by fal on live-action clips of people: skin that keeps its texture, faces that hold up in close-up, film-style light. Used by the H3 setting realism_lora (0 = off; 0.6-1.0); its trigger word is added to the prompt. Only for photoreal shots. Put in the models/loras folder ComfyUI reads.",
      "files": [
        {"path": "loras/h3-realism-people-t2v-i2v-r2v.safetensors", "size": 131229656,
         "sha256": "acc529601d2da117fb81179e76c56e488a3beab1171659d305f04fa3655b787e",
         "url": ")json" ATM_H3REALISM R"json(h3-realism-people-t2v-i2v-r2v.safetensors"}
      ]
    },
    {
      "id": "minimax-h3.turbo-v4-lora",
      "title": "MiniMax H3 Turbo v4 (EMA): a few-step add-on with finer faces and hands",
      "kind": "model",
      "licence": "Apache-2.0 for the add-on; the MiniMax H3 Community License applies to what you generate.",
      "licence_url": "https://huggingface.co/larryvrh/MiniMax-H3-Turbo-Lora",
      "notes": "larryvrh's few-step LoRA, used in place of a model's own add-on by the H3 setting speed_lora: turbo_v4. Its author suggests 6-8 steps, euler, beta. Put in the models/loras folder ComfyUI reads.",
      "files": [
        {"path": "loras/minimax_h3_turbo_v4_step600_ema.safetensors", "size": 779849816,
         "sha256": "5f3a626cd72c93a8b9318d6760c510bc5092d2ab13aaba1f932c5bab07a416d3",
         "url": ")json" ATM_H3TURBOV4 R"json(minimax_h3_turbo_v4_step600_ema.safetensors"}
      ]
    },
    {
      "id": "krea2.turbo-int8",
      "title": "Krea 2 Turbo: text to image, 8 steps (int8)",
      "kind": "model",
      "licence": "Krea 2 Community License: non-commercial use and research; commercial use needs a licence from Krea.",
      "licence_url": "https://huggingface.co/Comfy-Org/Krea-2",
      "notes": "A 12 B still-image model distilled to 8 steps, CFG off, up to 2048 x 2048; long natural-language prompts work best. For cast portraits, locations and first frames. Run by ComfyUI 0.26 or later; the text encoder loads with the CLIP type krea2. The files must be in the models folder ComfyUI reads.",
      "files": [
        {"path": "vae/qwen_image_vae.safetensors", "size": 253806246,
         "sha256": "a70580f0213e67967ee9c95f05bb400e8fb08307e017a924bf3441223e023d1f",
         "url": ")json" ATM_KREA2 R"json(vae/qwen_image_vae.safetensors"},
        {"path": "text_encoders/qwen3vl_4b_fp8_scaled.safetensors", "size": 5242467968,
         "sha256": "54bd5144df0bbc25dd6ccadfcb826b521445a1b06ae5a42570bdd2974ca87094",
         "url": ")json" ATM_KREA2 R"json(text_encoders/qwen3vl_4b_fp8_scaled.safetensors"},
        {"path": "diffusion_models/krea2_turbo_int8_convrot.safetensors", "size": 13492686496,
         "sha256": "8e4eeda70dd5037ab1ba2bef6b417f9f901e26093117cf397f741fc1fdaaf3f1",
         "url": ")json" ATM_KREA2 R"json(diffusion_models/krea2_turbo_int8_convrot.safetensors"}
      ]
    },
    {
      "id": "krea2.style-reference-lora",
      "title": "Krea 2 Style Reference: an add-on that takes the look of 1-2 reference images",
      "kind": "model",
      "licence": "The Krea 2 Community License, as the model it adds to.",
      "licence_url": "https://huggingface.co/ostris/krea2_turbo_style_reference",
      "notes": "Ostris's LoRA for Krea 2 Turbo: colour, light and rendering from one or two reference images, no trigger word; used by gen.image style_images (ComfyUI's own nodes, 0.26 or later). Put in the models/loras folder ComfyUI reads.",
      "files": [
        {"path": "loras/krea2_style_reference.safetensors", "size": 457111760,
         "sha256": "f50df5a9e62e4be8aa926a63dd5bb1a64770c4004f763c1208007ae13daa82b8",
         "url": ")json" ATM_KREA2 R"json(loras/krea2_style_reference.safetensors"}
      ]
    },
    {
      "id": "krea2.realism-v2-lora",
      "title": "Krea 2 Realism V2: an add-on for photoreal skin, light and detail",
      "kind": "model",
      "licence": "MIT for the add-on; the Krea 2 Community License applies to what you generate.",
      "licence_url": "https://huggingface.co/RudySen/Krea2-realism-V2",
      "notes": "RudySen's LoRA, used on Krea 2 Turbo; no trigger word. Its author suggests strength 1.0 with a good prompt and prefers 1.5-2.0, with prompts of four or five sentences. Put in the models/loras folder ComfyUI reads.",
      "files": [
        {"path": "loras/Krea2-realism-V2.safetensors", "size": 1562410344,
         "sha256": "fb0048ce3340adc2842ccfcc1a9a61a9773b6628b01ae5a2e139ca9c679e715d",
         "url": ")json" ATM_KREA2REALISM R"json(Krea2-realism-V2.safetensors"}
      ]
    },
    {
      "id": "z-image.turbo-int8",
      "title": "Z-Image Turbo: text to image, 8 steps (int8)",
      "kind": "model",
      "licence": "Apache 2.0: free to use, also commercially.",
      "licence_url": "https://huggingface.co/Tongyi-MAI/Z-Image-Turbo",
      "notes": "Alibaba's 6 B still-image model distilled to 8-9 steps, CFG off; photoreal, and writes English and Chinese text. Run by ComfyUI; the files must be in the models folder ComfyUI reads.",
      "files": [
        {"path": "vae/ae.safetensors", "size": 335304388,
         "sha256": "afc8e28272cd15db3919bacdb6918ce9c1ed22e96cb12c4d5ed0fba823529e38",
         "url": ")json" ATM_ZIMAGE R"json(vae/ae.safetensors"},
        {"path": "text_encoders/qwen_3_4b_fp8_mixed.safetensors", "size": 5631994051,
         "sha256": "72450b19758172c5a7273cf7de729d1c17e7f434a104a00167624cba94f68f15",
         "url": ")json" ATM_ZIMAGE R"json(text_encoders/qwen_3_4b_fp8_mixed.safetensors"},
        {"path": "diffusion_models/z_image_turbo_int8_convrot.safetensors", "size": 6201001296,
         "sha256": "be517ebd47c912a5626a588e1aeea43e6be4a43c0cdcd2b48a2a780d9f358635",
         "url": ")json" ATM_ZIMAGE R"json(diffusion_models/z_image_turbo_int8_convrot.safetensors"}
      ]
    },
    {
      "id": "z-image.turbo-realism-lora",
      "title": "Z-Image Turbo Realism: an add-on for ultra-realistic scenes",
      "kind": "model",
      "licence": "Apache 2.0, as the model it adds to.",
      "licence_url": "https://huggingface.co/suayptalha/Z-Image-Turbo-Realism-LoRA",
      "notes": "suayptalha's LoRA for Z-Image Turbo, trained on a hand-picked realistic part of CC12M; its trigger word is \"Realism\" at the start of the prompt. Put in the models/loras/z_image_turbo_realism folder ComfyUI reads (its file has a generic name).",
      "files": [
        {"path": "loras/z_image_turbo_realism/pytorch_lora_weights.safetensors", "size": 85094800,
         "sha256": "b38b074964f1f6564c22fee42125accd7a218a916966b5d0f0b2f63d4939c2f7",
         "url": ")json" ATM_ZIMAGEREALISM R"json(pytorch_lora_weights.safetensors"}
      ]
    },
    {
      "id": "ace-step.1.5-xl-turbo",
      "title": "ACE-Step 1.5 XL turbo: music from words",
      "kind": "model",
      "licence": "Apache 2.0: free to use, also commercially.",
      "licence_url": "https://huggingface.co/ACE-Step/Ace-Step1.5",
      "notes": "Makes a piece of music from a description (genre, instruments, mood), a tempo and a key: music.make. Run by ComfyUI, about 10 s for a minute of music on a 24 GB GPU. The files must be in the models folder ComfyUI reads.",
      "files": [
        {"path": "vae/ace_1.5_vae.safetensors", "size": 337431732,
         "sha256": "6de92e3a862acd287e08b024ac90f0783a8635451b728721a33ff03565bcb2bb",
         "url": ")json" ATM_ACESTEP R"json(vae/ace_1.5_vae.safetensors"},
        {"path": "text_encoders/qwen_0.6b_ace15.safetensors", "size": 1191588248,
         "sha256": "fd4590c82153b8ddb67e15a2e7aaa8afa8b83a858c8a9b82a4831063156aa7a7",
         "url": ")json" ATM_ACESTEP R"json(text_encoders/qwen_0.6b_ace15.safetensors"},
        {"path": "text_encoders/qwen_4b_ace15.safetensors", "size": 8379154232,
         "sha256": "ffe5ffb855086c2ab55e467e9859fb01894781020a0376484dd19de166b79873",
         "url": ")json" ATM_ACESTEP R"json(text_encoders/qwen_4b_ace15.safetensors"},
        {"path": "diffusion_models/acestep_v1.5_xl_turbo_bf16.safetensors", "size": 9974719892,
         "sha256": "86a1afb0a1f711f0e3304ff65d874df3ae6783db683dcf982513fb9b6d14ae71",
         "url": ")json" ATM_ACESTEP R"json(diffusion_models/acestep_v1.5_xl_turbo_bf16.safetensors"}
      ]
    },
    {
      "id": "hunyuan-foley.xxl",
      "title": "HunyuanVideo-Foley XXL: sound effects that follow what happens in a video",
      "kind": "model",
      "licence": "Tencent Hunyuan Community License: commercial use allowed, except in the European Union, the United Kingdom and South Korea; the two encoders it reads (SigLIP 2, CLAP) are Apache 2.0.",
      "licence_url": "https://huggingface.co/tencent/HunyuanVideo-Foley",
      "notes": "Watches a shot and makes the sounds of its actions in sync (a smash, footsteps, a crowd), steered by a few words: sfx.from_video. Run by ComfyUI with the attome_foley node (comfyui.install); about 20 GB of graphics memory (12 GB with offload). The files go in the foley folder of the models folder ComfyUI reads.",
      "files": [
        {"path": "foley/hunyuanvideo-foley/config.yaml", "size": 1313,
         "sha256": "557c4cdffebcffe7f33c4ae14be84d854afbeb769dcb3e3c3833787d1a34dc75",
         "url": ")json" ATM_HYFOLEY R"json(config.yaml"},
        {"path": "foley/hunyuanvideo-foley/hunyuanvideo_foley.pth", "size": 10301204679,
         "sha256": "2900021f8ee562a8175b2f1a3fafb06b9e9e848f11f4c6d4d5d72e126f7c0475",
         "url": ")json" ATM_HYFOLEY R"json(hunyuanvideo_foley.pth"},
        {"path": "foley/hunyuanvideo-foley/synchformer_state_dict.pth", "size": 950058171,
         "sha256": "8aff082f2df5c3bc52759db0c865c7ee772ae6400b860d1b7e90413f2defb67c",
         "url": ")json" ATM_HYFOLEY R"json(synchformer_state_dict.pth"},
        {"path": "foley/hunyuanvideo-foley/vae_128d_48k.pth", "size": 1486465965,
         "sha256": "07e6139ff33bd21ba8a7a7f40ed24aab13ac0d100cd686f8a9f3e03dc5251cb1",
         "url": ")json" ATM_HYFOLEY R"json(vae_128d_48k.pth"},
        {"path": "foley/siglip2-base-patch16-512/config.json", "size": 276,
         "sha256": "a15bc39fbcd92498dea6d5d3cb2b0d8b9ba389d6a3f2b565ea66128a2d141934",
         "url": ")json" ATM_SIGLIP2 R"json(config.json"},
        {"path": "foley/siglip2-base-patch16-512/preprocessor_config.json", "size": 394,
         "sha256": "63ff380d3e424f93e6fbca5cc8e74eeed882e96fd6be2b7728aed308f3ad1513",
         "url": ")json" ATM_SIGLIP2 R"json(preprocessor_config.json"},
        {"path": "foley/siglip2-base-patch16-512/special_tokens_map.json", "size": 636,
         "sha256": "baec30ea10906f16adb8c18af7a34023002c1746542612b8b41c9f09e1351351",
         "url": ")json" ATM_SIGLIP2 R"json(special_tokens_map.json"},
        {"path": "foley/siglip2-base-patch16-512/tokenizer_config.json", "size": 47164,
         "sha256": "14afe629fe4959b9e0d51e1852b8d9f7ad074f90a1a7125a4fcdd17f06e78fc8",
         "url": ")json" ATM_SIGLIP2 R"json(tokenizer_config.json"},
        {"path": "foley/siglip2-base-patch16-512/tokenizer.model", "size": 4241003,
         "sha256": "61a7b147390c64585d6c3543dd6fc636906c9af3865a5548f27f31aee1d4c8e2",
         "url": ")json" ATM_SIGLIP2 R"json(tokenizer.model"},
        {"path": "foley/siglip2-base-patch16-512/tokenizer.json", "size": 34363039,
         "sha256": "cb9140fae3ac5122c972d37adf83e1248471a38147ad76f8215c8872c6fd8322",
         "url": ")json" ATM_SIGLIP2 R"json(tokenizer.json"},
        {"path": "foley/siglip2-base-patch16-512/model.safetensors", "size": 1503344520,
         "sha256": "fe0e601c625e69eed8e73500d39e9b6164403fe03db8048e87913c3cefbbb3fe",
         "url": ")json" ATM_SIGLIP2 R"json(model.safetensors"},
        {"path": "foley/larger_clap_general/config.json", "size": 643,
         "sha256": "6268f76a067e3104bf9001e97d7dcc0fcfb16f3086cfb478ee72d95900d7fc1d",
         "url": ")json" ATM_LCLAP R"json(config.json"},
        {"path": "foley/larger_clap_general/preprocessor_config.json", "size": 541,
         "sha256": "9739f58296aa6f9ac18008fd0150fb2649bc554985fbde86d0a4041c882ac753",
         "url": ")json" ATM_LCLAP R"json(preprocessor_config.json"},
        {"path": "foley/larger_clap_general/special_tokens_map.json", "size": 280,
         "sha256": "06e405a36dfe4b9604f484f6a1e619af1a7f7d09e34a8555eb0b77b66318067f",
         "url": ")json" ATM_LCLAP R"json(special_tokens_map.json"},
        {"path": "foley/larger_clap_general/tokenizer_config.json", "size": 1362,
         "sha256": "e2eb445cfdbf4711de620cbdf10478b0423950799e85652d9f28da47066ab86d",
         "url": ")json" ATM_LCLAP R"json(tokenizer_config.json"},
        {"path": "foley/larger_clap_general/merges.txt", "size": 456318,
         "sha256": "1ce1664773c50f3e0cc8842619a93edc4624525b728b188a9e0be33b7726adc5",
         "url": ")json" ATM_LCLAP R"json(merges.txt"},
        {"path": "foley/larger_clap_general/vocab.json", "size": 798293,
         "sha256": "ed19656ea1707df69134c4af35c8ceda2cc9860bf2c3495026153a133670ab5e",
         "url": ")json" ATM_LCLAP R"json(vocab.json"},
        {"path": "foley/larger_clap_general/tokenizer.json", "size": 2108774,
         "sha256": "dc239041d98de27ffc3975473a1a23e3db4c937b23c138c38bbc66588bd247e5",
         "url": ")json" ATM_LCLAP R"json(tokenizer.json"},
        {"path": "foley/larger_clap_general/pytorch_model.bin", "size": 776444665,
         "sha256": "314eb00cce6ad68d25237b8446b659ccdb136ed4672c1bca470f142f72455026",
         "url": ")json" ATM_LCLAP R"json(pytorch_model.bin"}
      ]
    },
    {
      "id": "hunyuan-foley.xl",
      "title": "HunyuanVideo-Foley XL: sound effects from a video, with less memory",
      "kind": "model",
      "licence": "Tencent Hunyuan Community License: commercial use allowed, except in the European Union, the United Kingdom and South Korea; the two encoders it reads (SigLIP 2, CLAP) are Apache 2.0.",
      "licence_url": "https://huggingface.co/tencent/HunyuanVideo-Foley",
      "notes": "The smaller HunyuanVideo-Foley: the same use (sfx.from_video, size xl), a little less exact; about 16 GB of graphics memory (8 GB with offload). Shares its sync model, decoder and encoders with the XXL one.",
      "files": [
        {"path": "foley/hunyuanvideo-foley/config_xl.yaml", "size": 1278,
         "sha256": "873c89fb63fd27ce08de95f5e66a7b27f862a0d3bfa6d90863db7f34607083cb",
         "url": ")json" ATM_HYFOLEY R"json(config_xl.yaml"},
        {"path": "foley/hunyuanvideo-foley/hunyuanvideo_foley_xl.pth", "size": 5854140970,
         "sha256": "cf7e93dca93063942bc22b53d4fe888e9d9d5a1ad7977873b7b2c6b126cf00ca",
         "url": ")json" ATM_HYFOLEY R"json(hunyuanvideo_foley_xl.pth"},
        {"path": "foley/hunyuanvideo-foley/synchformer_state_dict.pth", "size": 950058171,
         "sha256": "8aff082f2df5c3bc52759db0c865c7ee772ae6400b860d1b7e90413f2defb67c",
         "url": ")json" ATM_HYFOLEY R"json(synchformer_state_dict.pth"},
        {"path": "foley/hunyuanvideo-foley/vae_128d_48k.pth", "size": 1486465965,
         "sha256": "07e6139ff33bd21ba8a7a7f40ed24aab13ac0d100cd686f8a9f3e03dc5251cb1",
         "url": ")json" ATM_HYFOLEY R"json(vae_128d_48k.pth"},
        {"path": "foley/siglip2-base-patch16-512/config.json", "size": 276,
         "sha256": "a15bc39fbcd92498dea6d5d3cb2b0d8b9ba389d6a3f2b565ea66128a2d141934",
         "url": ")json" ATM_SIGLIP2 R"json(config.json"},
        {"path": "foley/siglip2-base-patch16-512/preprocessor_config.json", "size": 394,
         "sha256": "63ff380d3e424f93e6fbca5cc8e74eeed882e96fd6be2b7728aed308f3ad1513",
         "url": ")json" ATM_SIGLIP2 R"json(preprocessor_config.json"},
        {"path": "foley/siglip2-base-patch16-512/special_tokens_map.json", "size": 636,
         "sha256": "baec30ea10906f16adb8c18af7a34023002c1746542612b8b41c9f09e1351351",
         "url": ")json" ATM_SIGLIP2 R"json(special_tokens_map.json"},
        {"path": "foley/siglip2-base-patch16-512/tokenizer_config.json", "size": 47164,
         "sha256": "14afe629fe4959b9e0d51e1852b8d9f7ad074f90a1a7125a4fcdd17f06e78fc8",
         "url": ")json" ATM_SIGLIP2 R"json(tokenizer_config.json"},
        {"path": "foley/siglip2-base-patch16-512/tokenizer.model", "size": 4241003,
         "sha256": "61a7b147390c64585d6c3543dd6fc636906c9af3865a5548f27f31aee1d4c8e2",
         "url": ")json" ATM_SIGLIP2 R"json(tokenizer.model"},
        {"path": "foley/siglip2-base-patch16-512/tokenizer.json", "size": 34363039,
         "sha256": "cb9140fae3ac5122c972d37adf83e1248471a38147ad76f8215c8872c6fd8322",
         "url": ")json" ATM_SIGLIP2 R"json(tokenizer.json"},
        {"path": "foley/siglip2-base-patch16-512/model.safetensors", "size": 1503344520,
         "sha256": "fe0e601c625e69eed8e73500d39e9b6164403fe03db8048e87913c3cefbbb3fe",
         "url": ")json" ATM_SIGLIP2 R"json(model.safetensors"},
        {"path": "foley/larger_clap_general/config.json", "size": 643,
         "sha256": "6268f76a067e3104bf9001e97d7dcc0fcfb16f3086cfb478ee72d95900d7fc1d",
         "url": ")json" ATM_LCLAP R"json(config.json"},
        {"path": "foley/larger_clap_general/preprocessor_config.json", "size": 541,
         "sha256": "9739f58296aa6f9ac18008fd0150fb2649bc554985fbde86d0a4041c882ac753",
         "url": ")json" ATM_LCLAP R"json(preprocessor_config.json"},
        {"path": "foley/larger_clap_general/special_tokens_map.json", "size": 280,
         "sha256": "06e405a36dfe4b9604f484f6a1e619af1a7f7d09e34a8555eb0b77b66318067f",
         "url": ")json" ATM_LCLAP R"json(special_tokens_map.json"},
        {"path": "foley/larger_clap_general/tokenizer_config.json", "size": 1362,
         "sha256": "e2eb445cfdbf4711de620cbdf10478b0423950799e85652d9f28da47066ab86d",
         "url": ")json" ATM_LCLAP R"json(tokenizer_config.json"},
        {"path": "foley/larger_clap_general/merges.txt", "size": 456318,
         "sha256": "1ce1664773c50f3e0cc8842619a93edc4624525b728b188a9e0be33b7726adc5",
         "url": ")json" ATM_LCLAP R"json(merges.txt"},
        {"path": "foley/larger_clap_general/vocab.json", "size": 798293,
         "sha256": "ed19656ea1707df69134c4af35c8ceda2cc9860bf2c3495026153a133670ab5e",
         "url": ")json" ATM_LCLAP R"json(vocab.json"},
        {"path": "foley/larger_clap_general/tokenizer.json", "size": 2108774,
         "sha256": "dc239041d98de27ffc3975473a1a23e3db4c937b23c138c38bbc66588bd247e5",
         "url": ")json" ATM_LCLAP R"json(tokenizer.json"},
        {"path": "foley/larger_clap_general/pytorch_model.bin", "size": 776444665,
         "sha256": "314eb00cce6ad68d25237b8446b659ccdb136ed4672c1bca470f142f72455026",
         "url": ")json" ATM_LCLAP R"json(pytorch_model.bin"}
      ]
    },
    {
      "id": "kokoro.82m",
      "title": "Kokoro: speech from text, with a ready-made voice",
      "kind": "model",
      "licence": "Apache 2.0: free to use, also commercially.",
      "licence_url": "https://huggingface.co/hexgrad/Kokoro-82M",
      "notes": "Clean English voices to choose from, fast: voice.make and Voice clips. Run by ComfyUI with the attome_kokoro node; the files must be in the models folder ComfyUI reads.",
      "files": [
        {"path": "Kokorotts/Kokoro-82M/config.json", "size": 2351,
         "sha256": "5abb01e2403b072bf03d04fde160443e209d7a0dad49a423be15196b9b43c17f",
         "url": ")json" ATM_KOKORO R"json(config.json"},
        {"path": "Kokorotts/Kokoro-82M/kokoro-v1_0.pth", "size": 327212226,
         "sha256": "496dba118d1a58f5f3db2efc88dbdc216e0483fc89fe6e47ee1f2c53f18ad1e4",
         "url": ")json" ATM_KOKORO R"json(kokoro-v1_0.pth"},
        {"path": "Kokorotts/Kokoro-82M/voices/af_bella.pt", "size": 523425,
         "sha256": "8cb64e02fcc8de0327a8e13817e49c76c945ecf0052ceac97d3081480e8e48d6",
         "url": ")json" ATM_KOKORO R"json(voices/af_bella.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/af_heart.pt", "size": 523425,
         "sha256": "0ab5709b8ffab19bfd849cd11d98f75b60af7733253ad0d67b12382a102cb4ff",
         "url": ")json" ATM_KOKORO R"json(voices/af_heart.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/af_nicole.pt", "size": 523430,
         "sha256": "c5561808bcf5250fe8c5f5de32caf2d94f27e57e95befdb098c5c85991d4c5da",
         "url": ")json" ATM_KOKORO R"json(voices/af_nicole.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/af_sarah.pt", "size": 523425,
         "sha256": "49bd364ea3be9eb3e9685e8f9a15448c4883112a7c0ff7ab139fa4088b08cef9",
         "url": ")json" ATM_KOKORO R"json(voices/af_sarah.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/af_sky.pt", "size": 523351,
         "sha256": "c799548aed06e0cb0d655a85a01b48e7f10484d71663f9a3045a5b9362e8512c",
         "url": ")json" ATM_KOKORO R"json(voices/af_sky.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/am_adam.pt", "size": 523420,
         "sha256": "ced7e284aba12472891be1da3ab34db84cc05cc02b5889535796dbf2d8b0cb34",
         "url": ")json" ATM_KOKORO R"json(voices/am_adam.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/am_eric.pt", "size": 523420,
         "sha256": "ada66f0eefff34ec921b1d7474d7ac8bec00cd863c170f1c534916e9b8212aae",
         "url": ")json" ATM_KOKORO R"json(voices/am_eric.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/am_fenrir.pt", "size": 523430,
         "sha256": "98e507eca1db08230ae3b6232d59c10aec9630022d19accac4f5d12fcec3c37a",
         "url": ")json" ATM_KOKORO R"json(voices/am_fenrir.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/am_liam.pt", "size": 523420,
         "sha256": "c82550757ddb31308b97f30040dda8c2d609a9e2de6135848d0a948368138518",
         "url": ")json" ATM_KOKORO R"json(voices/am_liam.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/am_michael.pt", "size": 523435,
         "sha256": "9a443b79a4b22489a5b0ab7c651a0bcd1a30bef675c28333f06971abbd47bd37",
         "url": ")json" ATM_KOKORO R"json(voices/am_michael.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/am_onyx.pt", "size": 523420,
         "sha256": "e8452be16cd0f6da7b4579eaf7b1e4506e92524882053d86d72b96b9a7fed584",
         "url": ")json" ATM_KOKORO R"json(voices/am_onyx.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/am_puck.pt", "size": 523420,
         "sha256": "dd1d8973f4ce4b7d8ae407c77a435f485dabc052081b80ea75c4f30b84f36223",
         "url": ")json" ATM_KOKORO R"json(voices/am_puck.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/bf_emma.pt", "size": 523420,
         "sha256": "d0a423deabf4a52b4f49318c51742c54e21bb89bbbe9a12141e7758ddb5da701",
         "url": ")json" ATM_KOKORO R"json(voices/bf_emma.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/bm_daniel.pt", "size": 523430,
         "sha256": "fc3fce4e9c12ed4dbc8fa9680cfe51ee190a96444ce7c3ad647549a30823fc5d",
         "url": ")json" ATM_KOKORO R"json(voices/bm_daniel.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/bm_fable.pt", "size": 523425,
         "sha256": "d44935f3135257a9064df99f007fc1342ff1aa767552b4a4fa4c3b2e6e59079c",
         "url": ")json" ATM_KOKORO R"json(voices/bm_fable.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/bm_george.pt", "size": 523430,
         "sha256": "f1bc812213dc59774769e5c80004b13eeb79bd78130b11b2d7f934542dab811b",
         "url": ")json" ATM_KOKORO R"json(voices/bm_george.pt"},
        {"path": "Kokorotts/Kokoro-82M/voices/bm_lewis.pt", "size": 523425,
         "sha256": "b5204750dcba01029d2ac9cec17aec3b20a6d64073c579d694a23cb40effbd0e",
         "url": ")json" ATM_KOKORO R"json(voices/bm_lewis.pt"}
      ]
    },
    {
      "id": "whisper.small",
      "title": "Whisper small: speech to text, word by word (many languages)",
      "kind": "model",
      "licence": "MIT: OpenAI's Whisper weights, converted for whisper.cpp; free to use, also commercially.",
      "licence_url": "https://github.com/openai/whisper/blob/main/LICENSE",
      "notes": "Hears what a clip says and times every word, for auto captions. Runs on the processor; about 12 times faster than real time on a recent one.",
      "files": [
        {"path": "ggml-small.bin", "size": 487601967,
         "sha256": "1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b",
         "url": ")json" ATM_WHISPER R"json(ggml-small.bin"}
      ]
    },
    {
      "id": "whisper.large-v3-turbo-q5",
      "title": "Whisper large-v3 turbo: speech to text, more exact (many languages)",
      "kind": "model",
      "licence": "MIT: OpenAI's Whisper weights, converted and quantized for whisper.cpp; free to use, also commercially.",
      "licence_url": "https://github.com/openai/whisper/blob/main/LICENSE",
      "notes": "About a third of the mistakes of Whisper small in Arabic (9 % of words wrong, against 26 %, on 100 FLEURS recordings), and as good in English. About 100 times real time on a recent graphics card (3.5 times on the processor), against 8.5 for small on the processor.",
      "files": [
        {"path": "ggml-large-v3-turbo-q5_0.bin", "size": 574041195,
         "sha256": "394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2",
         "url": ")json" ATM_WHISPER R"json(ggml-large-v3-turbo-q5_0.bin"}
      ]
    }
  ]
})json";

} // namespace

const std::vector<CatalogEntry> &builtin_catalog() {
  static const std::vector<CatalogEntry> catalog = [] {
    auto parsed = parse_catalog(nlohmann::json::parse(kCatalog));
    return parsed ? std::move(*parsed) : std::vector<CatalogEntry>{}; // a unit test checks that it parses and is not empty
  }();
  return catalog;
}

} // namespace atm::models
