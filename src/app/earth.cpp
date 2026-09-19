#include "app/earth.h"

#include <windows.h>
#include <wincodec.h>

#include "app/log.h"
#include "app/resource.h"

#include <algorithm>
#include <cmath>

namespace app {
namespace {

template <typename T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// Decodes the embedded JPEG `resource` into `format` with the Windows Imaging Component, which
// every Windows has, so no image library is needed.
bool Decode(int resource, const GUID& format, int bytesPerPixel, int& width, int& height,
            std::vector<uint8_t>& pixels) {
    HRSRC   res  = FindResourceW(nullptr, MAKEINTRESOURCEW(resource), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    if (!data) return false;

    // Already initialised on this thread in another mode is fine: COM is usable either way.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    bool                   ok        = false;
    IWICImagingFactory*    factory   = nullptr;
    IWICStream*            stream    = nullptr;
    IWICBitmapDecoder*     decoder   = nullptr;
    IWICBitmapFrameDecode* frame     = nullptr;
    IWICFormatConverter*   converter = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(data)), SizeofResource(nullptr, res))) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(frame, format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        converter->GetSize(&w, &h);
        width  = static_cast<int>(w);
        height = static_cast<int>(h);
        pixels.resize(static_cast<size_t>(w) * h * bytesPerPixel);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, w * bytesPerPixel, static_cast<UINT>(pixels.size()), pixels.data()));
    }
    Release(converter);
    Release(frame);
    Release(decoder);
    Release(stream);
    Release(factory);
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

// The full mip chain of `top`, each level the 2x2 box average of the one above.
std::vector<std::vector<uint8_t>> Mips(std::vector<uint8_t> top, int width, int height, int channels) {
    std::vector<std::vector<uint8_t>> levels;
    levels.push_back(std::move(top));
    int w = width, h = height;
    while (w > 1 || h > 1) {
        const int                   nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        const std::vector<uint8_t>& src = levels.back();
        std::vector<uint8_t>        dst(static_cast<size_t>(nw) * nh * channels);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
                const int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
                for (int c = 0; c < channels; ++c) {
                    const auto at = [&](int xx, int yy) { return src[(static_cast<size_t>(yy) * w + xx) * channels + c]; };
                    dst[(static_cast<size_t>(y) * nw + x) * channels + c] =
                        static_cast<uint8_t>((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
                }
            }
        }
        levels.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
    return levels;
}

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

float Smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

const render::EarthImages& EarthTextures() {
    static const render::EarthImages images = [] {
        render::EarthImages  e;
        int                  w = 0, h = 0, nw = 0, nh = 0;
        std::vector<uint8_t> day, night;
        if (!Decode(IDR_EARTH_DAY, GUID_WICPixelFormat32bppRGBA, 4, w, h, day) ||
            !Decode(IDR_EARTH_NIGHT, GUID_WICPixelFormat8bppGray, 1, nw, nh, night) || nw != w || nh != h) {
            Log("earth: the textures could not be decoded");
            return e;
        }
        e.id     = 1;
        e.width  = w;
        e.height = h;
        e.day    = Mips(std::move(day), w, h, 4);
        e.night  = Mips(std::move(night), w, h, 1);
        Log("earth: textures decoded, %dx%d, %u levels", w, h, static_cast<unsigned>(e.day.size()));
        return e;
    }();
    return images;
}

void EarthColor(const render::EarthImages& images, const render::EarthDraw& earth, const float n[3],
                const float view[3], float lod, float out[3]) {
    constexpr float kPi     = 3.14159265f;
    const float     sun[3]  = {1.0f, 0.88f, 0.74f};
    const float     city[3] = {1.0f, 0.72f, 0.42f};
    const float     air[3]  = {0.35f, 0.6f, 1.0f};

    // Where on the map, as shaders/earth.frag works it out.
    const float across[3] = {earth.east[1] * earth.north[2] - earth.east[2] * earth.north[1],
                             earth.east[2] * earth.north[0] - earth.east[0] * earth.north[2],
                             earth.east[0] * earth.north[1] - earth.east[1] * earth.north[0]};
    const float lon       = std::atan2(Dot(n, across), Dot(n, earth.east)) + earth.spin;
    const float lat       = std::asin(std::clamp(Dot(n, earth.north), -1.0f, 1.0f));
    float       u         = lon / (2.0f * kPi) + 0.5f;
    u -= std::floor(u);
    const float v = 0.5f - lat / kPi;

    // The nearest texel of the nearest level.
    const int level = std::clamp(static_cast<int>(std::lround(lod)), 0, static_cast<int>(images.day.size()) - 1);
    const int lw    = std::max(1, images.width >> level), lh = std::max(1, images.height >> level);
    const int x     = std::clamp(static_cast<int>(u * lw), 0, lw - 1);
    const int y     = std::clamp(static_cast<int>(v * lh), 0, lh - 1);
    const size_t at = static_cast<size_t>(y) * lw + x;

    const float ndl    = Dot(n, earth.light);
    const float lit    = 1.6f * std::max(ndl, 0.0f);
    const float lights = 6.0f * (images.night[level][at] / 255.0f) * (1.0f - Smoothstep(-0.12f, 0.08f, ndl));
    const float rim    = std::pow(1.0f - std::clamp(Dot(n, view), 0.0f, 1.0f), 3.0f);
    const float glow   = 0.9f * rim * (0.05f + 0.95f * Smoothstep(-0.25f, 0.45f, ndl));
    for (int c = 0; c < 3; ++c) {
        const float albedo = std::pow(images.day[level][at * 4 + c] / 255.0f, 2.2f);
        out[c]             = albedo * sun[c] * lit + city[c] * lights + air[c] * glow;
    }
}

}  // namespace app
