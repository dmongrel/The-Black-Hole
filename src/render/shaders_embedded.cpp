#include "render/shaders_embedded.h"

#include <cstring>

extern "C" const ShaderBlob* shader_blob(const char* name) {
    if (!name) return nullptr;
    for (unsigned i = 0; i < g_shader_count; ++i) {
        if (std::strcmp(g_shaders[i].name, name) == 0) return &g_shaders[i];
    }
    return nullptr;
}
