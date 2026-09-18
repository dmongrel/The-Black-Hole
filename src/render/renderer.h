// The Vulkan renderer. One device, one pipeline, one skybox, any number of windows (one per
// monitor in full-screen mode), each with its own surface and swapchain.
//
// Every frame is a single full-screen triangle: shaders/blackhole.frag integrates each pixel's
// light ray through the black hole's curved spacetime and samples the skybox wherever it escapes.
#ifndef BLACK_HOLE_RENDERER_H
#define BLACK_HOLE_RENDERER_H

#include <windows.h>

#include <memory>
#include <string>

namespace render {

class Renderer {
public:
    // Returns nullptr when Vulkan is missing or unusable. Never throws, never shows UI.
    static std::unique_ptr<Renderer> Create();
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    // False when this device cannot present to the window.
    bool AttachWindow(HWND hwnd);
    // The window changed size; its swapchain is rebuilt before the next frame.
    void ResizeWindow(HWND hwnd);
    void DetachWindow(HWND hwnd);

    // Draws every attached window. `seconds` is animation time.
    void RenderFrame(double seconds);

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
