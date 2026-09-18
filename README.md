# The Black Hole

A Windows screen saver that renders a black hole in the style of Gargantua from *Interstellar*,
with Vulkan and C++20.

## How it looks the way it does

Each pixel is one light ray, traced backwards from the camera through the curved spacetime of a
spinning (Kerr) black hole, all in one fragment shader
([`shaders/blackhole.frag`](shaders/blackhole.frag)). This is the approach of the paper behind
the film's images: James, von Tunzelmann, Franklin & Thorne, "Gravitational lensing by spinning
black holes in astrophysics, and in the movie Interstellar", Class. Quantum Grav. 32 (2015)
065001.

The ray is a null geodesic in Boyer-Lindquist coordinates, integrated with Hamilton's equations
(fourth-order Runge-Kutta). The photon's energy and axial angular momentum are conserved. Each
ray starts from the frame of a zero-angular-momentum observer at the camera: the frame the
spinning hole drags round with it. Step sizes come from how fast each coordinate is changing, so
the integration stays stable both near the horizon, where frame dragging spins the ray round,
and near the spin axis, where the coordinates themselves are singular. The spin is
`SPIN = 0.95` (a/M) in the shader. The film's Gargantua was about 0.999.

Everything characteristic of the film's image falls out of the geodesics:

- **The shadow.** Rays that fall through the horizon. Spin flattens it into a D on the side
  where the disk comes towards the camera, because photons orbiting with the spin can get
  closer before they are lost.
- **The photon ring.** Rays that circle near the photon orbits come out as a thin bright ring at
  the shadow's edge.
- **The arch.** The disk is flat, but light from its far side is bent over the top of the hole and
  under the bottom, so it appears as a halo standing up around the shadow.
- **Star streaks.** The background is lensed too. At this distance the Einstein radius is
  about 20°, so stars near the hole are drawn out into arcs.

The accretion disk is a thin plane from the prograde innermost stable circular orbit
(r ≈ 1.94 M at this spin, against 6 M without spin) out to 17 M. It has turbulent streaks that
rotate at the Kerr orbital rate. Its colour comes from a thin-disk temperature profile
(T ∝ r^−3/4). The frequency shift comes from the gas's circular-orbit four-velocity, which
combines Doppler beaming and gravitational redshift. The shift is toned down the way the film
toned it down.

The disk's streaks are anti-aliased. Lensing squeezes the disk hard near the shadow, so one pixel
there can cover many streaks. The tracer records each ray's disk crossings and compares them
with the neighbouring pixels' crossings to find how much of the disk each pixel spans. Detail
finer than that fades to its average. That count includes the extra fineness the pattern gets
from winding up as the inner disk out-orbits the outer. Where even the disk's overall radial
profile falls inside a few pixels, four samples are taken across the pixel.

The surroundings are a skybox: a star field with faint nebulous cloud and a dust-lane band. It
is baked on the CPU at start-up into a 2048² cubemap with mipmaps
([`src/render/skybox.cpp`](src/render/skybox.cpp)). The mipmaps keep the stars from sparkling
where lensing squeezes a wide patch of sky into a few pixels.

The tracer writes linear HDR into an offscreen image, and two more passes finish the frame. A
compute bloom chain ([`shaders/bloom_down.comp`](shaders/bloom_down.comp),
[`bloom_up.comp`](shaders/bloom_up.comp)) takes light above a soft threshold. It downsamples
through six half-size levels with a 13-tap filter, using a Karis average on the first level to
keep single hot pixels from flickering. It then adds the levels back up with a tent filter. The
composite ([`shaders/composite.frag`](shaders/composite.frag)) adds that glow to the image, then
exposes it and tone maps it (ACES). The glow is mostly around the Doppler-brightened side of the
disk, where the film's image blooms too.

### The camera

The screen saver's camera roams ([`src/render/camera.cpp`](src/render/camera.cpp)). It orbits
the hole continuously. Its pace, distance, height above the disk, roll, and the hole's place in
the frame each ease towards a randomly chosen value, hold there, then pick another.

- **Pace:** usually the original slow drift. Now and then it is two to three times faster.
- **Distance:** close (12.5–18 M, where the disk runs off the screen and the hole is framed off
  centre), middle (22–34 M), or far (42–64 M, where the whole system sits small in the stars).

Every one and a half to two and a half minutes the camera also breaks pattern with an event:

- **Dive:** it sinks through the disk's plane, where the disk thins to a line between the arches
  above and below the shadow, and hangs below it for a while.
- **Overhead:** it climbs to 55–72° and looks down across the disk at the shadow.
- **Swoop:** it goes in fast to about 9 M, well inside the disk's outer edge, then back out.

The development window (`/w`) keeps the original fixed orbit, which is a function of time alone,
so any frame can be captured again.

## Building

The build copies [Nuke-Saver](../Nuke-Saver)'s: a plain Makefile driving MinGW-w64.

### Prerequisites (MSYS2 UCRT64)

- `g++`, `windres`, and `make` (`mingw-w64-ucrt-x86_64-toolchain`)
- `glslc` (`mingw-w64-ucrt-x86_64-shaderc`)
- the Vulkan headers (`mingw-w64-ucrt-x86_64-vulkan-headers`)
- `xxd`

No Vulkan SDK is needed. [volk](third_party/volk) (vendored) loads `vulkan-1.dll` at run time,
so nothing links against it. A machine without a Vulkan driver still gets a (black) screen saver.

### Commands

```sh
make            # build the-black-hole.scr
make install    # copy to %LOCALAPPDATA%\The-Black-Hole and select it as the screen saver
make clean
```

The shaders are compiled with `glslc -Werror` and embedded in the `.scr`; nothing is loaded from
disk at run time.

## Running

| Argument | Behaviour |
|----------|-----------|
| (none) or `/s` | Full screen on every monitor; any key, click or mouse movement ends it |
| `/w` | A resizable window, for development, on the fixed camera; Esc closes it |
| `/p <hwnd>` | Live preview in the Screen Saver Settings dialog (roaming camera, smaller star map for a quick start) |
| `/c` | Settings (there are none yet) |
| anything else | Exits immediately |

### Diagnostics

| Environment variable | Effect |
|----------------------|--------|
| `BLACK_HOLE_LOG=1` | Log to `%TEMP%\the-black-hole.log` (or set it to a path) |
| `BLACK_HOLE_CAPTURE=<file.bmp>` | Save one frame and exit |
| `BLACK_HOLE_CAPTURE_AT=<seconds>` | When to take that frame (default 4) |
| `BLACK_HOLE_TIME=<seconds>` | Offset the animation clock, to capture elsewhere in the orbit (the roaming camera is stepped forward to it) |
| `BLACK_HOLE_CAMERA=classic\|roaming` | Override the camera (`/s` roams, `/w` is classic) |
| `BLACK_HOLE_SEED=<n>` | Fix the roaming camera's choices, so a run can be repeated |
| `BLACK_HOLE_EVENT=dive\|overhead\|swoop` | Roaming camera: every event is this one, the first after 8 seconds |
| `BLACK_HOLE_VALIDATE=1` | Enable the Khronos validation layer if it is installed |

## Layout

```
shaders/            fullscreen.vert, blackhole.frag (the ray tracer), bloom_*.comp, composite.frag
src/main.cpp        argument dispatch
src/app/            window host, argument parsing, input rules, logging
src/render/         Vulkan renderer, camera paths, skybox generator, embedded-shader lookup
third_party/volk/   Vulkan meta-loader
tools/              embed_shaders.sh (SPIR-V -> C arrays)
```
