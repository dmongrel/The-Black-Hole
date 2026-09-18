#include "render/skybox.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <thread>

namespace render {
namespace {

struct V3 {
    float x, y, z;
};
V3    operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3    operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3    Normalize(V3 a) { return a * (1.0f / std::sqrt(Dot(a, a))); }
V3    Cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float Smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
V3 Mix(V3 a, V3 b, float t) { return a * (1.0f - t) + b * t; }

// ---- value noise ----------------------------------------------------------------------------

float Lattice(int x, int y, int z, uint32_t seed) {
    uint32_t h = seed ^ (static_cast<uint32_t>(x) * 0x8da6b343u) ^
                 (static_cast<uint32_t>(y) * 0xd8163841u) ^ (static_cast<uint32_t>(z) * 0xcb1ab31fu);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<float>(h) * (1.0f / 4294967295.0f);
}

float Noise(V3 p, uint32_t seed) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int   x = static_cast<int>(fx), y = static_cast<int>(fy), z = static_cast<int>(fz);
    float       tx = p.x - fx, ty = p.y - fy, tz = p.z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    tz = tz * tz * (3.0f - 2.0f * tz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(Lattice(x, y, z, seed), Lattice(x + 1, y, z, seed), tx);
    const float x10 = lerp(Lattice(x, y + 1, z, seed), Lattice(x + 1, y + 1, z, seed), tx);
    const float x01 = lerp(Lattice(x, y, z + 1, seed), Lattice(x + 1, y, z + 1, seed), tx);
    const float x11 = lerp(Lattice(x, y + 1, z + 1, seed), Lattice(x + 1, y + 1, z + 1, seed), tx);
    return lerp(lerp(x00, x10, ty), lerp(x01, x11, ty), tz);
}

float Fbm(V3 p, int octaves, uint32_t seed) {
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * Noise(p, seed + static_cast<uint32_t>(i) * 1013u);
        norm += amp;
        p   = p * 2.03f + V3{5.2f, 1.3f, 7.9f};
        amp *= 0.5f;
    }
    return sum / norm;
}

// ---- colour and packing ---------------------------------------------------------------------

// Approximate linear RGB of a black body at `kelvin`, normalised so the brightest channel is 1.
V3 Blackbody(float kelvin) {
    const float t = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
    float r = t <= 66.0f ? 1.0f : 1.292936f * std::pow(t - 60.0f, -0.1332047f);
    float g = t <= 66.0f ? 0.3900816f * std::log(t) - 0.6318414f
                         : 1.1298909f * std::pow(t - 60.0f, -0.0755148f);
    float b = t >= 66.0f ? 1.0f : (t <= 19.0f ? 0.0f : 0.5432068f * std::log(t - 10.0f) - 1.1962541f);
    auto lin = [](float c) { return std::pow(std::clamp(c, 0.0f, 1.0f), 2.2f); };
    return {lin(r), lin(g), lin(b)};
}

// Packs linear RGB into VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, following the Vulkan spec's recipe.
uint32_t PackE5B9G9R9(float r, float g, float b) {
    constexpr int   N = 9, B = 15;
    constexpr float kMax = 65408.0f;  // (2^9 - 1) / 2^9 * 2^(31 - 15)
    r = std::clamp(r, 0.0f, kMax);
    g = std::clamp(g, 0.0f, kMax);
    b = std::clamp(b, 0.0f, kMax);
    const float maxc = std::max({r, g, b});
    if (maxc <= 0.0f) return 0;

    int   e     = std::max(-B - 1, static_cast<int>(std::floor(std::log2(maxc)))) + 1 + B;
    float denom = std::exp2(static_cast<float>(e - B - N));
    if (std::floor(maxc / denom + 0.5f) >= static_cast<float>(1 << N)) {
        denom *= 2.0f;
        ++e;
    }
    auto m = [&](float c) { return static_cast<uint32_t>(std::floor(c / denom + 0.5f)); };
    return m(r) | (m(g) << 9) | (m(b) << 18) | (static_cast<uint32_t>(e) << 27);
}

// ---- cube mapping ---------------------------------------------------------------------------

// Direction through the centre of texel (s, t), both in [-1, 1], on `face`.
V3 FaceDirection(int face, float s, float t) {
    switch (face) {
        case 0: return {1.0f, -t, -s};
        case 1: return {-1.0f, -t, s};
        case 2: return {s, 1.0f, t};
        case 3: return {s, -1.0f, -t};
        case 4: return {s, -t, 1.0f};
        default: return {-s, -t, -1.0f};
    }
}

// Inverse of FaceDirection.
void DirectionToFace(V3 d, int& face, float& s, float& t) {
    const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    if (ax >= ay && ax >= az) {
        face = d.x > 0 ? 0 : 1;
        s    = (d.x > 0 ? -d.z : d.z) / ax;
        t    = -d.y / ax;
    } else if (ay >= az) {
        face = d.y > 0 ? 2 : 3;
        s    = d.x / ay;
        t    = (d.y > 0 ? d.z : -d.z) / ay;
    } else {
        face = d.z > 0 ? 4 : 5;
        s    = (d.z > 0 ? d.x : -d.x) / az;
        t    = -d.y / az;
    }
}

// ---- content --------------------------------------------------------------------------------

// The galactic plane: the band where stars crowd together and the dust lies.
const V3 kBandNormal = Normalize({0.30f, 0.86f, 0.41f});

// Faint nebulous cloud. Deliberately dim: after tone mapping it should read as depth in the
// black, not as a picture of a nebula.
V3 Nebula(V3 d, uint32_t seed) {
    const float lat  = Dot(d, kBandNormal);
    const float band = std::exp(-(lat * lat) / (2.0f * 0.16f * 0.16f));

    const float large  = Fbm(d * 1.8f, 5, seed + 11);
    const float detail = Fbm(d * 6.0f, 4, seed + 23);
    const float hue    = Fbm(d * 1.1f + V3{3.1f, 0.0f, 1.7f}, 3, seed + 37);
    const float dust   = Smoothstep(0.50f, 0.72f, Fbm(d * 4.5f, 5, seed + 41));

    const float cloud = Smoothstep(0.46f, 0.78f, large) * (0.35f + 0.65f * detail);

    const V3 cool = {0.16f, 0.26f, 0.55f};
    const V3 warm = {0.55f, 0.22f, 0.30f};
    V3       col  = Mix(cool, warm, Smoothstep(0.35f, 0.70f, hue)) * (cloud * (0.35f + 0.65f * band) * 0.070f);

    // Diffuse glow of unresolved stars along the band, cut by dark dust lanes.
    const V3 glow = V3{0.95f, 0.85f, 0.72f} * (band * (0.4f + 0.6f * detail) * 0.025f * (1.0f - 0.8f * dust));
    return col + glow;
}

struct Star {
    int   face;
    float px, py;  // texel coordinates on the level-0 face
    float sigma;
    V3    color;
};

// The stars as a list, so each face can be built on its own and only one face of floats is
// ever held at a time.
std::vector<Star> MakeStars(int size, uint32_t seed) {
    std::mt19937                          rng(seed);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    std::normal_distribution<float>       gauss(0.0f, 1.0f);

    const V3 bandU = Normalize(Cross(kBandNormal, V3{0.0f, 0.0f, 1.0f}));
    const V3 bandW = Cross(kBandNormal, bandU);

    const int         uniformStars = 18000;
    const int         bandStars    = 16000;
    std::vector<Star> stars;
    stars.reserve(uniformStars + bandStars);
    for (int i = 0; i < uniformStars + bandStars; ++i) {
        V3 d;
        if (i < uniformStars) {
            d = Normalize({gauss(rng), gauss(rng), gauss(rng)});
        } else {
            const float a = uni(rng) * 6.2831853f;
            d = Normalize(bandU * std::cos(a) + bandW * std::sin(a) + kBandNormal * (gauss(rng) * 0.12f));
        }

        // Most stars are faint; a handful are bright enough to saturate through the tone mapper.
        float bright = 0.035f * std::pow(std::max(uni(rng), 1e-4f), -0.8f);
        if (i >= uniformStars) bright *= 0.6f;
        bright = std::min(bright, 6.0f);

        const V3    tint = Mix(V3{1.0f, 1.0f, 1.0f}, Blackbody(3000.0f + 9000.0f * std::pow(uni(rng), 1.5f)), 0.55f);
        const float lum  = std::max(0.2126f * tint.x + 0.7152f * tint.y + 0.0722f * tint.z, 0.05f);

        Star  s;
        float fs, ft;
        DirectionToFace(d, s.face, fs, ft);
        s.px    = (fs + 1.0f) * 0.5f * static_cast<float>(size) - 0.5f;
        s.py    = (ft + 1.0f) * 0.5f * static_cast<float>(size) - 0.5f;
        s.sigma = 0.45f + 0.2f * std::min(bright, 2.0f);
        s.color = tint * (bright / lum);
        stars.push_back(s);
    }
    return stars;
}

}  // namespace

SkyboxImage GenerateSkybox(int size, uint32_t seed) {
    SkyboxImage image;
    image.size   = size;
    image.levels = static_cast<int>(std::floor(std::log2(static_cast<float>(size)))) + 1;
    size_t total = 0;
    for (int l = 0; l < image.levels; ++l) {
        const size_t s = static_cast<size_t>(size >> l);
        image.levelOffset.push_back(total);
        total += s * s * 6;
    }
    image.texels.resize(total);

    const std::vector<Star> stars   = MakeStars(size, seed);
    const int               threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    std::vector<float>      rgb(static_cast<size_t>(size) * size * 3);

    for (int face = 0; face < 6; ++face) {
        // Nebula, per texel, spread over every core: it is the only expensive part.
        {
            std::vector<std::jthread> pool;
            for (int w = 0; w < threads; ++w) {
                pool.emplace_back([&, w] {
                    for (int y = w; y < size; y += threads) {
                        const float t = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                        for (int x = 0; x < size; ++x) {
                            const float s = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                            const V3    c = Nebula(Normalize(FaceDirection(face, s, t)), seed);
                            float*      o = &rgb[(static_cast<size_t>(y) * size + x) * 3];
                            o[0] = c.x;
                            o[1] = c.y;
                            o[2] = c.z;
                        }
                    }
                });
            }
        }

        // Stars, splatted as small Gaussians. A splat is clipped at its face's edge, which at
        // this size is invisible.
        for (const Star& st : stars) {
            if (st.face != face) continue;
            const int cx = static_cast<int>(std::lround(st.px)), cy = static_cast<int>(std::lround(st.py));
            for (int y = std::max(cy - 2, 0); y <= std::min(cy + 2, size - 1); ++y) {
                for (int x = std::max(cx - 2, 0); x <= std::min(cx + 2, size - 1); ++x) {
                    const float dx = static_cast<float>(x) - st.px, dy = static_cast<float>(y) - st.py;
                    const float w  = std::exp(-(dx * dx + dy * dy) / (2.0f * st.sigma * st.sigma));
                    float*      o  = &rgb[(static_cast<size_t>(y) * size + x) * 3];
                    o[0] += st.color.x * w;
                    o[1] += st.color.y * w;
                    o[2] += st.color.z * w;
                }
            }
        }

        // Pack this level, box-filter down to the next, repeat. The mips are what keep the star
        // field from sparkling where lensing squeezes a wide patch of sky into a few pixels.
        int s = size;
        for (int l = 0; l < image.levels; ++l) {
            uint32_t* out = &image.texels[image.levelOffset[l] + static_cast<size_t>(face) * s * s];
            for (size_t i = 0; i < static_cast<size_t>(s) * s; ++i) {
                out[i] = PackE5B9G9R9(rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2]);
            }
            if (s == 1) break;
            const int half = s / 2;
            for (int y = 0; y < half; ++y) {
                for (int x = 0; x < half; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        auto at = [&](int xx, int yy) { return rgb[(static_cast<size_t>(yy) * s + xx) * 3 + c]; };
                        // In place is safe: texel (x, y) of the half-size image is written only
                        // after every full-size texel at or before it has been read.
                        rgb[(static_cast<size_t>(y) * half + x) * 3 + c] =
                            0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
                    }
                }
            }
            s = half;
        }
    }
    return image;
}

}  // namespace render
