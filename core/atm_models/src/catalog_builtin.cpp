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
// ggerganov/whisper.cpp at revision 5359861c739e955e79d9a303bcbc70fb988958b1 (read 2026-10-09: the revision, size and hash are what the
// repository's own headers report for ggml-small.bin, and the hash matches the file as downloaded).
#define ATM_WHISPER "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/"

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
        "sizes": {"multiple": 32, "max_pixels": 2088960},
        "settings": {
          "steps": {"type": "integer", "min": 1, "max": 50, "default": 8},
          "sampler": {"type": "choice", "options": ["res_multistep", "euler"], "default": "res_multistep"},
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
        "sizes": {"multiple": 32, "max_pixels": 2088960},
        "settings": {
          "steps": {"type": "integer", "min": 1, "max": 50, "default": 8},
          "sampler": {"type": "choice", "options": ["res_multistep", "euler"], "default": "res_multistep"},
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
        "sizes": {"multiple": 32, "max_pixels": 2088960},
        "settings": {
          "steps": {"type": "integer", "min": 1, "max": 50, "default": 8},
          "sampler": {"type": "choice", "options": ["res_multistep", "euler"], "default": "res_multistep"},
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
