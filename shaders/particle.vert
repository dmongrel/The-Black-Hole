#version 450

// One particle of a broken-up credit at a world position, drawn as two triangles facing the
// camera: a flat square block of the pixelated text at first, softening into a round glow.
//
// The ray tracer bends every pixel's light through the hole's spacetime; that is far too costly
// per particle, so the particle's direction is bent with the point-lens approximation instead.
// For a source a distance s behind the hole (camera at distance D), at angle beta from the hole,
// the primary image is at theta = (beta + sqrt(beta^2 + 4 thetaE^2)) / 2 with
// thetaE^2 = 4 s / (D (D + s)) in units G = c = M = 1. A particle whose image would fall inside
// the shadow cannot be seen at all. Not exact in the strong field by the inner disk, but the
// stream arches round the hole the way the disk does.

layout(location = 0) in vec4 inPositionSize;  // world xyz, radius in pixels
layout(location = 1) in vec4 inLight;         // brightness, heat, square, unused

layout(push_constant) uniform Push {
    vec4 camPos;    // xyz, w = tan(vertical fov / 2)
    vec4 camRight;  // xyz, w = aspect ratio
    vec4 camUp;     // xyz, w unused
    vec4 camFwd;    // xyz, w = viewport height in pixels
} pc;

layout(location = 0) out vec2 vCorner;
layout(location = 1) out vec3 vColor;
layout(location = 2) out float vSquare;

const float SHADOW_B   = 5.2;   // critical impact parameter, about 3 sqrt(3) M
const vec3  WARM_WHITE = vec3(1.0, 0.93, 0.82);
const vec3  DISK_GLOW  = vec3(1.0, 0.62, 0.32);

void main() {
    const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),
                                    vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));
    vCorner = corners[gl_VertexIndex];
    vSquare = inLight.z;

    vec3  cam  = pc.camPos.xyz;
    vec3  d    = inPositionSize.xyz - cam;
    float dist = length(d);
    vec3  dir  = d / dist;

    // Lensing, for a particle behind the hole.
    float D       = length(cam);
    vec3  toHole  = -cam / D;
    float cosBeta = clamp(dot(dir, toHole), -1.0, 1.0);
    float s       = dot(inPositionSize.xyz, toHole);  // how far beyond the hole, along the sight line
    float visible = 1.0;
    if (s > 0.0 && cosBeta > 0.0) {
        float beta    = acos(cosBeta);
        float thetaE2 = 4.0 * s / (D * (D + s));
        float theta   = 0.5 * (beta + sqrt(beta * beta + 4.0 * thetaE2));
        vec3  across  = dir - toHole * cosBeta;
        float len     = length(across);
        across        = len > 1e-6 ? across / len : pc.camUp.xyz;
        dir           = toHole * cos(theta) + across * sin(theta);
        float shadow  = asin(min(SHADOW_B / D, 1.0));
        visible       = smoothstep(shadow * 0.97, shadow * 1.05, theta);
    }

    float tanHalf = pc.camPos.w;
    float aspect  = pc.camRight.w;
    float height  = pc.camFwd.w;
    float z       = dot(dir, pc.camFwd.xyz);
    if (z < 0.02) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);  // behind the camera: outside the clip volume
        vColor      = vec3(0.0);
        return;
    }
    vec2 ndc = vec2(dot(dir, pc.camRight.xyz) / (z * tanHalf * aspect), -dot(dir, pc.camUp.xyz) / (z * tanHalf));

    // Below about a pixel a particle is drawn at a pixel with its light spread over it, so it dims
    // as it shrinks instead of flickering between pixels.
    float radius = inPositionSize.w;
    float drawn  = max(radius, 1.2);
    float energy = (radius / drawn) * (radius / drawn);

    ndc += vCorner * drawn * 2.0 / vec2(height * aspect, height);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vColor      = mix(WARM_WHITE, DISK_GLOW, inLight.y) * (inLight.x * energy * visible);
}
