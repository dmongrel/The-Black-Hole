#include "app/credit_roll.h"

#include <windows.h>

#include "app/earth.h"
#include "app/log.h"
#include "app/resource.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <string>

namespace app {
namespace {

// The timeline, in seconds.
constexpr double kFirstDelay = 3.0;   // the black hole alone first
constexpr double kFlyIn      = 3.0;
constexpr double kHoldOne    = 3.0;   // a single line
constexpr double kHoldTwo    = 4.0;   // two lines take longer to read
constexpr double kShake      = 1.1;   // the text shaking, its colours coming apart, just before
constexpr double kPixelate   = 0.12;  // the letters in blocks, a moment, before they come loose
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
constexpr float  kPixelLight     = 0.97f; // the blocks a shade dimmer than the text, so the two
                                          // read apart where they meet
constexpr float  kRoundLight     = 3.9f;  // a round glow carries this much less light than a flat
                                          // square the same size, so it is made brighter by it
constexpr float  kSpin           = 0.95f;  // must match SPIN in shaders/blackhole.frag

// The Earth, for a credit naming Sci-Man Dan: seconds into that credit.
constexpr double kEarthStart     = 3.3;    // the credit has landed
constexpr double kEarthSweep     = 3.0;    // into the frame
constexpr double kEarthRest      = 2.0;    // there, labelled, before it breaks up
constexpr double kLabelFade      = 0.4;
constexpr float  kEarthFlightMin = 3.5f;   // shorter than the letters': it starts nearer the hole
constexpr float  kEarthFlightMax = 5.5f;
constexpr float  kEarthLingerMin = 0.4f;
constexpr float  kEarthLingerMax = 1.2f;
constexpr float  kEarthSpinRate  = 0.22f;  // radians a second
constexpr float  kShadowRadius   = 5.2f;   // the shadow's apparent radius, about 3 sqrt(3) M
// When the last of it has gone into the hole, and its credit can go.
constexpr double kEarthEnd = kEarthStart + kEarthSweep + kEarthRest + kPixelate + kDissolve + kEarthLingerMax +
                             kEarthFlightMax * kGone;

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

float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Normalize3(float v[3]) {
    const float length = std::sqrt(Dot3(v, v));
    if (length > 0.0f) {
        for (int i = 0; i < 3; ++i) v[i] /= length;
    }
}

// Whether `credit` names Sci-Man Dan, however it is spaced, hyphenated or capitalised.
bool NamesSciManDan(const Credit& credit) {
    std::wstring letters;
    for (wchar_t c : credit.first + L" " + credit.second) {
        if (std::iswalnum(c)) letters += static_cast<wchar_t>(std::towlower(c));
    }
    return letters.find(L"scimandan") != std::wstring::npos;
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

// A label on one line: `text` in the titles' face, then `symbol` from the emoji font (drawn in
// outline, one colour, like the text), on one baseline.
render::TextImage RasterizeLabel(const std::wstring& text, const std::wstring& symbol, int pixels, uint32_t id) {
    HDC   dc       = CreateCompatibleDC(nullptr);
    HFONT face     = MakeFont(pixels);
    HFONT emoji    = CreateFontW(-static_cast<int>(std::lround(pixels * 1.15)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Emoji");
    const int tracking = std::max(1, static_cast<int>(std::lround(pixels * kTracking)));

    SIZE        textSize{}, symbolSize{};
    TEXTMETRICW faceMetrics{}, emojiMetrics{};
    SelectObject(dc, face);
    SetTextCharacterExtra(dc, tracking);
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &textSize);
    GetTextMetricsW(dc, &faceMetrics);
    SelectObject(dc, emoji);
    SetTextCharacterExtra(dc, 0);
    GetTextExtentPoint32W(dc, symbol.c_str(), static_cast<int>(symbol.size()), &symbolSize);
    GetTextMetricsW(dc, &emojiMetrics);

    const int pad     = static_cast<int>(std::ceil(pixels * 0.35));
    const int gap     = pixels / 2;
    const int ascent  = std::max(faceMetrics.tmAscent, emojiMetrics.tmAscent);
    const int descent = std::max(faceMetrics.tmDescent, emojiMetrics.tmDescent);

    render::TextImage image;
    image.id     = id;
    image.width  = static_cast<int>(textSize.cx) + gap + static_cast<int>(symbolSize.cx) + 2 * pad;
    image.height = ascent + descent + 2 * pad;

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
        SetTextAlign(dc, TA_BASELINE | TA_LEFT);
        SelectObject(dc, face);
        SetTextCharacterExtra(dc, tracking);
        TextOutW(dc, pad, pad + ascent, text.c_str(), static_cast<int>(text.size()));
        SelectObject(dc, emoji);
        SetTextCharacterExtra(dc, 0);
        TextOutW(dc, pad + static_cast<int>(textSize.cx) + gap, pad + ascent, symbol.c_str(),
                 static_cast<int>(symbol.size()));
        GdiFlush();
        const auto* px = static_cast<const uint8_t*>(bits);
        image.coverage.resize(static_cast<size_t>(image.width) * image.height);
        for (size_t i = 0; i < image.coverage.size(); ++i) image.coverage[i] = px[i * 4 + 1];  // green
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
    } else {
        image.width = image.height = 0;
    }
    DeleteObject(face);
    DeleteObject(emoji);
    DeleteDC(dc);
    return image;
}

}  // namespace

CreditRoll::CreditRoll(std::vector<Credit> credits, int width, int height)
    : credits_(std::move(credits)), width_(std::max(width, 1)), height_(std::max(height, 1)) {
    // Decode the Earth now, at start-up, rather than as its credit flies in.
    for (const Credit& c : credits_) {
        if (NamesSciManDan(c)) {
            EarthTextures();
            break;
        }
    }
}

float CreditRoll::HoldSeconds() const {
    const double hold = credits_[index_].second.empty() ? kHoldOne : kHoldTwo;
    // The Earth holds its credit until the last of it has gone into the hole.
    return static_cast<float>(earthCredit_ && EarthTextures().id ? std::max(hold, kEarthEnd - kFlyIn) : hold);
}

float CreditRoll::CreditSeconds() const {
    return static_cast<float>(kFlyIn + HoldSeconds() + kShake + kPixelate + kDissolve + kLingerMax + kFlightMax * kGone + kGap);
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
    fontPixels_ = fontPixels;
    block_      = std::max(2, static_cast<int>(std::lround(fontPixels / kFontPerBlock)));
    while (inked / static_cast<size_t>(block_ * block_) > kParticleBudget) ++block_;

    earthCredit_ = NamesSciManDan(credits_[index_]);
    earthPlaced_ = false;
    earthBroken_ = false;
    earthParticles_.clear();
    if (earthCredit_ && label_.width == 0) {
        label_ = RasterizeLabel(L"Earth (ROUND)", L"\U0001F44D", static_cast<int>(std::lround(fontPixels_ * 0.8)),
                                nextTextId_++);
    }
    Log("credits: %u of %u, %dx%d px, blocks of %d%s", static_cast<unsigned>(index_ + 1),
        static_cast<unsigned>(credits_.size()), text_.width, text_.height, block_, earthCredit_ ? ", with the Earth" : "");
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
            p.brightness  = kPixelLight * kBrightness * coverage;
            particles_.push_back(p);
            ++n;
        }
    }
    Log("credits: broke into %u particles (blocks of %d px)", n, block);
}

void CreditRoll::Shake(float progress, float seconds, float perPx) {
    // Jerky: a new displacement every beat, held until the next, not a smooth wobble. It builds
    // and keeps on to the last moment: the text only comes back together as it pixelates.
    constexpr float kBeat = 1.0f / 14.0f;
    const auto      beat  = static_cast<uint32_t>(seconds / kBeat);
    const uint32_t  key   = beat * 7919u + static_cast<uint32_t>(index_) * 104729u + 17u;
    const float     build = Smoothstep(0.0f, 0.3f, progress);
    if (build <= 0.0f) return;

    // The whole text jumps; now and then it sticks for a beat.
    const float font = static_cast<float>(fontPixels_);
    if (Hash(key, 20) > 0.2f) {
        const float jump = build * 0.18f * font * perPx;
        const float dx = jump * (2.0f * Hash(key, 21) - 1.0f), dy = 0.6f * jump * (2.0f * Hash(key, 22) - 1.0f);
        overlay_.rect[0] += dx;
        overlay_.rect[2] += dx;
        overlay_.rect[1] += dy;
        overlay_.rect[3] += dy;
    }

    // Red and blue slip off green, opposite ways; on some beats they tear further apart, about
    // half a line, into three images of the text.
    const float w = static_cast<float>(text_.width), h = static_cast<float>(text_.height);
    float       sx = build * 0.04f * font * (2.0f * Hash(key, 23) - 1.0f);
    float       sy = build * 0.02f * font * (2.0f * Hash(key, 24) - 1.0f);
    if (Hash(key, 25) < 0.3f * build) {
        const float tear = (0.35f + 0.3f * Hash(key, 26)) * 1.3f * font;  // about half a line
        if (Hash(key, 27) < 0.5f) {
            sy = tear * (Hash(key, 28) < 0.5f ? 1.0f : -1.0f);
        } else {
            sx = 2.0f * tear * (Hash(key, 28) < 0.5f ? 1.0f : -1.0f);
        }
    }
    overlay_.split[0] = sx / w;
    overlay_.split[1] = sy / h;
    overlay_.split[2] = -sx / w * (0.8f + 0.4f * Hash(key, 29));
    overlay_.split[3] = -sy / h * (0.8f + 0.4f * Hash(key, 30));
}

void CreditRoll::PlaceEarth(const render::CameraPose& camera, const render::CameraBasis& b) {
    const uint32_t seed    = earthRuns_++ * 7717u + 101u;
    const float    tanHalf = camera.tanHalfFov;
    const float    halfW   = tanHalf * static_cast<float>(width_) / static_cast<float>(height_);
    const float    halfH   = tanHalf;

    // The hole as seen: where it is on the screen (in the units of the text's rectangles), and
    // how big its shadow looks.
    const float toHole[3] = {-b.position[0], -b.position[1], -b.position[2]};
    const float distance  = std::sqrt(Dot3(toHole, toHole));
    const float hz        = Dot3(toHole, b.forward);
    const float hx        = hz > 0.05f * distance ? Dot3(toHole, b.right) / hz : 1e3f;
    const float hy        = hz > 0.05f * distance ? Dot3(toHole, b.up) / hz : 1e3f;
    const float shadow    = std::tan(std::asin(std::min(kShadowRadius / distance, 0.95f)));

    // A fifth to a third of the hole's size.
    const float fraction = 0.2f + (1.0f / 3.0f - 0.2f) * Hash(seed, 1);
    const float size     = fraction * shadow;

    // Somewhere at random that is out of the hole's range, wholly in the frame with room for the
    // label beneath, and clear of the credit; failing that, as far from the hole as can be found.
    // Kept off the frame's far sides, where a wide view stretches a sphere.
    const float perPx     = 2.0f * tanHalf / static_cast<float>(height_);
    const float labelRoom = 2.4f * static_cast<float>(fontPixels_) * perPx;
    const float labelHalf = 0.5f * static_cast<float>(label_.width) * perPx + 0.03f * halfW;
    const float spanX     = std::max(std::min(0.8f * halfW, halfW - std::max(1.3f * size, labelHalf)), 0.0f);
    float       bestX = 0.5f * halfW, bestY = 0.4f * halfH, bestClear = -1e9f;
    for (uint32_t i = 0; i < 96; ++i) {
        const float x     = (2.0f * Hash(seed, 10 + 2 * i) - 1.0f) * spanX;
        const float lowY  = -halfH + 1.3f * size + labelRoom;
        const float y     = lowY + Hash(seed, 11 + 2 * i) * std::max(halfH - 1.3f * size - lowY, 0.0f);
        float       clear = std::hypot(x - hx, y - hy) - (2.4f * shadow + size);
        const bool  overText = x + size > restRect_[0] && x - size < restRect_[2] && y + size > restRect_[3] &&
                              y - size - labelRoom < restRect_[1];
        if (overText) clear -= 10.0f * halfH;
        if (clear > bestClear) {
            bestClear = clear;
            bestX     = x;
            bestY     = y;
        }
        if (clear >= 0.0f) break;
    }

    const auto direction = [&](float x, float y, float d[3]) {
        for (int i = 0; i < 3; ++i) d[i] = b.forward[i] + b.right[i] * x + b.up[i] * y;
        Normalize3(d);
    };
    const auto at = [&](float x, float y, float dist, float out[3]) {
        float d[3];
        direction(x, y, d);
        for (int i = 0; i < 3; ++i) out[i] = b.position[i] + d[i] * dist;
    };

    // Nearer than the hole: at about the distance where the hole would be square to it from the
    // camera (Thales), the disk's light falls across its face and splits it into day and night,
    // the night side showing its cities. Its size is the apparent one, so the distance does not
    // change how big it looks beside the hole.
    float       towardEarth[3], towardHole[3] = {toHole[0], toHole[1], toHole[2]};
    direction(bestX, bestY, towardEarth);
    Normalize3(towardHole);
    const float dist = std::max(0.35f * distance, 0.95f * distance * Dot3(towardEarth, towardHole));

    // It comes in from the side of the frame away from the hole, arcing up and over, from a
    // little farther off.
    const float side  = bestX >= hx ? 1.0f : -1.0f;
    const float fromX = side * (halfW + 2.5f * size);
    const float fromY = bestY + (Hash(seed, 3) - 0.5f) * 0.6f * halfH;
    at(fromX, fromY, 1.25f * dist, earthPath_[0]);
    at(0.5f * (fromX + bestX), std::max(fromY, bestY) + 0.25f * halfH, 1.1f * dist, earthPath_[1]);
    at(bestX, bestY, dist, earthPath_[2]);
    earth_.radius = dist * size / std::sqrt(1.0f + size * size);

    // Its axis tilted as the real one is, against the camera's up, one way or the other; and a
    // random face to it.
    const float tilt = (Hash(seed, 4) < 0.5f ? 1.0f : -1.0f) * 0.41f;
    for (int i = 0; i < 3; ++i) earth_.north[i] = b.up[i] * std::cos(tilt) + b.right[i] * std::sin(tilt);
    const float along = Dot3(b.right, earth_.north);
    for (int i = 0; i < 3; ++i) earth_.east[i] = b.right[i] - earth_.north[i] * along;
    Normalize3(earth_.east);
    earthSpin_    = 6.2831853f * Hash(seed, 5);
    earth_.images = &EarthTextures();
    Log("credits: the Earth, %.2f of the hole's size, at (%.2f, %.2f), clear by %.2f", fraction, bestX / halfW,
        bestY / halfH, bestClear);
}

void CreditRoll::BreakUpEarth(const render::CameraPose& camera, const render::CameraBasis& b) {
    // One particle for each block of the pixelated Earth, on the screen's block grid as
    // shaders/earth.frag draws it, where the block's centre meets the sphere and in the colour
    // it is drawn there.
    const render::EarthImages& images  = EarthTextures();
    const int                  block   = block_;
    const float                W       = static_cast<float>(width_), H = static_cast<float>(height_);
    const float                tanHalf = camera.tanHalfFov, aspect = W / H;
    const float                pixelAngle = 2.0f * tanHalf / H;
    const float                texels  = static_cast<float>(images.width) / 6.2831853f;
    const float                radius  = earth_.radius;

    const float oc[3] = {earth_.center[0] - b.position[0], earth_.center[1] - b.position[1],
                         earth_.center[2] - b.position[2]};
    const float z     = Dot3(oc, b.forward);
    if (z <= radius) return;
    const float cx = Dot3(oc, b.right) / z, cy = Dot3(oc, b.up) / z;
    const float px = (cx / (tanHalf * aspect) * 0.5f + 0.5f) * W;
    const float py = (-cy / tanHalf * 0.5f + 0.5f) * H;
    const float rp = radius / std::sqrt(Dot3(oc, oc) - radius * radius) / pixelAngle * 1.3f + static_cast<float>(block);

    // Its own straight-on frame, as shaders/earth.frag draws it.
    float e[3] = {oc[0], oc[1], oc[2]}, r[3], u[3];
    Normalize3(e);
    const float re = Dot3(b.right, e);
    for (int i = 0; i < 3; ++i) r[i] = b.right[i] - e[i] * re;
    Normalize3(r);
    const float ue = Dot3(b.up, e), ur = Dot3(b.up, r);
    for (int i = 0; i < 3; ++i) u[i] = b.up[i] - e[i] * ue - r[i] * ur;
    Normalize3(u);
    const int   x0 = std::max(0, static_cast<int>(std::floor((px - rp) / block))) * block;
    const int   y0 = std::max(0, static_cast<int>(std::floor((py - rp) / block))) * block;
    const int   x1 = std::min(width_, static_cast<int>(px + rp));
    const int   y1 = std::min(height_, static_cast<int>(py + rp));

    uint32_t n = 0;
    for (int by = y0; by < y1; by += block) {
        for (int bx = x0; bx < x1; bx += block) {
            const float fx = bx + 0.5f * block, fy = by + 0.5f * block;
            const float nx = fx / W * 2.0f - 1.0f, ny = fy / H * 2.0f - 1.0f;
            const float ox = nx * tanHalf * aspect - cx, oy = -ny * tanHalf - cy;
            float       d[3], real[3];
            for (int i = 0; i < 3; ++i) {
                d[i]    = e[i] + r[i] * ox + u[i] * oy;
                real[i] = b.forward[i] + b.right[i] * nx * tanHalf * aspect - b.up[i] * ny * tanHalf;
            }
            Normalize3(d);
            Normalize3(real);
            const float along = Dot3(oc, d);
            float       perp[3];
            for (int i = 0; i < 3; ++i) perp[i] = oc[i] - d[i] * along;
            const float off = std::sqrt(Dot3(perp, perp));
            if (along <= 0.0f || off >= radius) continue;

            const float t = along - std::sqrt(radius * radius - off * off);
            Particle    p;
            float       normal[3], view[3];
            // Shaded where the straight-on ray meets it; set off from where that looks to be, on
            // the camera's true ray through the block, so it starts exactly on its block.
            for (int i = 0; i < 3; ++i) {
                normal[i]  = (b.position[i] + d[i] * t - earth_.center[i]) / radius;
                view[i]    = -d[i];
                p.start[i] = b.position[i] + real[i] * t;
            }
            const float span = along * pixelAngle * block / (radius * std::max(Dot3(normal, view), 0.25f));
            EarthColor(images, earth_, normal, view, std::log2(std::max(span * texels, 1.0f)), p.color);

            p.world      = true;
            p.brightness = kPixelLight;
            p.radius     = 0.5f * block;
            p.release    = kDissolve * Hash(n, 40);
            p.flight     = kEarthFlightMin + (kEarthFlightMax - kEarthFlightMin) * Hash(n, 41);
            p.linger     = Hash(n, 42) < kHeavy ? kEarthLingerMin + (kEarthLingerMax - kEarthLingerMin) * Hash(n, 43) : 0.0f;
            p.turn       = 2.0f + 1.5f * Hash(n, 44);
            p.phase      = fx * 0.009f + fy * 0.006f + 0.6f * Hash(n, 45);
            // The same drift apart as the letters', as it looks on the screen at this distance.
            const float drift = (0.012f + 0.02f * Hash(n, 46)) * t;
            p.scale           = t / kRestDepth;
            p.scatter[0]      = (Hash(n, 47) - 0.5f) * drift;
            p.scatter[1]      = (Hash(n, 48) - 0.5f) * drift;
            p.scatter[2]      = 0.3f * Hash(n, 49) * drift;
            earthParticles_.push_back(p);
            ++n;
        }
    }
    Log("credits: the Earth broke into %u particles", n);
}

void CreditRoll::UpdateEarth(double local, double seconds, const render::CameraPose& camera,
                             const render::CameraBasis& b, float inner) {
    if (EarthTextures().id == 0 || local < kEarthStart) return;
    if (!earthPlaced_) {
        earthPlaced_ = true;
        PlaceEarth(camera, b);
    }

    const double arrive  = kEarthStart + kEarthSweep;
    const double breakAt = arrive + kEarthRest;
    if (local < breakAt + kPixelate) {
        // Sweeping in, easing to a stop and turning as it comes; held there, labelled; then, for
        // a moment, pixelated.
        const float s = std::min(1.0f, static_cast<float>((local - kEarthStart) / kEarthSweep));
        const float e = 1.0f - (1.0f - s) * (1.0f - s) * (1.0f - s);
        for (int i = 0; i < 3; ++i) {
            earth_.center[i] = (1.0f - e) * (1.0f - e) * earthPath_[0][i] + 2.0f * (1.0f - e) * e * earthPath_[1][i] +
                               e * e * earthPath_[2][i];
            earth_.light[i]  = -earth_.center[i];  // lit by the disk, round the hole
        }
        Normalize3(earth_.light);
        earth_.spin    = earthSpin_ + kEarthSpinRate * static_cast<float>(local - kEarthStart);
        earth_.block   = local >= breakAt ? static_cast<float>(block_) : 1.0f;
        earth_.alpha   = 1.0f;
        overlay_.earth = earth_;

        // The label, centred under it, from half way through its sweep in until it breaks up.
        const float fadeIn  = Smoothstep(0.0f, static_cast<float>(kLabelFade),
                                        static_cast<float>(local - (kEarthStart + 0.5 * kEarthSweep)));
        const float fadeOut = 1.0f - Smoothstep(0.0f, 0.25f, static_cast<float>(local - breakAt));
        const float rel[3]  = {earth_.center[0] - b.position[0], earth_.center[1] - b.position[1],
                               earth_.center[2] - b.position[2]};
        const float z       = Dot3(rel, b.forward);
        if (fadeIn * fadeOut > 0.0f && z > 2.0f * earth_.radius) {
            const float perPx = 2.0f * camera.tanHalfFov / static_cast<float>(height_);
            const float cx = Dot3(rel, b.right) / z, cy = Dot3(rel, b.up) / z;
            const float r  = earth_.radius / std::sqrt(Dot3(rel, rel) - earth_.radius * earth_.radius);
            const float w = label_.width * perPx, h = label_.height * perPx;
            const float top = cy - r * 1.08f;
            overlay_.label        = &label_;
            overlay_.labelRect[0] = cx - 0.5f * w;
            overlay_.labelRect[1] = top;
            overlay_.labelRect[2] = cx + 0.5f * w;
            overlay_.labelRect[3] = top - h;
            overlay_.labelAlpha   = fadeIn * fadeOut;
        }
        return;
    }
    if (!earthBroken_) {
        earthBroken_ = true;
        BreakUpEarth(camera, b);
    }
    Advance(earthParticles_, start_ + breakAt + kPixelate, seconds, b, inner);
}

const render::Overlay& CreditRoll::Update(double seconds, const render::CameraPose& camera) {
    overlay_.text         = nullptr;
    overlay_.label        = nullptr;
    overlay_.earth.images = nullptr;
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
    const double shakeAt   = kFlyIn + HoldSeconds();
    const double breakAt   = shakeAt + kShake;
    if (local < 0.0) return overlay_;

    std::memcpy(overlay_.rect, restRect_, sizeof(restRect_));
    overlay_.brightness = kBrightness;
    overlay_.block      = 1.0f;
    std::memset(overlay_.split, 0, sizeof(overlay_.split));
    overlay_.scale      = 1.0f;
    overlay_.alpha      = 1.0f;

    const render::CameraBasis b     = render::Basis(camera);
    const float               inner = InnerEdge() * 1.03f;
    if (earthCredit_) UpdateEarth(local, seconds, camera, b, inner);

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
    } else if (local < shakeAt) {
        overlay_.text = &text_;
    } else if (local < breakAt) {
        overlay_.text = &text_;
        Shake(static_cast<float>((local - shakeAt) / kShake), static_cast<float>(local - shakeAt), perPx);
    } else if (local < breakAt + kPixelate) {
        // The shake ends straight into pixelated letters, at full block size from the first
        // frame, so the text is never seen whole and still between the two.
        overlay_.text  = &text_;
        overlay_.block = static_cast<float>(block_);
    } else {
        // Then each block is a particle, and they come loose.
        if (!brokenUp_) {
            brokenUp_ = true;
            BreakUp(text_);
        }
        Advance(particles_, start_ + breakAt + kPixelate, seconds, b, inner);
    }
    return overlay_;
}

void CreditRoll::Advance(std::vector<Particle>& particles, double t0, double seconds, const render::CameraBasis& b,
                         float inner) {
    for (Particle& p : particles) {
        const double            releaseAt = t0 + p.release;
        render::OverlayParticle out{};
        std::memcpy(out.color, p.color, sizeof(out.color));
        if (seconds < releaseAt) {
            // Still a block of its letter, fixed to the camera, or of the Earth, where it was.
            for (int i = 0; i < 3; ++i) {
                out.position[i] = p.world ? p.start[i]
                                          : b.position[i] + b.right[i] * p.local[0] + b.up[i] * p.local[1] +
                                                b.forward[i] * p.local[2];
            }
            out.radius     = p.radius;
            out.brightness = p.brightness;
            out.square     = 1.0f;
            overlay_.particles.push_back(out);
            continue;
        }
        if (!p.released) {
            // A letter's block leaves the camera here: from now on it is in the world, where
            // the hole is.
            p.released   = true;
            p.releasedAt = releaseAt;
            float burst[3];
            for (int i = 0; i < 3; ++i) {
                if (!p.world) {
                    p.start[i] = b.position[i] + b.right[i] * p.local[0] + b.up[i] * p.local[1] + b.forward[i] * p.local[2];
                }
                burst[i] = b.right[i] * p.scatter[0] + b.up[i] * p.scatter[1] + b.forward[i] * p.scatter[2];
            }
            std::memcpy(p.scatter, burst, sizeof(burst));

            // The way it will go, fixed now. It sets off straight back towards the hole (`via`
            // lies half way there, so the curve leaves along that line), then bends across to the
            // disk: for the letters on the hole's left, as seen, on the camera's side; for the
            // Earth, on its own side.
            const auto  id   = static_cast<uint32_t>(&p - particles.data());
            const float hole = std::sqrt(Dot3(p.start, p.start));
            for (int i = 0; i < 3; ++i) {
                p.via[i] = 0.5f * p.start[i] + 0.03f * hole * (Hash(id, 10 + i) - 0.5f);
            }
            const float side      = p.world ? std::atan2(p.start[2], p.start[0]) : std::atan2(-b.right[2], -b.right[0]);
            const float bendAngle = side + 0.5f * (Hash(id, 13) - 0.5f);
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
            const float sway = 0.006f * kRestDepth * p.scale * std::sin(2.3f * since + p.phase) * drift;
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
            // A curve from where it was, past `via`, to the bend: slow to leave, then gathering pace.
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
}

}  // namespace app
