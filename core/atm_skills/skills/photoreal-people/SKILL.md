---
name: photoreal-people
description: Real-looking people with MiniMax H3 - skin that does not look plastic, faces and hair that hold up, light and framing that read as filmed (live action, ads, UGC, brand films). Read with the skill film whenever people must look real; not for drawn, anime or stylised looks.
---

# Photoreal people with MiniMax H3

H3 is a distilled model run with a few-step add-on, and it smooths skin: faces come out waxy, hair uniform, which a viewer
reads as "AI" at once. The recipe below was chosen by a person watching versions of the same shot side by side (same
prompt, light and seed), one change at a time. Everything here is a setting a clip asks for: leave it out for drawn, anime
or stylised looks, where the defaults change nothing.

## Setting up

- Two add-ons from the catalog: `minimax-h3.turbo-v4-lora` (780 MB) and `minimax-h3.realism-people-lora` (131 MB).
  `models.list` says whether they are here; `models.fetch {id}` gets one. **Ask the owner before a download, with its size.**
- They load on the H3 models that use a few-step add-on (`minimax-h3.singularity-ref2va.int8`, `minimax-h3.fl2va.turbo8-int8`).

## The recipe (every shot of real people: faces, hair, hands, bodies)

`gen.create_clip {..., settings: {speed_lora: "turbo_v4", sampler: "euler", scheduler: "beta", realism_lora: 1.0, stage: "final"}}`

- `speed_lora: "turbo_v4"` (euler, beta) replaces the model's own few-step add-on: sharper eyes, real skin texture and
  freckles, hair with real strands, with the same mood and motion.
- `realism_lora: 1.0` on top: matte skin, natural colour, a less posed face. Its trigger word is added to the prompt for you.
- `stage: "final"` for the film (1952 x 1088): the detail skin needs. Work out the content at preview first.
- Chosen over every alternative on a face close-up (11 versions) and on a shot from behind of hair and shoulders
  (5 versions). In a 30 s ad it was used on every shot with people; one shot also came out better framed (a held product
  became large and readable).
- The shot keeps its prompt, seed and length: switching an existing clip to the recipe is a patch of its settings and a
  new take.

## Light and framing that read as filmed

- **Keep the light that carries the mood** (low sun, backlight on the hair). Moving a face into soft shade to stop the shine
  took the mood with it, and the person preferred the shinier original.
- Frame people the way real brand films do: medium shots, backlit, from behind, people small in the landscape; close-ups of
  hands, skin and things more than of faces.
- Faces far away (small in a wide shot) come out worst: keep them out of focus or turned away.

## Tested and not better (do not repeat)

- Words for skin ("visible pores, matte skin, unretouched") and soft shade light: a tiny gain, and the mood was lost.
- Other samplers and schedulers (er_sde, euler_ancestral; beta without Turbo v4), and a weaker few-step add-on (strength
  0.7, 10 steps): a little texture at full size, still plastic, and she moved less.
- The realism add-on without Turbo v4: changed little.
- A new face reference cropped from a take made with the recipe, and 3 megapixels at the final stage (25 % slower): not
  chosen over the recipe with the old reference at 2 megapixels.
- silveroxides' merged few-step add-ons (his test repository, no licence): the most freckles and texture in stills, but the
  person preferred the recipe in motion, and they added jewellery the description forbids.

## Not known yet

- Grain, a softer grade or an enhancer after generation.
- People talking (lip sync and the voice) with the recipe.
