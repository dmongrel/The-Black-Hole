#version 450

// A credit's text: a rectangle on the plane one unit in front of the camera, fixed to the camera.
// Scaling it about the view axis is the same as bringing it nearer, which is how it flies in.

layout(push_constant) uniform Push {
    vec4 rect;    // left, top, right, bottom; the screen's top edge is y = tan(fov / 2)
    vec4 params;  // scale, alpha, block, aspect ratio
    vec4 more;    // tan(fov / 2), brightness, unused, unused
    vec4 split;   // red x, y, blue x, y: offsets from green, in texture coordinates
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                                    vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
    // Grown by the colour split, so the displaced red and blue images are not cut off.
    vec2 margin = max(abs(pc.split.xy), abs(pc.split.zw));
    vec2 uv     = mix(-margin, 1.0 + margin, corners[gl_VertexIndex]);
    vUV         = uv;

    vec2 p = vec2(mix(pc.rect.x, pc.rect.z, uv.x), mix(pc.rect.y, pc.rect.w, uv.y)) * pc.params.x;
    gl_Position = vec4(p.x / (pc.more.x * pc.params.w), -p.y / pc.more.x, 0.0, 1.0);
}
