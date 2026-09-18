// The space around the black hole: a star field with faint nebulous cloud, baked once at start-up
// into a cubemap. The black hole shader bends rays and then looks this up, so lensing of the
// background comes for free.
#ifndef BLACK_HOLE_SKYBOX_H
#define BLACK_HOLE_SKYBOX_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace render {

// E5B9G9R9 shared-exponent HDR texels (4 bytes each, and always filterable) with a full mip
// chain. Stored level by level; within a level, faces in Vulkan order +X -X +Y -Y +Z -Z.
struct SkyboxImage {
    int                   size   = 0;  // texels along one face edge at level 0
    int                   levels = 0;
    std::vector<size_t>   levelOffset;  // index into texels of each level's first face
    std::vector<uint32_t> texels;
};

SkyboxImage GenerateSkybox(int size, uint32_t seed);

}  // namespace render

#endif
