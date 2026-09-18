#version 450

// The glyphs, in the warm white of film titles, added to the HDR image before bloom so they
// glow a little with everything else. As a credit breaks up it is drawn pixelated: each block of
// texels shows their average, the same blocks, block for block, that then become particles.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uText;

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 params;  // scale, alpha, block, aspect ratio
    vec4 more;    // tan(fov / 2), brightness, unused, unused
} pc;

const vec3 WARM_WHITE = vec3(1.0, 0.93, 0.82);

void main() {
    float coverage;
    int   block = int(pc.params.z);
    if (block <= 1) {
        coverage = texture(uText, vUV).r;
    } else {
        ivec2 size   = textureSize(uText, 0);
        ivec2 origin = (ivec2(vUV * vec2(size)) / block) * block;
        float sum    = 0.0;
        for (int y = 0; y < block; ++y) {
            for (int x = 0; x < block; ++x) {
                ivec2 t = origin + ivec2(x, y);
                if (t.x < size.x && t.y < size.y) sum += texelFetch(uText, t, 0).r;
            }
        }
        coverage = sum / float(block * block);
    }
    outColor = vec4(WARM_WHITE * (coverage * pc.params.y * pc.more.y), 1.0);
}
