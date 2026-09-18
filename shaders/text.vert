#version 450

// A credit's text: a rectangle on the plane one unit in front of the camera, fixed to the camera.
// Scaling it about the view axis is the same as bringing it nearer, which is how it flies in.

layout(push_constant) uniform Push {
    vec4 rect;    // left, top, right, bottom; the screen's top edge is y = tan(fov / 2)
    vec4 params;  // scale, alpha, sweep, aspect ratio
    vec4 more;    // tan(fov / 2), brightness, unused, unused
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                                    vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
    vec2 c = corners[gl_VertexIndex];
    vUV    = c;

    vec2 p = vec2(mix(pc.rect.x, pc.rect.z, c.x), mix(pc.rect.y, pc.rect.w, c.y)) * pc.params.x;
    gl_Position = vec4(p.x / (pc.more.x * pc.params.w), -p.y / pc.more.x, 0.0, 1.0);
}
