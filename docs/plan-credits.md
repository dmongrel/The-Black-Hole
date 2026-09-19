# Plan: movie-credit text that breaks into particles

Status: built (2026-09-18). Changes after review:
- The left-to-right break-up sweep is replaced. The whole line pixelates over 1.2 s, and then
  each block becomes one particle, with nothing added. The blocks come loose at random over 1.5 s,
  so the letters dissolve.
- About a fifth of the particles hang back 0.8–2.4 s before the flow takes them.
- Particles fade out halfway round the spiral onto the disk.
- The typeface is Michroma (SIL OFL 1.1, the same one GoLLM uses), embedded with its licence. See
  THIRD_PARTY_NOTICES.md.
- Before the break-up, the text shakes jerkily with its red, green and blue split apart, up to the
  moment it pixelates.
- A credit naming Sci-Man Dan holds while the Earth comes on. The Earth is a ray-cast sphere with
  NASA's Blue Marble and Black Marble images, lit by the disk, and it is drawn round wherever it is
  in the frame. It sweeps in 3.3 s after the credit lands, taking 3 s, well clear of the shadow
  and the text. It is 1/5 to 1/3 of the shadow's size. The label "Earth (ROUND)" and a thumbs up
  fades in halfway through the sweep. After a 2 s rest the Earth pixelates on the text's block
  grid and breaks into particles coloured as it was drawn (EarthColor mirrors the shader). They
  set off from the sphere itself and spiral into the hole. The credit's own shake and break-up
  follow once the last of them has gone.

## What it does

1. A line of text flies in from behind the camera and slows to a halt in front of it, like a
   film credit, coming to rest in the lower-left third of the screen, clear of the hole. It is fixed to the camera while it travels and holds.
2. It holds for a moment, then "pixelates": the glyphs dissolve into a great many particles.
3. The particles leave the camera's frame of reference and flow, in a sweeping arc, down to the
   inner edge of the accretion disk, the hot bright ring hugging the shadow. They follow the black hole wherever the camera has moved it on
   screen. They shrink and dim as they go.
4. On reaching the disk they vanish. One second later the next line starts.
5. After the last line, a one-minute pause, then the list starts again from the top.

Line format: `Directed by: Christopher Nolan` becomes two lines, left-justified, split at the
first colon, which is dropped. A line without a colon is a single left-justified line. An empty list turns the
feature off.

## Where the text comes from

The Screen Saver Settings dialog belongs to Windows; its thumbnail is only a window to draw into
and cannot hold a text box. The screen saver's own **Settings…** button (`/c`) can: it will open
a small dialog with a multi-line text box, one credit per line, saved to a file in the user's
profile. The file can also be edited by hand. (Decided.)

- File: `%APPDATA%\The-Black-Hole\credits.txt`, UTF-8, one credit per line, blank lines ignored.
  Each half of a credit is trimmed of surrounding spaces.
- Default list: [`assets/credits-default.txt`](../assets/credits-default.txt), embedded in the
  `.scr` at build time. It fills the Settings text box and plays whenever the user has not saved
  a list of their own. Saving an empty list turns the credits off; that is distinct from never
  having saved one.
- `/c` dialog: the text box, OK / Cancel. Room later for the other settings (spin, bloom, event
  frequency).

## Rendering

**Text.** Rasterised once per line on the CPU with GDI (DirectWrite if the anti-aliasing is not
good enough) into a single-channel alpha texture. Unicode throughout. Drawn as a camera-space
quad in its own pass after the ray tracer and before bloom, so the text glows slightly with
everything else. Typeface: a light, widely letter-spaced sans in warm white (Segoe UI Light,
falling back to Bahnschrift Light), both lines the same weight. Size and colour tuned by eye.

**Fly-in.** The quad starts behind the camera (negative depth), passes the near plane and eases
to rest at its hold distance. Its position and fade are one function of time, so a capture can
land on any moment of it.

**Break-up.** Each particle is seeded from an opaque texel of the glyph texture: a grid sampled
at the texture's resolution and kept only where alpha is above a threshold, giving tens of
thousands of particles for a typical line. A compute pass owns their state in a storage buffer
(position, velocity, age, size, brightness). The break-up sweeps across the line from left to
right rather than all at once, so it reads as the words dissolving in order.

**The flow.** At the moment of break-up each particle's position is converted from camera space
into world space. From then on it moves in world coordinates, so it stays with the hole as the
camera moves. Its path is a curl-noise-disturbed spiral: pulled towards the target radius on the
disk, turned in the disk's direction of rotation so that the stream arcs round with the flow
rather than dropping straight in, and settling into the disk plane as it arrives. A particle is
retired when it reaches the disk (radius within a band of the target and height near zero) or
exceeds its maximum age.

**Particles are drawn** as additive point sprites in the same pass as the text, sized and dimmed
by distance travelled. Tracing every particle's light through the hole's curved spacetime is out
of reach at this count, so each is bent with the point-lens approximation instead: its angular
offset from the hole, beta, is pushed outward to the primary image angle
theta = (beta + sqrt(beta^2 + 4 theta_E^2)) / 2, where theta_E is the Einstein angle for the
particle's distance behind the hole (zero in front of it). A particle whose image would fall
inside the shadow's angular radius (about 5.2 M / camera distance) is hidden. Not exact near the
inner edge, but the stream arches round the hole the way the disk does.

**Multiple monitors.** The credits appear on the primary monitor only; the others show the black
hole alone.

**Preview pane.** The thumbnail shows the black hole only; at 152×112 the text would be a few
unreadable pixels.

## Timing (defaults, all to be tuned by eye)

| Phase | Duration |
|-------|----------|
| Fly-in from behind the camera | 3 s, easing out |
| Hold | 3 s for a single line, 4 s for two |
| Break-up sweep | 1 s across the line |
| Flow to the disk | about 10 s from break-up to the last particle absorbed (your call) |
| Gap before the next line | 1 s after the last particle is gone |
| Pause after the last line | 60 s |

## Decisions

- ~~Q1~~ Text source: decided, the `/c` dialog writing `credits.txt` in `%APPDATA%`.
- ~~Q2~~ The colon: decided, dropped.
- ~~Q3~~ Disk target: decided, the inner edge (ISCO, r ≈ 1.94 M).
- ~~Q4~~ Placement: decided, lower-left third.
- ~~Q5~~ Typeface: decided, light wide sans, warm white; size by eye.
- ~~Q6~~ Near the hole: decided, point-lens bending plus hiding inside the shadow.
- ~~Q7~~ Monitors: decided, primary only.
- ~~Q8~~ Preview pane: decided, black hole only.

## Work, in order

1. `credits.txt` loading and line parsing (colon split, trimming, blank lines), with the embedded
   default list as the fallback. Environment variable
   `BLACK_HOLE_CREDITS=<file>` overrides the path for development.
2. `/c` dialog with the text box, saving the file.
3. Text rasterisation into a texture; camera-space quad pass between the tracer and bloom.
4. Fly-in and hold, driven by a credits sequencer with the timing above.
5. Particle seeding from the glyph texture; compute simulation; point-sprite drawing.
6. The flow field towards the disk, in world space.
7. The sequencer's full cycle: next line, 60-second pause, repeat.
8. Capture hooks (`BLACK_HOLE_CREDITS_AT=<seconds>` to jump into the sequence) and a
   performance check at 3440×1440, where the ray tracer already takes most of the frame.
9. README, commit.
