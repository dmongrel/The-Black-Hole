#version 450

// A Gargantua-style spinning black hole.
//
// Each pixel's light ray is traced backwards from the camera through Kerr spacetime, the geometry
// around a rotating black hole, as in James, von Tunzelmann, Franklin & Thorne, "Gravitational
// lensing by spinning black holes in astrophysics, and in the movie Interstellar" (2015).
//
// The ray is a null geodesic in Boyer-Lindquist coordinates (r, theta, phi), integrated with
// Hamilton's equations. With the photon's energy fixed at 1 and its axial angular momentum L
// conserved, the super-Hamiltonian is H = N / (2 Sigma) with
//
//     N = Delta p_r^2 + p_theta^2 + (L - a sin^2 theta)^2 / sin^2 theta - (r^2 + a^2 - a L)^2 / Delta
//     Sigma = r^2 + a^2 cos^2 theta,   Delta = r^2 - 2r + a^2
//
// and H = 0 along the ray. Rays that fall through the horizon make the shadow; rays that circle
// near the photon orbits make the thin bright ring; rays that cross the equatorial plane pick up
// light from the disk, which is how the far side of the disk arches over and under the shadow.
// Spin drags the rays round with the hole: the shadow is flattened on the side where the disk
// comes towards the camera, and the disk can reach much further in before its orbits fail.
//
// Units are G = c = M = 1. Spin is the y axis; the disk is the plane y = 0.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform samplerCube uSky;

layout(push_constant) uniform Push {
    vec4 camPos;    // xyz, w = tan(vertical fov / 2)
    vec4 camRight;  // xyz, w = aspect ratio
    vec4 camUp;     // xyz, w = time in seconds
    vec4 camFwd;    // xyz, w unused
} pc;

const float SPIN      = 0.95;  // a / M. The film's Gargantua was 0.999 or so
const float DISK_OUT  = 17.0;
const float ESCAPE_R  = 70.0;  // beyond this, bending is negligible and the ray reads the sky
const int   MAX_STEPS = 900;
const int   MAX_HITS  = 4;     // disk crossings kept per ray; a fifth is inside the photon ring
// Extra softening of the disk's texture, in pixels of footprint: the anti-aliasing alone would
// leave the finest streaks right at the limit of what a pixel can show.
const float DISK_BLUR = 1.15;

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

// Band-limited: `width` is the pixel's footprint in noise space. An octave whose cells are
// smaller than the footprint cannot be shown, only aliased, so it fades to its mean instead.
float fbm(vec3 p, float width) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        float keep = 1.0 - smoothstep(0.4, 0.9, width);
        sum += amp * mix(0.5, vnoise(p), keep);
        p = p * 2.07 + vec3(11.3, 4.1, 7.7);
        amp *= 0.5;
        width *= 2.07;
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

// ---- Kerr geometry ------------------------------------------------------------------------------

// Radius of the innermost stable prograde circular orbit (Bardeen, Press & Teukolsky 1972).
float iscoRadius(float a) {
    float z1 = 1.0 + pow(1.0 - a * a, 1.0 / 3.0) * (pow(1.0 + a, 1.0 / 3.0) + pow(1.0 - a, 1.0 / 3.0));
    float z2 = sqrt(3.0 * a * a + z1 * z1);
    return 3.0 + z2 - sqrt((3.0 - z1) * (3.0 + z1 + 2.0 * z2));
}

// Boyer-Lindquist to Cartesian (oblate spheroidal; spin along y).
vec3 toCartesian(float r, float theta, float phi) {
    float R = sqrt(r * r + SPIN * SPIN);
    return vec3(R * sin(theta) * cos(phi), r * cos(theta), R * sin(theta) * sin(phi));
}

// Hamilton's equations. x = (r, theta, phi), p = (p_r, p_theta); L is conserved.
// The terms proportional to N are dropped from dp: N = 0 on a null ray.
void derivs(vec3 x, vec2 p, float L, out vec3 dx, out vec2 dp) {
    float a  = SPIN;
    float r  = x.x;
    float s  = sin(x.y);
    float c  = cos(x.y);
    float s2 = max(s * s, 1e-9);  // the pole is a coordinate singularity, not a physical one

    float sigma = r * r + a * a * c * c;
    float delta = r * r - 2.0 * r + a * a;
    float B     = r * r + a * a - a * L;

    dx.x = delta * p.x / sigma;
    dx.y = p.y / sigma;
    dx.z = ((L - a * s2) / s2 + a * B / delta) / sigma;

    float dNdr  = (2.0 * r - 2.0) * p.x * p.x - 4.0 * r * B / delta + B * B * (2.0 * r - 2.0) / (delta * delta);
    float dNdth = 2.0 * s * c * (a * a - L * L / (s2 * s2));
    dp = -0.5 * vec2(dNdr, dNdth) / sigma;
}

// One classic fourth-order Runge-Kutta step of length h.
void rk4(vec3 x, vec2 p, float L, float h, out vec3 xo, out vec2 po) {
    vec3 k1x, k2x, k3x, k4x;
    vec2 k1p, k2p, k3p, k4p;
    derivs(x, p, L, k1x, k1p);
    derivs(x + 0.5 * h * k1x, p + 0.5 * h * k1p, L, k2x, k2p);
    derivs(x + 0.5 * h * k2x, p + 0.5 * h * k2p, L, k3x, k3p);
    derivs(x + h * k3x, p + h * k3p, L, k4x, k4p);
    xo = x + h / 6.0 * (k1x + 2.0 * k2x + 2.0 * k3x + k4x);
    po = p + h / 6.0 * (k1p + 2.0 * k2p + 2.0 * k3p + k4p);
}

// ---- accretion disk -----------------------------------------------------------------------------

// Streaky turbulence at a point of the disk, rotating differentially (Keplerian, omega ~ r^-1.5).
// Differential rotation winds any pattern up tighter forever, so two copies half a period apart
// are cross-faded, each reset while it is invisible.
//
// `footprint` is how far r and phi move across one pixel. The winding matters as much as the
// lensing here: late in a copy's life its angle changes with radius many times faster than the
// noise itself does, so the streaks it draws are that much finer radially.
float diskTurbulence(float r, float phi, float time, vec2 footprint) {
    const float PERIOD = 60.0;
    const float SPEED  = 4.0;   // speeds the orbit up; the real one is far too slow to watch
    float t1 = mod(time, PERIOD);
    float t2 = mod(time + 0.5 * PERIOD, PERIOD);
    float w1 = 1.0 - abs(2.0 * t1 / PERIOD - 1.0);

    float omega = SPEED / (pow(r, 1.5) + SPIN);
    float a1 = phi - omega * t1;
    float a2 = phi - omega * t2;
    // Sampled on a cylinder so the pattern has no seam at phi = +-pi. The radial frequency is
    // much higher than the angular one, which is what stretches features into arcs.
    float spin   = r * sqrt(r) + SPIN;
    float dOmega = SPEED * 1.5 * sqrt(r) / (spin * spin);  // -d(omega)/dr
    float w1w    = length(vec2(1.9 * footprint.x, 2.6 * (footprint.y + dOmega * t1 * footprint.x)));
    float w2w    = length(vec2(1.9 * footprint.x, 2.6 * (footprint.y + dOmega * t2 * footprint.x)));
    float n1 = fbm(vec3(r * 1.9, cos(a1) * 2.6, sin(a1) * 2.6), w1w);
    float n2 = fbm(vec3(r * 1.9 + 17.0, cos(a2) * 2.6, sin(a2) * 2.6), w2w);
    return mix(n2, n1, w1);
}

// Light from the disk at Boyer-Lindquist (r, phi) reaching the camera along a ray with angular
// momentum L. rgb is premultiplied by alpha. `footprint` is how far r and phi move across one
// pixel, for filtering the texture.
vec4 shadeDisk(float r, float phi, float L, float diskIn, float time, vec2 footprint) {
    float inner = smoothstep(diskIn, diskIn + 1.0, r);
    float outer = 1.0 - smoothstep(DISK_OUT * 0.55, DISK_OUT, r);
    float edge  = inner * outer;
    if (edge <= 0.0) return vec4(0.0);

    // The rings run at 7 radians per unit of r. Where lensing squeezes the disk, they and the
    // turbulence fade towards their average rather than beating against the pixel grid.
    float turb    = diskTurbulence(r, phi, time, footprint);
    float streaks = smoothstep(0.30, 0.80, turb);
    float ringsAA = 1.0 - smoothstep(0.6, 2.2, 7.0 * footprint.x);
    float rings   = 0.75 + 0.25 * ringsAA * sin(r * 7.0 + turb * 6.0);

    // Frequency shift g = E_observed / E_emitted for gas on a prograde circular Kerr orbit, which
    // folds together the Doppler shift and the gravitational redshift. The ray was traced
    // backwards, so the real photon's angular momentum is -L. The film toned the beaming down
    // so the disk did not look lopsided; so does this, with BEAMING.
    const float BEAMING = 0.45;
    float sr    = sqrt(r);
    float omega = 1.0 / (r * sr + SPIN);
    float ut    = (r * sr + SPIN) / (pow(r, 0.75) * sqrt(max(r * sr - 3.0 * sr + 2.0 * SPIN, 1e-4)));
    float g     = 1.0 / (ut * max(1.0 + omega * L, 0.05));
    g           = mix(1.0, g, BEAMING);

    // Thin-disk temperature falls off roughly as r^-3/4 away from the inner edge.
    float kelvin    = 6800.0 * pow(diskIn / r, 0.75) * g;
    float intensity = 3.2 * pow(diskIn / r, 1.6) * g * g * g;

    vec3  color = blackbody(kelvin) * intensity * (0.30 + 1.2 * streaks) * rings;
    float alpha = edge * mix(0.35, 0.95, streaks) * mix(1.0, 0.55, (r - diskIn) / (DISK_OUT - diskIn));
    return vec4(color * alpha, alpha);
}

// ---- tracing ------------------------------------------------------------------------------------

void main() {
    float time = pc.camUp.w;
    vec2  ndc  = vUV * 2.0 - 1.0;
    vec3  dir  = normalize(pc.camFwd.xyz +
                           ndc.x * pc.camPos.w * pc.camRight.w * pc.camRight.xyz -
                           ndc.y * pc.camPos.w * pc.camUp.xyz);

    const float a       = SPIN;
    float       horizon = 1.0 + sqrt(1.0 - a * a);
    float       diskIn  = iscoRadius(a);

    // Camera position in Boyer-Lindquist coordinates.
    vec3  cam = pc.camPos.xyz;
    float rho2 = dot(cam, cam) - a * a;
    float r    = sqrt(0.5 * (rho2 + sqrt(rho2 * rho2 + 4.0 * a * a * cam.y * cam.y)));
    float th   = acos(clamp(cam.y / r, -1.0, 1.0));
    float ph   = atan(cam.z, cam.x);

    // The ray's direction in the camera's local frame. At the camera's distance the coordinate
    // directions are close enough to the spherical ones.
    vec3  rhat  = normalize(cam);
    float sth   = sqrt(max(1.0 - rhat.y * rhat.y, 1e-8));
    vec3  phhat = vec3(-sin(ph), 0.0, cos(ph));
    vec3  thhat = vec3(rhat.y * cos(ph), -sth, rhat.y * sin(ph));
    vec3  n     = vec3(dot(dir, rhat), dot(dir, thhat), dot(dir, phhat));

    // Momenta for a photon with that direction as seen by a zero-angular-momentum observer (the
    // frame that the spinning hole drags round with it), scaled to unit energy at infinity.
    float s2    = sin(th) * sin(th);
    float sigma = r * r + a * a * cos(th) * cos(th);
    float delta = r * r - 2.0 * r + a * a;
    float Ak    = (r * r + a * a) * (r * r + a * a) - a * a * delta * s2;
    float alpha = sqrt(sigma * delta / Ak);
    float omega = 2.0 * a * r / Ak;
    float varpi = sqrt(Ak / sigma) * sin(th);
    float L     = varpi * n.z / (alpha + varpi * omega * n.z);
    float Ez    = (1.0 - omega * L) / alpha;

    vec3 x = vec3(r, th, ph);
    vec2 p = vec2(sqrt(sigma / delta) * Ez * n.x, sqrt(sigma) * Ez * n.y);

    // Disk crossings are recorded here and shaded after the loop, where the control flow is
    // uniform again and neighbouring pixels' hits can be compared to size each pixel's footprint.
    // The haze is too faint to need the disk's exact opacity: behind each crossing it is dimmed by
    // a typical one instead, which keeps it out of per-crossing storage.
    vec2  hits[MAX_HITS];
    for (int k = 0; k < MAX_HITS; ++k) hits[k] = vec2(0.0);
    vec3  haze      = vec3(0.0);
    float hazeTrans = 1.0;
    int   hitCount  = 0;
    bool  captured = false;
    vec3  prevPos  = cam;
    vec3  pos      = cam;

    int i = 0;
    for (; i < MAX_STEPS; ++i) {
        if (x.x < horizon + 0.02) {
            captured = true;
            break;
        }
        if (x.x > ESCAPE_R && p.x > 0.0) break;

        vec3 k1x, k2x, k3x, k4x;
        vec2 k1p, k2p, k3p, k4p;
        derivs(x, p, L, k1x, k1p);

        // The step is chosen from how fast each coordinate is moving, not from r alone: near the
        // pole theta and phi swing hard for rays with small L, and near the horizon frame
        // dragging spins phi. Letting any of them jump turns the integration to noise there.
        float h = clamp(0.05 * x.x, 0.01, 2.5);
        // Near the axis a ray with small L bounces off the L^2 / sin^2 theta barrier, and the
        // bounce has to be resolved or p_theta blows up: hence theta and p_theta both limited.
        h = min(h, 0.1 * max(abs(sin(x.y)), 0.002) / max(abs(k1x.y), 1e-6));
        h = min(h, 0.2 * (abs(p.y) + 0.05 * x.x) / max(abs(k1p.y), 1e-6));
        // phi by the distance it moves the ray, not the angle: at the pole phi spins without
        // the ray going anywhere, and limiting the angle there only starves the ray of steps.
        h = min(h, 0.12 / max(abs(k1x.z * sin(x.y)), 1e-6));
        h = min(h, 0.2 * (x.x - horizon) / max(abs(k1x.x), 1e-6));
        h = max(h, 1e-4);

        // Classic fourth-order Runge-Kutta.
        derivs(x + 0.5 * h * k1x, p + 0.5 * h * k1p, L, k2x, k2p);
        derivs(x + 0.5 * h * k2x, p + 0.5 * h * k2p, L, k3x, k3p);
        derivs(x + h * k3x, p + h * k3p, L, k4x, k4p);
        vec3 xn = x + h / 6.0 * (k1x + 2.0 * k2x + 2.0 * k3x + k4x);
        vec2 pn = p + h / 6.0 * (k1p + 2.0 * k2p + 2.0 * k3p + k4p);

        // Crossing the equatorial plane: the disk is infinitely thin.
        float c0 = cos(x.y);
        float c1 = cos(xn.y);
        if (c0 * c1 < 0.0 && abs(c0) < 0.3 && abs(c1) < 0.3) {
            // Where exactly: a straight line between the step's ends is off by an error that jumps
            // as neighbouring rays take different numbers of steps, which the disk's fine
            // streaks turn into a moire. So step exactly to the line's estimate, then take one
            // Newton step onto the plane from there.
            vec3 xm, dxm;
            vec2 pm, dpm;
            rk4(x, p, L, h * c0 / (c0 - c1), xm, pm);
            derivs(xm, pm, L, dxm, dpm);
            float dl = cos(xm.y) / max(abs(sin(xm.y) * dxm.y), 1e-6) * sign(dxm.y);
            dl       = clamp(dl, -h, h);
            float rh = xm.x + dxm.x * dl;
            if (rh > diskIn && rh < DISK_OUT) {
                hits[hitCount++] = vec2(rh, xm.z + dxm.z * dl);
                hazeTrans *= 0.4;
                // Behind this many layers of disk the sky, and any further image, is hidden.
                if (hitCount == MAX_HITS) {
                    x = xn;
                    break;
                }
            }
        }

        prevPos = pos;
        pos     = toCartesian(xn.x, xn.y, xn.z);

        // A faint warm haze hugging the disk plane: stands in for the glow around Gargantua's disk
        // and gets lensed along with everything else.
        float rxz  = length(pos.xz);
        float glow = exp(-abs(pos.y) * 1.4) * smoothstep(DISK_OUT, diskIn, rxz) * smoothstep(diskIn * 0.8, diskIn * 1.3, rxz);
        haze += hazeTrans * glow * min(h, 2.5) * 0.010 * vec3(1.0, 0.72, 0.45);

        x = xn;
        p = pn;
    }

    // A ray still close in when the steps run out is circling a photon orbit: it belongs to the
    // shadow's edge, and would otherwise read the sky in an arbitrary direction.
    if (i == MAX_STEPS && x.x < 5.0) captured = true;

    // The escaping ray's direction, from its last step: out here it is all but straight.
    // Sampled outside any branch so the implicit mip selection sees neighbouring pixels' final
    // directions: where lensing squeezes a wide patch of sky into a pixel, it reads a coarser mip
    // instead of sparkling.
    vec3 away = pos - prevPos;
    vec3 sky  = texture(uSky, normalize(dot(away, away) > 0.0 ? away : dir)).rgb;

    // Composite front to back. Each hit's footprint is how far its (r, phi) moves to the
    // neighbouring pixels' hit of the same order. Where a neighbour has no such hit (the edge of
    // an image) the difference is meaningless, so it is capped: that pixel is merely softened.
    vec3  color = haze;
    float trans = 1.0;   // how much of what lies further along the ray still reaches the camera
    const float TWO_PI = 6.2831853;
    for (int k = 0; k < MAX_HITS; ++k) {
        // A neighbour without a hit of this order has nothing to compare: that axis is dropped.
        float present = k < hitCount ? 1.0 : 0.0;
        vec2  dx      = abs(dFdx(present)) < 0.5 ? dFdx(hits[k]) : vec2(0.0);
        vec2  dy      = abs(dFdy(present)) < 0.5 ? dFdy(hits[k]) : vec2(0.0);
        // Rays either side of the spin axis wind round it opposite ways, so their phi can differ
        // by whole turns for neighbouring points of the disk.
        dx.y -= TWO_PI * round(dx.y / TWO_PI);
        dy.y -= TWO_PI * round(dy.y / TWO_PI);
        vec2 footprint = min(sqrt(dx * dx + dy * dy) * DISK_BLUR, vec2(3.0, 1.0));
        if (k < hitCount) {
            vec4 d;
            if (footprint.x < 0.08) {
                d = shadeDisk(hits[k].x, hits[k].y, L, diskIn, time, footprint);
            } else {
                // Where lensing squeezes the disk hard (the thin arcs hugging the shadow) its
                // whole radial profile, inner edge and all, falls inside a few pixels, and
                // filtering the texture alone leaves stair-steps. Four samples across the
                // footprint on a rotated grid average the profile itself.
                const vec2 grid[4] = vec2[4](vec2(-0.375, -0.125), vec2(0.125, -0.375),
                                             vec2(0.375, 0.125), vec2(-0.125, 0.375));
                d = vec4(0.0);
                for (int j = 0; j < 4; ++j) {
                    vec2 at = hits[k] + grid[j] * footprint;
                    d += shadeDisk(at.x, at.y, L, diskIn, time, footprint * 0.5);
                }
                d *= 0.25;
            }
            color += trans * d.rgb;
            trans *= 1.0 - d.a;
        }
    }
    // Past a full stack of hits nothing further shows.
    if (hitCount < MAX_HITS && !captured) color += trans * sky;

    // Linear HDR: bloom and tone mapping follow in their own passes.
    outColor = vec4(color, 1.0);
}
