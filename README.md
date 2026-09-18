# The Black Hole

A Windows screen saver that renders a black hole in the style of Gargantua from *Interstellar*,
with Vulkan and C++20.

## How it looks the way it does

Each pixel is one light ray, traced backwards from the camera through the curved spacetime of a
Schwarzschild (non-spinning) black hole, all in one fragment shader
([`shaders/blackhole.frag`](shaders/blackhole.frag)). A photon's orbit obeys
u″ + u = 3Mu² (u = 1/r), which can be integrated as a straight-line path pushed by an extra
acceleration −(3/2)·r_s·h²·**r**/|**r**|⁵, where h is the photon's conserved angular momentum.
Everything characteristic of the film's image falls out of that one equation:

- **The shadow.** Rays with a small impact parameter fall through the horizon.
- **The photon ring.** Rays that graze r = 3M wrap around the hole and come out as a thin bright
  circle at the shadow's edge.
- **The arch.** The disk is flat, but light from its far side is bent over the top of the hole and
  under the bottom, so it appears as a halo standing up around the shadow.
- **Star streaks.** The background is lensed too. At this distance the Einstein radius is
  about 20°, so stars near the hole are drawn out into arcs.

The accretion disk is a thin plane with turbulent, differentially rotating (Keplerian) streaks.
Its colour comes from a thin-disk temperature profile (T ∝ r^−3/4). It also has relativistic
Doppler beaming and gravitational redshift, toned down the way the film toned them down.

The film's renders were made with Kip Thorne's equations for a *spinning* black hole; see
James, von Tunzelmann, Franklin & Thorne, "Gravitational lensing by spinning black holes in
astrophysics, and in the movie Interstellar", Class. Quantum Grav. 32 (2015) 065001. This first
version uses the non-spinning case, which gets the look without the Kerr geodesic equations.
Spin would mostly flatten one side of the shadow.

The surroundings are a skybox: a star field with faint nebulous cloud and a dust-lane band. It
is baked on the CPU at start-up into a 2048² cubemap with mipmaps
([`src/render/skybox.cpp`](src/render/skybox.cpp)). The mipmaps keep the stars from sparkling
where lensing squeezes a wide patch of sky into a few pixels.

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
| `/w` | A resizable window, for development; Esc closes it |
| `/p <hwnd>` | Preview pane of the Screen Saver Settings dialog (currently black) |
| `/c` | Settings (there are none yet) |
| anything else | Exits immediately |

### Diagnostics

| Environment variable | Effect |
|----------------------|--------|
| `BLACK_HOLE_LOG=1` | Log to `%TEMP%\the-black-hole.log` (or set it to a path) |
| `BLACK_HOLE_CAPTURE=<file.bmp>` | Save one frame and exit |
| `BLACK_HOLE_CAPTURE_AT=<seconds>` | When to take that frame (default 4) |
| `BLACK_HOLE_TIME=<seconds>` | Offset the animation clock, to capture elsewhere in the orbit |
| `BLACK_HOLE_VALIDATE=1` | Enable the Khronos validation layer if it is installed |

## Layout

```
shaders/            fullscreen.vert, blackhole.frag (the ray tracer)
src/main.cpp        argument dispatch
src/app/            window host, argument parsing, input rules, logging
src/render/         Vulkan renderer, skybox generator, embedded-shader lookup
third_party/volk/   Vulkan meta-loader
tools/              embed_shaders.sh (SPIR-V -> C arrays)
```
