---
name: photoreal-people
description: Real-looking people with MiniMax H3 - skin that does not look plastic, faces that hold up, light and framing that read as filmed (live action, ads, UGC, brand films). Read with the skill film whenever people must look real; not for drawn, anime or stylised looks.
---

# Photoreal people with MiniMax H3

H3 is a distilled model run with a few-step add-on, and it smooths skin: faces come out waxy and evenly lit, which a viewer
reads as "AI" at once. These rules were found by testing one thing at a time on the same shot (same prompt, light and seed,
final stage) and letting a person judge the result. Each says how sure it is. Everything here is a setting a clip asks for:
leave them out for drawn, anime or stylised looks, where the defaults change nothing.

## Setting up

- The add-ons are models of the catalog: `minimax-h3.turbo-v4-lora` (780 MB) and `minimax-h3.realism-people-lora` (131 MB).
  `models.list` says whether they are here; `models.fetch {id}` gets one. **Ask the owner before a download, with its size.**
- They load on the H3 models that use a few-step add-on (`minimax-h3.singularity-ref2va.int8`, `minimax-h3.fl2va.turbo8-int8`).

## The recipe (use it for every shot of real people)

`gen.create_clip {..., settings: {speed_lora: "turbo_v4", sampler: "euler", scheduler: "beta", realism_lora: 1.0, stage: "final"}}`

- **The few-step add-on decides the skin.** Turbo v4 in place of the model's own add-on gave sharper eyes, defined freckles
  and real skin texture, with the same mood and motion.
- **With the realism add-on on top** (`realism_lora: 1.0`; its trigger word is added to the prompt for you) skin turns matte
  and colour natural, and the face less posed. Of four versions (the model's own add-on, the realism add-on alone, Turbo v4
  alone, both) the person chose Turbo v4 alone first and, on a second look, this pair (1 shot, 4 versions: close; confirm on
  more shots). The realism add-on alone changed little.
- Work at preview size for content (`stage` left out), then make the shots with people at `stage: "final"`: the final
  stage adds the detail that skin needs (1952 x 1088 against 1280 x 704).

## Light and framing that read as filmed

- **Keep the light that carries the mood.** Moving a face into soft shade to stop the shine took the mood with it, and the
  person preferred the shinier original. Change the add-on, not the light.
- Frame people the way real brand films do: medium shots, backlit, from behind, people small in the landscape; close-ups of
  hands, skin and things rather than of faces. A face that fills the frame shows every weakness of the model.
- Faces far away (a small face in a wide shot) come out worst at any setting: keep them out of focus or turn them away.

## Known not to work (measured)

- **Fixing plastic skin with words and samplers alone.** Seven versions of one close-up: "visible pores, matte skin,
  unretouched" and soft light in the prompt; euler, er_sde or euler_ancestral with beta; `lora_strength` 0.7 with 10 steps.
  The best showed pores at full size, but the person saw only a tiny gain, still plastic, and preferred the original:
  the weaker add-on made her move less and the softer light lost the mood.

## Not known yet

- Whether a face reference made with the recipe (rather than one cropped from a preview take) carries real skin into every
  shot of that person.
- Grain, a softer grade or an enhancer after generation.
- Hands, bodies and skin away from the face (arms, backs) with the recipe.
