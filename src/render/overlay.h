// What the renderer draws over the black hole on one window: a credit's text and the particles it
// breaks into. The credit roll (src/app/credit_roll.cpp) fills it in each frame.
#ifndef BLACK_HOLE_OVERLAY_H
#define BLACK_HOLE_OVERLAY_H

#include <cstdint>
#include <vector>

namespace render {

// A credit's glyphs as 8-bit coverage, row-major, top row first.
struct TextImage {
    uint32_t             id = 0;  // changes whenever the pixels do, so the renderer re-uploads
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> coverage;
};

// One particle as the GPU reads it (a per-instance vertex): world position, size on screen, and
// its light. World units are the hole's mass, with the hole at the origin. The size is in
// pixels rather than world units: seen from the camera, a particle's trip to the hole is a
// fifteen-fold shrink by perspective alone, which would lose the stream long before the disk.
struct OverlayParticle {
    float position[3];
    float radius;  // pixels
    float brightness;
    float heat;    // 0 as it leaves the text, 1 as it reaches the disk: tints it towards the disk
    float square;  // 1: a flat square block of the pixelated text; 0: a soft round glow
    float pad;
};
static_assert(sizeof(OverlayParticle) == 32);

// More than this many particles are not drawn. A credit's break-up stays well below it.
constexpr size_t kMaxOverlayParticles = 65536;

struct Overlay {
    // The text, or nullptr when none is showing. Its rectangle is on the plane one unit in front
    // of the camera, in the units where the screen's top edge is at y = tan(fov / 2): left, top,
    // right, bottom, with y up. `scale` enlarges it about the view axis as though it were that
    // many times nearer, which is how it flies in.
    const TextImage* text = nullptr;
    float            rect[4]{};
    float            scale      = 1.0f;
    float            alpha      = 0.0f;
    float            brightness = 1.0f;
    float            block      = 1.0f;  // drawn pixelated in blocks this many texels wide (1: as set)
    // Chromatic aberration: where the red and blue images sit relative to the green one, as a
    // fraction of the text's width and height: red x, y, blue x, y (y down). Zero: one image.
    float            split[4]{};

    std::vector<OverlayParticle> particles;
};

}  // namespace render

#endif
