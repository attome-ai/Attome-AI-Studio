---
name: blender
description: Use Blender from Attome to make 3D and motion-graphic shots for a film - 3D titles, product turntables, abstract backgrounds, diagrams that move, objects and places that no video model can keep the same. Read it before calling any blender.* Tool.
---

# Blender with Attome

Attome runs a **headless Blender** for you. You write Python; Blender builds and draws the scene; the result is an ordinary file
(a picture, a PNG sequence with a clear background, or an mp4) that you put on the timeline like any clip. Nothing is held open
between calls: **the scene lives in a .blend file**, so work in steps and keep the file.

## When it is the right tool

- Text that must be exact and look solid: titles, lower thirds, numbers, equations as shapes (a video model garbles letters).
- Things that must be *the same* every frame and shot: a logo, a product, a diagram, a planet, a machine part, an arrow.
- Precise camera moves: a turntable, an orbit, a push in, a crane up, at the speed you choose.
- Abstract backgrounds and loops: particles-like fields of shapes, gradients, glowing objects, floors with reflections.
- Explaining: a field with arrows, a magnet and its lines, an orbit, a molecule built from spheres - drawn, not generated.

It is the wrong tool for people, faces, animals and natural scenes in motion: use the generative models (`film`) for those.

## Setting up

1. `blender.status`. If `installed` is true, go on. If not, **ask the person first** (about 400 MB; it is a GPL-3.0 program that
   Attome only runs), then `blender.install` (a job: `jobs.get`), then `blender.status` again.
2. Work in a folder inside the project or beside it. Name the scene file (`scene.blend`).

## The loop

1. **Build:** `blender.run {code, blend: "<folder>/scene.blend", save: "<folder>/scene.blend"}`. With no existing file it starts
   empty. The code has `bpy` and the helper module `ab` ready; whatever it leaves in a variable named `result` (JSON) comes back.
2. **Look:** `blender.render {blend, output: "<folder>/look.png", quality: "draft"}` draws one frame at half size in seconds. Then
   **look at the picture** (open the file; you cannot judge a 3D scene from its description). Change what is wrong, render again.
3. **Draw for real:** `blender.render {blend, output: "<folder>/shot.mp4"}` (a video of the frame range), or a folder for a PNG
   sequence. With `transparent: true` a sequence keeps the background clear so it lays over the film.
4. **Put it in the film:** `timeline.edit add_clip` with the file. A PNG sequence: render a video of it instead, or ask the person
   how they want it; a transparent shot is best as a PNG picture sequence the editor can import.
5. `blender.inspect {blend}` reads a file back (objects, sizes, camera, frames) when you did not build it or lost track.

A script that fails returns the Python traceback: read it, fix the line, run again. Blender's Python API is `bpy`; anything it can do,
you can do in `code`.

## The helper module `ab`

`ab.set_render(width, height, fps, engine, samples, transparent, frames)` - engine `eevee` (fast; use it unless you need real glass or
caustics), `cycles` (slow, exact), `workbench` (flat, instant). `ab.world(color, strength, hdri)`, `ab.material(name, color, metallic,
roughness, emission, emission_strength, alpha)`, `ab.add("cube"|"sphere"|"cylinder"|"cone"|"torus"|"plane"|"monkey", ...)`,
`ab.text3d(text, size, depth, bevel, material_, location)`, `ab.import_model(path)` (glb gltf obj fbx stl ply usd abc),
`ab.picture_plane(path)`, `ab.camera(location, target, lens)`, `ab.look_at(obj, target)`, `ab.light("area"|"sun"|"point"|"spot", ...)`,
`ab.three_point_lights(target)`, `ab.keyframe(obj, path, frame, value)`, `ab.spin(obj, frames, turns)`, `ab.orbit(cam, target, radius,
height, frames)`, `ab.dolly(cam, start, end, frames)`, `ab.describe_scene()`. Read the file whole before using it:
`skill.get {"id": "blender", "file": "scripts/attome_blender.py"}`. Sizes are metres, angles degrees. A camera at negative Y looking
at the origin sees a text made by `ab.text3d` the right way round.

## Rules that save renders

- **Match the film.** Set the size and rate to the sequence (`project.get` gives the canvas and the rate) before building; a 3D
  shot at another shape gets cropped or letterboxed.
- **Always a light and a world.** A scene with neither draws black. `ab.three_point_lights` and `ab.world` are the quick start.
- **Draft first, always.** Never draw a whole video before one draft frame looks right, and one at the first, middle and last
  frame of a move when the camera or an object moves.
- **Loops:** a spin or an orbit of whole turns with `linear` movement ends where it began, so the clip can repeat.
- **Length:** frames = seconds x fps. Set `frames` in `ab.set_render` or `ab.spin`/`ab.orbit` and draw that range.
- **Time:** eevee at 1080p draws a frame in about a second on a graphics card; cycles ten to a hundred times that. Choose by the
  shot, say the estimate before a long render, and use `quality: "draft"` to measure.
- **Colour:** a title in pure white on pure black looks harsh; keep a little colour in the light and the world.
- **Do not write a person's name, a brand or a story into a general recipe**; keep what you learn here in general terms (the
  skill `film` says how).
