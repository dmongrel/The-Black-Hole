// The Earth that one credit brings on (see credit_roll.cpp): its textures, decoded from the NASA
// images embedded in the .scr, and its shading on the CPU, the same as shaders/earth.frag, so the
// particles it breaks into are the colours it was drawn in.
#ifndef BLACK_HOLE_EARTH_H
#define BLACK_HOLE_EARTH_H

#include "render/overlay.h"

namespace app {

// Decoded on first use and kept. Its id is 0 if the images could not be read.
const render::EarthImages& EarthTextures();

// The linear HDR colour of the surface of `earth` whose outward normal is `n`, seen along `view`
// (unit, from the surface towards the camera), from mip level `lod`.
void EarthColor(const render::EarthImages& images, const render::EarthDraw& earth, const float n[3],
                const float view[3], float lod, float out[3]);

}  // namespace app

#endif
