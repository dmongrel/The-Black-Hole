#include "app/credit_roll.h"

#include <windows.h>

#include "app/log.h"
#include "app/resource.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace app {
namespace {

// The timeline, in seconds.
constexpr double kFirstDelay = 3.0;   // the black hole alone first
constexpr double kFlyIn      = 3.0;
constexpr double kHoldOne    = 3.0;   // a single line
constexpr double kHoldTwo    = 4.0;   // two lines take longer to read
constexpr double kPixelate   = 1.2;   // the letters coarsening into blocks
constexpr float  kDissolve   = 1.5f;  // the blocks coming loose, at random all over the line
constexpr float  kFlightMin  = 5.8;   // each particle's trip to the disk. Spread wide, so the
constexpr float  kFlightMax  = 8.8;   // cloud strings out into a stream along the path
constexpr float  kHeavy      = 0.22f; // of the particles: they hang back before the flow takes them
constexpr float  kLingerMin  = 0.8f;
constexpr float  kLingerMax  = 2.4f;
constexpr float  kGone       = 0.75f; // of the flight: faded out by here, half way round the spiral
constexpr double kGap        = 1.0;
constexpr double kPause      = 60.0;  // after the last credit, before the first again

// Layout. The text rests this many world units in front of the camera: near enough that it
// stays with the camera while it moves, and that its particles start big and shrink as they
// fly off towards the hole.
constexpr float kRestDepth  = 2.0f;
constexpr float kLeftMargin = 0.07f;  // of the screen's width
constexpr float kBottom     = 0.13f;  // of the screen's height, from the bottom
constexpr float kFontHeight = 0.026f; // of the screen's height; Michroma is broad and tall-bodied
constexpr float kTracking   = 0.06f;  // letter-spacing, of the font's height
constexpr float kMaxWidth   = 0.80f;  // of the screen's width; longer credits are set smaller
constexpr float kBrightness = 1.6f;   // linear HDR: a little over white, so it blooms slightly

constexpr size_t kParticleBudget = 60000;  // under render::kMaxOverlayParticles
constexpr float  kFontPerBlock   = 20.0f; // blocks are about this fraction of the font's height
constexpr float  kRoundLight     = 3.9f;  // a round glow carries this much less light than a flat
                                          // square the same size, so it is made brighter by it
constexpr float  kSpin           = 0.95f;  // must match SPIN in shaders/blackhole.frag

float Smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A repeatable pseudo-random number in [0, 1) for particle `i`, stream `k`.
float Hash(uint32_t i, uint32_t k) {
    uint32_t h = i * 0x9E3779B1u + k * 0x85EBCA77u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return static_cast<float>(h >> 8) / 16777216.0f;
}

// The prograde innermost stable circular orbit (Bardeen, Press & Teukolsky), as in the shader:
// the disk's bright inner edge, where the particles end.
float InnerEdge() {
    const float a  = kSpin;
    const float z1 = 1.0f + std::cbrt(1.0f - a * a) * (std::cbrt(1.0f + a) + std::cbrt(1.0f - a));
    const float z2 = std::sqrt(3.0f * a * a + z1 * z1);
    return 3.0f + z2 - std::sqrt((3.0f - z1) * (3.0f + z1 + 2.0f * z2));
}

// Michroma, embedded in the .scr (see THIRD_PARTY_NOTICES.md), added for this process alone:
// nothing is installed on the machine. Once; the font stays loaded until the process ends.
void LoadEmbeddedFont() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    HRSRC   res  = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_CREDITS_FONT), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    DWORD   count = 0;
    if (!data || !AddFontMemResourceEx(LockResource(data), SizeofResource(nullptr, res), nullptr, &count)) {
        Log("credits: the embedded font could not be loaded");
    }
}

HFONT MakeFont(int pixels) {
    // Michroma: squared, wide and even, like the titles of a film set in space. It has one weight.
    // Segoe UI Light (on every Windows since 7) and Bahnschrift Light are the fallbacks.
    LoadEmbeddedFont();
    for (const wchar_t* face : {L"Michroma", L"Segoe UI Light", L"Bahnschrift Light"}) {
        const int weight = std::wcscmp(face, L"Michroma") == 0 ? FW_NORMAL : FW_LIGHT;
        HFONT font = CreateFontW(-pixels, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
        if (!font) continue;
        HDC      dc  = CreateCompatibleDC(nullptr);
        HGDIOBJ  old = SelectObject(dc, font);
        wchar_t  got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        DeleteDC(dc);
        if (std::wcscmp(got, face) == 0) return font;
        DeleteObject(font);
    }
    return CreateFontW(-pixels, 0, 0, 0, FW_LIGHT, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, nullptr);
}

// The credit's lines, left-justified, as 8-bit coverage.
render::TextImage Rasterize(const Credit& credit, int screenWidth, int screenHeight, uint32_t id, int& fontPixels) {
    const std::wstring* lines[2] = {&credit.first, &credit.second};
    const int           count    = credit.second.empty() ? 1 : 2;

    HDC dc     = CreateCompatibleDC(nullptr);
    int pixels = std::max(12, static_cast<int>(std::lround(screenHeight * kFontHeight)));
    HFONT font = nullptr;
    int   widest = 0, tracking = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (font) DeleteObject(font);
        font     = MakeFont(pixels);
        tracking = std::max(1, static_cast<int>(std::lround(pixels * kTracking)));
        SelectObject(dc, font);
        SetTextCharacterExtra(dc, tracking);
        widest = 0;
        for (int i = 0; i < count; ++i) {
            SIZE size{};
            GetTextExtentPoint32W(dc, lines[i]->c_str(), static_cast<int>(lines[i]->size()), &size);
            widest = std::max(widest, static_cast<int>(size.cx));
        }
        const int limit = static_cast<int>(screenWidth * kMaxWidth);
        if (widest <= limit) break;
        pixels = std::max(10, pixels * limit / widest);
    }

    const int pad        = static_cast<int>(std::ceil(pixels * 0.35));
    const int lineHeight = static_cast<int>(std::lround(pixels * 1.3));

    render::TextImage image;
    image.id     = id;
    image.width  = widest + 2 * pad;
    image.height = count * lineHeight + 2 * pad;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = image.width;
    bmi.bmiHeader.biHeight      = -image.height;  // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void*   bits   = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
        std::memset(bits, 0, static_cast<size_t>(image.width) * image.height * 4);
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        for (int i = 0; i < count; ++i) {
            TextOutW(dc, pad, pad + i * lineHeight, lines[i]->c_str(),
                     static_cast<int>(lines[i]->size()));
        }
        GdiFlush();
        const auto* px = static_cast<const uint8_t*>(bits);
        image.coverage.resize(static_cast<size_t>(image.width) * image.height);
        for (size_t i = 0; i < image.coverage.size(); ++i) image.coverage[i] = px[i * 4 + 1];  // green
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
    } else {
        image.width = image.height = 0;
    }
    DeleteObject(font);
    DeleteDC(dc);
    fontPixels = pixels;
    return image;
}

}  // namespace

CreditRoll::CreditRoll(std::vector<Credit> credits, int width, int height)
    : credits_(std::move(credits)), width_(std::max(width, 1)), height_(std::max(height, 1)) {}

float CreditRoll::HoldSeconds() const {
    return static_cast<float>(credits_[index_].second.empty() ? kHoldOne : kHoldTwo);
}

float CreditRoll::CreditSeconds() const {
    return static_cast<float>(kFlyIn + HoldSeconds() + kPixelate + kDissolve + kLingerMax + kFlightMax * kGone + kGap);
}

void CreditRoll::StartCredit(size_t index) {
    index_    = index;
    brokenUp_ = false;
    particles_.clear();
    int fontPixels = 0;
    text_          = Rasterize(credits_[index_], width_, height_, nextTextId_++, fontPixels);

    // Blocks big enough to read as pixelation at this size, and few enough for the budget.
    size_t inked = 0;
    for (uint8_t c : text_.coverage) inked += c > 0;
    block_ = std::max(2, static_cast<int>(std::lround(fontPixels / kFontPerBlock)));
    while (inked / static_cast<size_t>(block_ * block_) > kParticleBudget) ++block_;
    Log("credits: %u of %u, %dx%d px, blocks of %d", static_cast<unsigned>(index_ + 1),
        static_cast<unsigned>(credits_.size()), text_.width, text_.height, block_);
}

void CreditRoll::BreakUp(const render::TextImage& text) {
    // One particle for each block of the pixelated text, where the block is and as bright as it
    // is drawn (the same average as shaders/text.frag), so the letters themselves come apart and
    // nothing is added that was not there.
    const int   block = block_;
    const float left = restRect_[0], top = restRect_[1], right = restRect_[2], bottom = restRect_[3];

    uint32_t n = 0;
    for (int y = 0; y < text.height; y += block) {
        for (int x = 0; x < text.width; x += block) {
            float sum = 0.0f;
            for (int yy = y; yy < std::min(y + block, text.height); ++yy) {
                for (int xx = x; xx < std::min(x + block, text.width); ++xx) {
                    sum += text.coverage[static_cast<size_t>(yy) * text.width + xx] / 255.0f;
                }
            }
            const float coverage = sum / static_cast<float>(block * block);
            if (coverage < 0.02f) continue;

            const float u = (x + 0.5f * block) / text.width;
            const float v = (y + 0.5f * block) / text.height;
            Particle    p;
            p.local[0] = (left + u * (right - left)) * kRestDepth;
            p.local[1] = (top + v * (bottom - top)) * kRestDepth;
            p.local[2] = kRestDepth;
            p.release  = kDissolve * Hash(n, 0);  // anywhere along the line, not in order
            p.flight   = kFlightMin + (kFlightMax - kFlightMin) * Hash(n, 1);
            p.linger   = Hash(n, 16) < kHeavy ? kLingerMin + (kLingerMax - kLingerMin) * Hash(n, 15) : 0.0f;
            p.turn     = 2.0f + 1.5f * Hash(n, 2);
            p.phase    = u * 9.0f + v * 3.0f + 0.6f * Hash(n, 3);
            // As it comes loose it drifts a little away from its neighbours, mostly across the
            // screen, so the letters look drawn apart.
            const float bx = Hash(n, 4) - 0.5f, by = Hash(n, 5) - 0.5f, bz = 0.3f * Hash(n, 6);
            const float drift = (0.012f + 0.02f * Hash(n, 7)) * kRestDepth;
            p.scatter[0]  = bx * drift;
            p.scatter[1]  = by * drift;
            p.scatter[2]  = bz * drift;
            p.radius      = 0.5f * block;
            p.brightness  = kBrightness * coverage;
            particles_.push_back(p);
            ++n;
        }
    }
    Log("credits: broke into %u particles (blocks of %d px)", n, block);
}

const render::Overlay& CreditRoll::Update(double seconds, const render::CameraPose& camera) {
    overlay_.text = nullptr;
    overlay_.particles.clear();
    if (credits_.empty()) return overlay_;

    if (!started_) {
        started_ = true;
        start_   = kFirstDelay;
        StartCredit(0);
    }
    while (seconds >= start_ + CreditSeconds()) {
        start_ += CreditSeconds();
        if (index_ + 1 == credits_.size()) {
            start_ += kPause;
            StartCredit(0);
        } else {
            StartCredit(index_ + 1);
        }
    }

    // Where the text rests, for the camera's field of view now.
    const float tanHalf = camera.tanHalfFov;
    const float aspect  = static_cast<float>(width_) / static_cast<float>(height_);
    const float perPx   = 2.0f * tanHalf / static_cast<float>(height_);
    restRect_[0] = (-1.0f + 2.0f * kLeftMargin) * tanHalf * aspect;
    restRect_[3] = -(1.0f - 2.0f * kBottom) * tanHalf;
    restRect_[2] = restRect_[0] + text_.width * perPx;
    restRect_[1] = restRect_[3] + text_.height * perPx;

    const double local     = seconds - start_;
    const double breakAt   = kFlyIn + HoldSeconds();
    if (local < 0.0) return overlay_;

    std::memcpy(overlay_.rect, restRect_, sizeof(restRect_));
    overlay_.brightness = kBrightness;
    overlay_.block      = 1.0f;
    overlay_.scale      = 1.0f;
    overlay_.alpha      = 1.0f;

    if (local < kFlyIn) {
        // From behind the camera: its depth runs from behind the viewer to the resting distance,
        // easing out so it slows to a halt. Shown once it is safely in front.
        const float s     = static_cast<float>(local / kFlyIn);
        const float depth = -0.6f + 1.6f * (1.0f - (1.0f - s) * (1.0f - s) * (1.0f - s));
        if (depth > 0.07f) {
            overlay_.text  = &text_;
            overlay_.scale = 1.0f / depth;
            overlay_.alpha = Smoothstep(0.07f, 0.6f, depth);
        }
        return overlay_;
    }
    if (local < breakAt) {
        overlay_.text = &text_;
        return overlay_;
    }

    // Breaking up. First the letters pixelate, the blocks growing to their full size.
    const double sinceBreak = local - breakAt;
    if (sinceBreak < kPixelate) {
        overlay_.text  = &text_;
        overlay_.block = 1.0f + std::floor(static_cast<float>(sinceBreak / kPixelate) * static_cast<float>(block_));
        return overlay_;
    }
    // Then each block is a particle, and they come loose.
    if (!brokenUp_) {
        brokenUp_ = true;
        BreakUp(text_);
    }

    const render::CameraBasis b     = render::Basis(camera);
    const float               inner = InnerEdge() * 1.03f;
    const double              t0    = start_ + breakAt + kPixelate;
    for (Particle& p : particles_) {
        const double releaseAt = t0 + p.release;
        render::OverlayParticle out{};
        if (seconds < releaseAt) {
            // Still a block of its letter, fixed to the camera.
            for (int i = 0; i < 3; ++i) {
                out.position[i] = b.position[i] + b.right[i] * p.local[0] + b.up[i] * p.local[1] + b.forward[i] * p.local[2];
            }
            out.radius     = p.radius;
            out.brightness = p.brightness;
            out.square     = 1.0f;
            overlay_.particles.push_back(out);
            continue;
        }
        if (!p.released) {
            // It leaves the camera here: from now on it is in the world, where the hole is.
            p.released   = true;
            p.releasedAt = releaseAt;
            float burst[3];
            for (int i = 0; i < 3; ++i) {
                p.start[i] = b.position[i] + b.right[i] * p.local[0] + b.up[i] * p.local[1] + b.forward[i] * p.local[2];
                burst[i]   = b.right[i] * p.scatter[0] + b.up[i] * p.scatter[1] + b.forward[i] * p.scatter[2];
            }
            std::memcpy(p.scatter, burst, sizeof(burst));

            // The way it will go, fixed now from the camera it leaves. First deep into the scene
            // while keeping to the text's place on screen (a path straight at the hole would run
            // along the line of sight and show as a blob over it); then across to the disk on
            // the hole's left, as seen, on the camera's side of the disk.
            const auto  id    = static_cast<uint32_t>(&p - particles_.data());
            const float hole  = std::sqrt(b.position[0] * b.position[0] + b.position[1] * b.position[1] +
                                          b.position[2] * b.position[2]);
            const float depth = 0.55f * hole;
            const float wide  = 1.25f * depth / p.local[2];  // a little wider than the text, for the arc
            for (int i = 0; i < 3; ++i) {
                p.via[i] = b.position[i] + (b.right[i] * p.local[0] + b.up[i] * p.local[1]) * wide +
                           b.forward[i] * depth + 0.04f * hole * (Hash(id, 10 + i) - 0.5f);
            }
            const float leftAngle = std::atan2(-b.right[2], -b.right[0]);
            const float bendAngle = leftAngle + 0.5f * (Hash(id, 13) - 0.5f);
            const float bendR     = 7.5f + 2.5f * Hash(id, 14);
            p.bend[0]             = bendR * std::cos(bendAngle);
            p.bend[1]             = std::copysign(0.6f, b.position[1]);
            p.bend[2]             = bendR * std::sin(bendAngle);
            p.endAngle            = bendAngle + p.turn;
        }

        // Loose, the block softens into a round glow carrying the same light.
        const float since = static_cast<float>(seconds - p.releasedAt);
        const float round = Smoothstep(0.0f, 0.8f, since);
        const float light = p.brightness * (1.0f + (kRoundLight - 1.0f) * round);
        out.square        = 1.0f - round;

        // A heavy one drifts loose and hangs there, the stream pulling past it, before it goes.
        const float drift = p.linger > 0.0f ? 1.0f - std::exp(-2.0f * std::min(since, p.linger)) : 0.0f;
        if (since < p.linger) {
            const float sway = 0.006f * kRestDepth * std::sin(2.3f * since + p.phase) * drift;
            for (int i = 0; i < 3; ++i) out.position[i] = p.start[i] + drift * p.scatter[i] + sway * b.up[i];
            out.radius     = p.radius;
            out.brightness = light;
            overlay_.particles.push_back(out);
            continue;
        }
        const float s = (since - p.linger) / p.flight;
        if (s >= kGone) continue;

        float pos[3];
        constexpr float kLeg = 0.5f;  // of the flight spent reaching the disk; the rest spirals in
        if (s < kLeg) {
            // A curve from the text, past `via`, to the bend: slow to leave, then gathering pace.
            const float t = s / kLeg;
            const float e = t * t * (2.0f - t);
            for (int i = 0; i < 3; ++i) {
                pos[i] = (1.0f - e) * (1.0f - e) * p.start[i] + 2.0f * (1.0f - e) * e * p.via[i] + e * e * p.bend[i];
            }
        } else {
            // A spiral about the spin axis from the bend to the inner edge: the radius closes, the
            // angle swings round with the disk's rotation faster as it tightens, like an orbit
            // decaying, and the height settles into the disk. Neighbours share a wobble, so the
            // stream flows in ribbons.
            const float t     = (s - kLeg) / (1.0f - kLeg);
            const float r2    = std::hypot(p.bend[0], p.bend[2]);
            const float a2    = std::atan2(p.bend[2], p.bend[0]);
            float       r     = r2 + (inner - r2) * (1.0f - std::pow(1.0f - t, 1.6f));
            const float angle = a2 + (p.endAngle - a2) * std::pow(t, 1.3f);
            float       y     = p.bend[1] * (1.0f - t) * (1.0f - t);
            const float amp   = 0.08f * r * std::sin(3.14159265f * t);
            const float w     = 6.2831853f * 1.5f * t + p.phase;
            r += amp * std::sin(w);
            y += 0.4f * amp * std::cos(w);
            pos[0] = r * std::cos(angle);
            pos[1] = y;
            pos[2] = r * std::sin(angle);
        }
        // The drift apart as it comes loose, easing in, then given up to the stream; a heavy one
        // has drifted already, and leaves from there.
        const float apart = p.linger > 0.0f ? drift * (1.0f - Smoothstep(0.0f, 0.5f, s))
                                            : Smoothstep(0.0f, 0.12f, s) * (1.0f - s);
        for (int i = 0; i < 3; ++i) out.position[i] = pos[i] + apart * p.scatter[i];
        out.radius     = p.radius * (1.0f - 0.45f * s);
        out.brightness = light * (1.0f - 0.4f * s) * (1.0f - Smoothstep(0.42f, kGone, s));
        out.heat       = Smoothstep(0.4f, 1.0f, s);
        overlay_.particles.push_back(out);
    }
    return overlay_;
}

}  // namespace app
