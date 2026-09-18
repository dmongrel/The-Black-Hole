#version 450

// The last pass: the traced HDR image plus its bloom, exposed, tone mapped, and encoded for the
// swapchain.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uHdr;
layout(set = 0, binding = 1) uniform sampler2D uBloom;  // the chain's first level, half size

layout(push_constant) uniform Push {
    float bloom;        // how much of the chain is added back
    float exposure;
    float manualGamma;  // 1 when the target is UNORM and needs sRGB encoding here
    float pad;
} pc;

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 srgbEncode(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
    // Added before the exposure rather than after, so the glow tone-maps with the light it came
    // from instead of washing out the black around the hole.
    vec3 hdr   = texture(uHdr, vUV).rgb + texture(uBloom, vUV).rgb * pc.bloom;
    vec3 color = aces(hdr * pc.exposure);
    if (pc.manualGamma > 0.5) color = srgbEncode(color);
    outColor = vec4(color, 1.0);
}
