---
name: web-motion
description: Make motion graphics for a video with web pages (HTML, CSS, JavaScript) - titles, lower thirds, kinetic type, lists, counters, charts, logo stings, end cards, UI mock-ups, transitions. Attome draws the page frame by frame in a headless browser and the result goes on the timeline, with transparency over the footage. Read it before calling web.preview or web.render.
---

# Web motion graphics with Attome

You write a page; Attome draws it, frame by frame, at the film's size and rate. Use it for everything that is **type, shape, layout,
data or UI**: it comes out crisp, exact and on brand, which a video model cannot do (it garbles letters) and which is faster than 3D.
Use generated video for people, places and things that move in the world; put web graphics over it.

## Setting up

`web.status` says which browser draws (Edge is on every Windows; Chrome works; ATTOME_BROWSER names another). Nothing is downloaded.

## The loop

1. **Write the page** (one file, or a folder with `index.html` and its pictures and fonts). Size the body to the canvas
   (`width: 1920px; height: 1080px` for 16:9; 1080 x 1920 for 9:16).
2. **Look** at two or three moments: `web.preview {path or html, at: 0.5, output: ".../look.png", transparent: true}`. Open the picture.
   `page_errors` lists what the page threw: fix those first.
3. **Render**: `web.render {path, output: "<folder>", seconds, fps: <the sequence's rate>, width, height}` (give the canvas size: a vertical
   film needs `width: 1080, height: 1920`, or the page is drawn landscape and shrunk to fit, captions and cards far too small). A folder gives a PNG sequence that keeps
   transparency (the default for a folder); an `.mp4` gives an opaque video (a full-screen card). A job: `jobs.get` until done.
4. **Put it in the film**: `timeline.edit add_clip {path: "<folder>", at, track: "new"}`. A sequence on a track above the video
   lays its transparent parts over it. Trim, move and fade it like any clip.

Several pages draw at once (one per second of animation, up to 6): about 25-40 frames a second at 1080p, so a 5 s title renders
in about 5 s. Putting a long sequence on the timeline does not slow the export: its pictures are read ahead.

## How to animate so every frame is exact

Attome does not play the page in real time: before each frame it sets the page's time, then takes the picture. So:

- **Use CSS animations and the Web Animations API** (`element.animate(...)`): they are set to the frame's time exactly, delays and
  easing included. This is the first choice for everything.
- **Or draw from a clock**: `requestAnimationFrame(t => ...)`, `performance.now()` and `Date.now()` run on the film's time.
  For full control define `window.attomeFrame = (seconds) => { ... }`: it is called on every frame with the time, after the
  animations were set; draw everything that depends on time in it (counters, charts, canvas).
- **Do not use** `setTimeout` or `setInterval` for animation, CSS `transition` on hover or class changes made later, or anything
  that waits for the network. Put every library and font in the folder (the page has no internet unless `network: true`).
- **Draw each frame from its time alone.** The frames are shared out between several pages, so one page does not see every
  frame or see them in order. Compute positions from `t` (an easing of `t`, a seeded random); never step a simulation from the
  frame before. A page that must carry something from frame to frame renders with `workers: 1`.
- Animations start at time 0 when the page loads. Stagger with `animation-delay`, not with timers.
- Use `animation-fill-mode: both` (or `forwards`) so a thing stays where it ends and is hidden before it starts.

## Design rules that keep it looking professional

- **One grid, one type scale.** Safe margins of 6% of the width on each side; three sizes of type at most (title, line, small).
- **Few colours**: the brand's two or three plus white; a dark panel at 55-75% opacity behind text over busy footage.
- **Motion with purpose**: things enter in 0.3-0.6 s with an ease-out (`cubic-bezier(.16,1,.3,1)`), leave faster; stagger list items
  by 0.08-0.15 s. Nothing moves all the time except what must draw the eye.
- **Hold to read**: about 0.3 s per word on screen after the motion ends, and never less than 1.5 s for a line.
- **Match the cut**: start the graphic on the beat or the word it belongs to; its length is the scene's.
- **Text as text**: real fonts (Segoe UI, Inter, or a font file in the folder), never pictures of text.
- Look at the frames at full size before the final render: spacing, cropping at the edges, and the contrast against the footage.

## Examples (ready to adapt)

`skill.get {"id": "web-motion", "file": "examples/<name>.html"}`; save it in a folder, change the words and colours, preview, render.

- `examples/title_card.html` - a big title with a line and a subtitle, staggered in, for the opening or a chapter.
- `examples/lower_third.html` - a name or a service with a coloured tag and a line, sliding in at the lower left; transparent.
- `examples/list_reveal.html` - a heading and items that appear one by one with ticks; transparent.
- `examples/counter.html` - a number that counts up to its value with a label, drawn from `window.attomeFrame`.
- `examples/end_card.html` - a logo (a picture file in the folder) with a tagline and contact lines.

The CSS custom properties at the top of each (`--brand`, `--accent`, `--text`) carry the colours; the timings are in seconds in one place.
