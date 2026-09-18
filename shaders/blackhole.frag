#version 450

// A Gargantua-style black hole.
//
// Each pixel's light ray is traced backwards from the camera through Schwarzschild spacetime.
// A photon's path obeys the orbit equation u'' + u = 3M u^2 (u = 1/r), which is the same as a
// Newtonian trajectory under the extra acceleration a = -(3/2) r_s h^2 r / |r|^5, where h is the
// photon's conserved angular momentum. Integrating that bends the ray around the hole: rays with
// small impact parameters fall through the horizon (the shadow), grazing ones wrap around the
// photon sphere (the thin bright ring), and rays that cross the equatorial plane pick up light
// from the accretion disk. That is how the far side of the disk appears arched over the top and
// under the bottom of the shadow, as in the film and in James, von Tunzelmann, Franklin & Thorne,
// "Gravitational lensing by spinning black holes in astrophysics, and in the movie Interstellar"
// (2015). This is the non-spinning case; Gargantua spins, which mostly flattens one side of the
// shadow.
//
// Units are G = c = M = 1, so the horizon is at r = 2 and the photon sphere at r = 3.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform samplerCube uSky;

layout(push_constant) uniform Push {
    vec4 camPos;    // xyz, w = tan(vertical fov / 2)
    vec4 camRight;  // xyz, w = aspect ratio
    vec4 camUp;     // xyz, w = time in seconds
    vec4 camFwd;    // xyz, w = 1 when the target is UNORM and needs sRGB encoding here
} pc;

const float RS        = 2.0;   // Schwarzschild radius
const float DISK_IN   = 4.5;   // inner edge; a little inside the r = 6 ISCO, for the look
const float DISK_OUT  = 17.0;
const float ESCAPE_R  = 70.0;  // beyond this, bending is negligible and the ray reads the sky
const int   MAX_STEPS = 500;

// ---- noise --------------------------------------------------------------------------------------

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float vnoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash13(i + vec3(0, 0, 0)), hash13(i + vec3(1, 0, 0)), f.x),
                   mix(hash13(i + vec3(0, 1, 0)), hash13(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash13(i + vec3(0, 0, 1)), hash13(i + vec3(1, 0, 1)), f.x),
                   mix(hash13(i + vec3(0, 1, 1)), hash13(i + vec3(1, 1, 1)), f.x), f.y),
               f.z);
}

float fbm(vec3 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += amp * vnoise(p);
        p = p * 2.07 + vec3(11.3, 4.1, 7.7);
        amp *= 0.5;
    }
    return sum / 0.9375;
}

// ---- colour -------------------------------------------------------------------------------------

// Approximate linear RGB of a black body, brightest channel 1.
vec3 blackbody(float kelvin) {
    float t = clamp(kelvin, 1000.0, 40000.0) / 100.0;
    float r = t <= 66.0 ? 1.0 : 1.292936 * pow(t - 60.0, -0.1332047);
    float g = t <= 66.0 ? 0.3900816 * log(t) - 0.6318414 : 1.1298909 * pow(t - 60.0, -0.0755148);
    float b = t >= 66.0 ? 1.0 : (t <= 19.0 ? 0.0 : 0.5432068 * log(t - 10.0) - 1.1962541);
    return pow(clamp(vec3(r, g, b), 0.0, 1.0), vec3(2.2));
}

vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

vec3 srgbEncode(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

// ---- accretion disk -----------------------------------------------------------------------------

// Streaky turbulence at a point of the disk, rotating differentially (Keplerian, omega ~ r^-1.5).
// Differential rotation winds any pattern up tighter forever, so two copies half a period apart
// are cross-faded, each reset while it is invisible.
float diskTurbulence(float r, float phi, float time) {
    const float PERIOD = 60.0;
    const float SPIN   = 4.0;   // speeds the orbit up; the real one at r = 6 is far too slow to see
    float t1 = mod(time, PERIOD);
    float t2 = mod(time + 0.5 * PERIOD, PERIOD);
    float w1 = 1.0 - abs(2.0 * t1 / PERIOD - 1.0);

    float omega = SPIN * pow(r, -1.5);
    float a1 = phi - omega * t1;
    float a2 = phi - omega * t2;
    // Sampled on a cylinder so the pattern has no seam at phi = +-pi. The radial frequency is
    // much higher than the angular one, which is what stretches features into arcs.
    float n1 = fbm(vec3(r * 1.9, cos(a1) * 2.6, sin(a1) * 2.6));
    float n2 = fbm(vec3(r * 1.9 + 17.0, cos(a2) * 2.6, sin(a2) * 2.6));
    return mix(n2, n1, w1);
}

// Light emitted towards the camera by the disk at `hit`, seen along `dir`. rgb is premultiplied
// by alpha.
vec4 shadeDisk(vec3 hit, vec3 dir, float time) {
    float r   = length(hit.xz);
    float phi = atan(hit.z, hit.x);

    float inner = smoothstep(DISK_IN, DISK_IN + 1.2, r);
    float outer = 1.0 - smoothstep(DISK_OUT * 0.55, DISK_OUT, r);
    float edge  = inner * outer;
    if (edge <= 0.0) return vec4(0.0);

    float turb    = diskTurbulence(r, phi, time);
    float streaks = smoothstep(0.30, 0.80, turb);
    float rings   = 0.75 + 0.25 * sin(r * 7.0 + turb * 6.0);

    // Relativistic Doppler shift and beaming. The gas orbits at beta = sqrt(M / (r - 2M)) as seen
    // by a static observer, and g combines that with the gravitational redshift. The film toned
    // the beaming down so the disk did not look lopsided; so does this, with BEAMING.
    const float BEAMING = 0.45;
    vec3  vel   = normalize(vec3(-hit.z, 0.0, hit.x));
    float beta  = sqrt(1.0 / max(r - RS, 0.5));
    beta        = min(beta, 0.8);
    float gamma = inversesqrt(1.0 - beta * beta);
    float g     = sqrt(1.0 - RS / r) / (gamma * (1.0 - beta * dot(vel, -dir)));
    g           = mix(1.0, g, BEAMING);

    // Thin-disk temperature falls off roughly as r^-3/4.
    float kelvin    = 6800.0 * pow(DISK_IN / r, 0.75) * g;
    float intensity = 3.2 * pow(DISK_IN / r, 1.6) * g * g * g;

    vec3  color = blackbody(kelvin) * intensity * (0.30 + 1.2 * streaks) * rings;
    float alpha = edge * mix(0.35, 0.95, streaks) * mix(1.0, 0.55, (r - DISK_IN) / (DISK_OUT - DISK_IN));
    return vec4(color * alpha, alpha);
}

// ---- tracing ------------------------------------------------------------------------------------

vec3 accel(vec3 p, float h2) {
    float r2 = dot(p, p);
    return -1.5 * RS * h2 * p / (r2 * r2 * sqrt(r2));
}

void main() {
    float time = pc.camUp.w;
    vec2  ndc  = vUV * 2.0 - 1.0;
    vec3  dir  = normalize(pc.camFwd.xyz +
                           ndc.x * pc.camPos.w * pc.camRight.w * pc.camRight.xyz -
                           ndc.y * pc.camPos.w * pc.camUp.xyz);

    vec3  p  = pc.camPos.xyz;
    vec3  v  = dir;
    vec3  L  = cross(p, v);
    float h2 = dot(L, L);

    vec3  color    = vec3(0.0);
    float trans    = 1.0;   // how much of what lies further along the ray still reaches the camera
    bool  captured = false;

    for (int i = 0; i < MAX_STEPS; ++i) {
        float r = length(p);
        if (r < RS * 1.02) {
            captured = true;
            break;
        }
        if (r > ESCAPE_R && dot(p, v) > 0.0) break;

        // Steps shrink near the hole, where the path curves hardest.
        float dt = clamp(0.07 * (r - RS * 0.9), 0.02, 2.5);

        // Velocity Verlet.
        vec3 a0 = accel(p, h2);
        vec3 pn = p + v * dt + 0.5 * a0 * dt * dt;
        vec3 vn = v + 0.5 * (a0 + accel(pn, h2)) * dt;

        // Crossing the equatorial plane: the disk is infinitely thin.
        if (p.y * pn.y < 0.0) {
            float f   = p.y / (p.y - pn.y);
            vec3  hit = mix(p, pn, f);
            float rh  = length(hit.xz);
            if (rh > DISK_IN && rh < DISK_OUT) {
                vec4 d = shadeDisk(hit, normalize(mix(v, vn, f)), time);
                color += trans * d.rgb;
                trans *= 1.0 - d.a;
                if (trans < 0.005) break;
            }
        }

        // A faint warm haze hugging the disk plane: stands in for the glow around Gargantua's disk
        // and gets lensed along with everything else.
        float rxz  = length(pn.xz);
        float haze = exp(-abs(pn.y) * 1.4) * smoothstep(DISK_OUT, DISK_IN, rxz) * smoothstep(DISK_IN * 0.8, DISK_IN * 1.3, rxz);
        color += trans * haze * dt * 0.010 * vec3(1.0, 0.72, 0.45);

        p = pn;
        v = vn;
    }

    // Sampled outside any branch so the implicit mip selection sees neighbouring pixels' final
    // directions: where lensing squeezes a wide patch of sky into a pixel, it reads a coarser mip
    // instead of sparkling.
    vec3 sky = texture(uSky, normalize(v)).rgb;
    if (!captured) color += trans * sky;

    color = aces(color * 1.1);
    if (pc.camFwd.w > 0.5) color = srgbEncode(color);
    outColor = vec4(color, 1.0);
}
