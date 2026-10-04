#include "atm/models/models.hpp"

// The catalog compiled into this version. Every file is pinned to one revision of its publisher's repository, with the
// size and SHA-256 that revision reports, so what arrives is what was listed here or it is refused.

namespace atm::models {
namespace {

// Comfy-Org/MiniMax-H3 at revision e5eb578a89295337b8ff433a035929ce0279e0b6 (read 2026-10-04).
#define ATM_H3 "https://huggingface.co/Comfy-Org/MiniMax-H3/resolve/e5eb578a89295337b8ff433a035929ce0279e0b6/"

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
        "accepts": ["start_image", "end_image"],
        "seconds": {"min": 1, "max": 15},
        "sizes": {"multiple": 32, "max_pixels": 2088960},
        "settings": {
          "steps": {"type": "integer", "min": 1, "max": 50, "default": 8},
          "sampler": {"type": "choice", "options": ["res_multistep", "euler"], "default": "res_multistep"},
          "attention": {"type": "choice", "options": ["int8", "default"], "default": "int8"}
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
