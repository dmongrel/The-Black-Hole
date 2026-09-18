// The credits, played over the black hole like the titles of a film.
//
// Each credit flies in from behind the camera and comes to rest in the lower left of the screen,
// fixed to the camera. After a hold the letters pixelate, and the blocks come loose one by one
// all over the line, each a particle. They leave the camera behind and stream in a spiralling
// arc down onto the accretion disk, shrinking and dimming, and fade out on the way to its inner edge. A second later the next credit comes in;
// after the last, a minute's pause, then the list again from the top.
//
// Everything is a function of time and the camera, so the same moment always looks the same.
#ifndef BLACK_HOLE_CREDIT_ROLL_H
#define BLACK_HOLE_CREDIT_ROLL_H

#include "app/credits.h"
#include "render/camera.h"
#include "render/overlay.h"

#include <cstdint>
#include <vector>

namespace app {

class CreditRoll {
public:
    // `width` and `height` are the pixel size of the window the credits play on; the text is
    // set to suit it.
    CreditRoll(std::vector<Credit> credits, int width, int height);

    bool Empty() const { return credits_.empty(); }

    // The overlay at `seconds` into the run, seen from `camera`. Call with increasing times.
    const render::Overlay& Update(double seconds, const render::CameraPose& camera);

private:
    struct Particle {
        float  local[3];     // where its block sits in the text, in the camera's frame
        float  release;      // seconds after the text has pixelated
        float  linger;       // seconds a heavy one hangs back before the flow takes it; 0 for most
        float  flight;       // seconds from leaving to the disk
        float  turn;         // how far round the hole it swings on the way, in radians
        float  phase;        // of its wobble, shared with its neighbours so they stream together
        float  scatter[3];   // its drift as it comes loose; camera frame, then world
        float  radius;       // pixels: half its block's width
        float  brightness;
        bool   released = false;
        double releasedAt = 0.0;
        float  start[3]{};   // world position at release
        float  via[3]{};     // deep in the scene, still where the text was on screen
        float  bend[3]{};    // on the disk, beside the hole: where the spiral begins
        float  endAngle = 0; // round the spin axis, where it meets the inner edge
    };

    void  StartCredit(size_t index);
    void  BreakUp(const render::TextImage& text);
    float HoldSeconds() const;
    float CreditSeconds() const;  // from its fly-in to the last particle gone, plus the gap

    std::vector<Credit> credits_;
    int                 width_, height_;

    size_t   index_      = 0;
    double   start_      = 0.0;  // when the current credit began to fly in
    bool     started_    = false;
    uint32_t nextTextId_ = 1;
    bool     brokenUp_   = false;
    int      block_      = 3;  // texels on a side of each block the text pixelates into

    render::TextImage     text_;
    float                 restRect_[4]{};  // at the camera's current field of view
    std::vector<Particle> particles_;
    render::Overlay       overlay_;
};

}  // namespace app

#endif
