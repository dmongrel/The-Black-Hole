#version 450

// A soft round spot, added to the HDR image.

layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec3 vColor;
layout(location = 0) out vec4 outColor;

void main() {
    float falloff = exp(-3.0 * dot(vCorner, vCorner));
    outColor = vec4(vColor * falloff, 1.0);
}
