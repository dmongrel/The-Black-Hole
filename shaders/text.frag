#version 450

// The glyphs, in the warm white of film titles, added to the HDR image before bloom so they
// glow a little with everything else. Left of the sweep they have broken into particles.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uText;

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 params;  // scale, alpha, sweep, aspect ratio
    vec4 more;    // tan(fov / 2), brightness, unused, unused
} pc;

const vec3 WARM_WHITE = vec3(1.0, 0.93, 0.82);

void main() {
    float coverage = texture(uText, vUV).r;
    float kept     = smoothstep(pc.params.z, pc.params.z + 0.015, vUV.x);
    outColor = vec4(WARM_WHITE * (coverage * kept * pc.params.y * pc.more.y), 1.0);
}
