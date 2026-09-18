#version 450

// The glyphs, in the warm white of film titles, added to the HDR image before bloom so they
// glow a little with everything else. As a credit breaks up it is drawn pixelated: each block of
// texels shows their average, the same blocks, block for block, that then become particles.
// Just before that it shakes, and its red, green and blue come apart: three images of it.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uText;

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 params;  // scale, alpha, block, aspect ratio
    vec4 more;    // tan(fov / 2), brightness, unused, unused
    vec4 split;   // red x, y, blue x, y: offsets from green, in texture coordinates
} pc;

// Outside the image there is no text (the sampler would repeat its edge).
float Coverage(vec2 uv) {
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;
    return texture(uText, uv).r;
}

const vec3 WARM_WHITE = vec3(1.0, 0.93, 0.82);

void main() {
    vec3 coverage;
    int  block = int(pc.params.z);
    if (block <= 1) {
        coverage = vec3(Coverage(vUV - pc.split.xy), Coverage(vUV), Coverage(vUV - pc.split.zw));
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
        coverage = vec3(sum / float(block * block));
    }
    outColor = vec4(WARM_WHITE * (coverage * pc.params.y * pc.more.y), 1.0);
}
