// Where the camera is and what it looks at. Units are the black hole's mass (G = c = M = 1); the
// hole is at the origin, spin along +y, the disk in the plane y = 0.
#ifndef BLACK_HOLE_CAMERA_H
#define BLACK_HOLE_CAMERA_H

#include <cstdint>
#include <random>

namespace render {

struct CameraPose {
    float position[3];
    float target[3];
    float roll;        // radians about the view axis
    float tanHalfFov;  // vertical
};

// The original path: a slow orbit a few degrees above the disk, a function of time alone, so a
// frame at a given time can always be captured again. Development and captures use it.
CameraPose ClassicCamera(double seconds);

// The screen saver's camera. Every quantity drifts towards a randomly chosen target, holds there
// for a while, then picks another: the pace (sometimes two or three times the classic speed),
// the distance (from close enough that the disk runs off the screen to far enough that the whole
// system is small), the height above the disk, the roll, and where in the frame the hole sits.
//
// Every few minutes, on average, it also breaks pattern with an event: a dive through the disk's
// plane to look at it from below, a climb to look down on it from high above, or a fast swoop in
// close past the hole.
class RoamingCamera {
public:
    enum class Event { Any, Dive, Overhead, Swoop };

    // `only` restricts the events to one kind and brings the first forward, for development.
    explicit RoamingCamera(uint32_t seed, Event only = Event::Any);

    // Advances by dt seconds of real time and returns the new pose.
    CameraPose Advance(double dt);

private:
    // One quantity: eases from `from` to `to` over `move` seconds, then holds for `hold`.
    struct Drift {
        float from = 0.0f, to = 0.0f;
        float elapsed = 0.0f, move = 1.0f, hold = 0.0f;
        float Value() const;
        bool  Done() const { return elapsed >= move + hold; }
    };

    void PickTempo();
    bool StartEvent();
    void PickDistance();
    void PickHeight();
    void PickRoll();
    void PickAim();
    float Uniform(float lo, float hi);

    std::mt19937 rng_;
    Event        only_;
    float        azimuth_;
    float        untilEvent_;  // tempo-scaled seconds
    Drift        tempo_, distance_, inclination_, roll_, aimX_, aimY_;
};

}  // namespace render

#endif
