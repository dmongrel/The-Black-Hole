#version 450

// The Earth, which one credit brings on: a sphere ray-cast at every pixel of the full-screen
// triangle, drawn over the black hole. It is placed well clear of the hole, where the bending of
// light is slight, so it is not lensed.
//
// Its light is the accretion disk's, so the side facing the hole is lit and the far side is
// night, where the cities show. The shading here must match EarthColor in src/app/earth.cpp,
// which colours the particles it breaks into, block for block, when pixelated.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uDay;    // surface colour, sRGB-encoded
layout(set = 0, binding = 1) uniform sampler2D uNight;  // city lights

layout(push_constant) uniform Push {
    vec4 camPos;    // xyz, w = tan(vertical fov / 2)
    vec4 camRight;  // xyz, w = aspect ratio
    vec4 camUp;     // xyz, w unused
    vec4 camFwd;    // xyz, w = viewport height in pixels
    vec4 center;    // xyz, w = radius
    vec4 north;     // xyz, w = spin, radians
    vec4 east;      // xyz, w = block size in pixels (1: not pixelated)
    vec4 light;     // xyz, towards the light; w = opacity
} pc;

const float PI    = 3.14159265;
const vec3  SUN   = vec3(1.0, 0.88, 0.74);   // the disk's warm light
const vec3  CITY  = vec3(1.0, 0.72, 0.42);   // sodium lamps
const vec3  AIR   = vec3(0.35, 0.6, 1.0);    // the atmosphere at the limb

vec3 Shade(vec3 n, vec3 view, float lod) {
    vec3  across = cross(pc.east.xyz, pc.north.xyz);
    float lon    = atan(dot(n, across), dot(n, pc.east.xyz)) + pc.north.w;
    float lat    = asin(clamp(dot(n, pc.north.xyz), -1.0, 1.0));
    vec2  uv     = vec2(fract(lon / (2.0 * PI) + 0.5), 0.5 - lat / PI);

    vec3  albedo = pow(textureLod(uDay, uv, lod).rgb, vec3(2.2));
    float city   = textureLod(uNight, uv, lod).r;  // linear: a city is a speck, and the mips average it away
    float ndl    = dot(n, pc.light.xyz);

    vec3 color = albedo * SUN * (1.6 * max(ndl, 0.0));
    color += CITY * (6.0 * city * (1.0 - smoothstep(-0.12, 0.08, ndl)));
    float rim = pow(1.0 - clamp(dot(n, view), 0.0, 1.0), 3.0);
    color += AIR * (0.9 * rim * (0.05 + 0.95 * smoothstep(-0.25, 0.45, ndl)));
    return color;
}

void main() {
    float tanHalf = pc.camPos.w;
    float aspect  = pc.camRight.w;
    float height  = pc.camFwd.w;
    float block   = pc.east.w;

    // Pixelated, every pixel of a block shows the block's centre, as its particle will.
    vec2 pixel = gl_FragCoord.xy;
    if (block > 1.0) pixel = (floor(gl_FragCoord.xy / block) + 0.5) * block;
    vec2 ndc = pixel / vec2(height * aspect, height) * 2.0 - 1.0;

    // Drawn as though looked at straight on, so it stays round anywhere in the frame, where a
    // wide view would stretch it towards the edges: a pixel's offset from its projected centre,
    // in the image plane, is taken as an offset from the direction straight to it.
    vec3  oc    = pc.center.xyz - pc.camPos.xyz;
    float depth = dot(oc, pc.camFwd.xyz);
    if (depth <= 0.0) discard;
    vec2 offset = vec2(ndc.x * tanHalf * aspect, -ndc.y * tanHalf) - vec2(dot(oc, pc.camRight.xyz), dot(oc, pc.camUp.xyz)) / depth;
    vec3 e      = normalize(oc);
    vec3 r      = normalize(pc.camRight.xyz - e * dot(pc.camRight.xyz, e));
    vec3 u      = normalize(pc.camUp.xyz - e * dot(pc.camUp.xyz, e) - r * dot(pc.camUp.xyz, r));
    vec3 d      = normalize(e + r * offset.x + u * offset.y);

    float along  = dot(oc, d);
    vec3  perpV  = oc - d * along;  // from the ray's closest approach to the centre
    float perp   = length(perpV);
    float radius = pc.center.w;
    if (along <= 0.0) discard;

    // Anti-aliased at the limb; pixelated, a block is in or out.
    float pixelAngle = 2.0 * tanHalf / height;
    float cover      = block > 1.0 ? float(perp < radius) : clamp((radius - perp) / (along * pixelAngle) + 0.5, 0.0, 1.0);
    if (cover <= 0.0) discard;

    // At the limb, the edge pixels are shaded as the limb itself.
    float p   = min(perp, radius * 0.9995);
    vec3  out_ = perp > 1e-6 ? perpV / perp : vec3(0.0);
    vec3  hit = pc.center.xyz - out_ * p - d * sqrt(radius * radius - p * p);
    vec3  n   = (hit - pc.center.xyz) / radius;

    // The mip level for the stretch of surface one pixel (or block) spans, across its slope.
    float texels = float(textureSize(uDay, 0).x) / (2.0 * PI);
    float span   = along * pixelAngle * max(block, 1.0) / (radius * max(dot(n, -d), 0.25));
    float lod    = log2(max(span * texels, 1.0));

    outColor = vec4(Shade(n, -d, lod), cover * pc.light.w);
}
