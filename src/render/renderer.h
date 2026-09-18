// The Vulkan renderer. One device, one pipeline, one skybox, any number of windows (one per
// monitor in full-screen mode), each with its own surface and swapchain.
//
// Every frame has three passes. shaders/blackhole.frag integrates each pixel's light ray through
// the black hole's curved spacetime into an HDR image, sampling the skybox wherever a ray escapes;
// a compute chain (bloom_down.comp, bloom_up.comp) blurs its brightest light into a glow; and
// composite.frag adds the glow back, tone maps, and writes the swapchain image.
#ifndef BLACK_HOLE_RENDERER_H
#define BLACK_HOLE_RENDERER_H

#include <windows.h>

#include <memory>
#include <string>

#include "render/camera.h"

namespace render {

class Renderer {
public:
    // Returns nullptr when Vulkan is missing or unusable. Never throws, never shows UI.
    // `skyboxSize` is the star field's cube face edge in texels: 2048 for a monitor, far less
    // for the Settings dialog's thumbnail, where baking the full one would only delay it.
    static std::unique_ptr<Renderer> Create(int skyboxSize = 2048);
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // False when this device cannot present to the window.
    bool AttachWindow(HWND hwnd);
    // The window changed size; its swapchain is rebuilt before the next frame.
    void ResizeWindow(HWND hwnd);
    void DetachWindow(HWND hwnd);

    // Draws every attached window. `seconds` is animation time (the disk's turbulence); `camera`
    // is where the frame is seen from.
    void RenderFrame(double seconds, const CameraPose& camera);

    // Writes the next frame presented to `hwnd` to a 32-bit BMP at `path`.
    void RequestCapture(HWND hwnd, const std::string& path);
    bool CapturePending() const;

    void WaitIdle();

    struct Impl;

private:
    explicit Renderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace render

#endif
