#include "render/camera.h"

#include <algorithm>
#include <cmath>

namespace render {
namespace {

constexpr float kDegrees      = 0.017453293f;
constexpr float kTanHalfFov   = 0.44522869f;  // tan(24 degrees)
constexpr float kBaseOrbit    = 0.012f;       // radians per second at tempo 1, the classic pace

float Smootherstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

CameraPose Pose(float azimuth, float inclination, float distance, float roll, float aimX, float aimY) {
    const float px = distance * std::cos(inclination) * std::cos(azimuth);
    const float py = distance * std::sin(inclination);
    const float pz = distance * std::cos(inclination) * std::sin(azimuth);

    // The aim is an angular offset from the hole, turned into a point beside it: right and up of
    // the line of sight, scaled by the distance so the angle is what it says.
    const float fl = std::sqrt(px * px + py * py + pz * pz);
    const float fx = -px / fl, fy = -py / fl, fz = -pz / fl;
    float       rx = -fz, rz = fx;  // cross(forward, +y)
    const float rl = std::sqrt(rx * rx + rz * rz);
    rx /= rl;
    rz /= rl;
    const float ux = -rz * fy, uy = rz * fx - rx * fz, uz = rx * fy;  // cross(right, forward)
    const float ox = distance * std::tan(aimX), oy = distance * std::tan(aimY);

    CameraPose pose{};
    pose.position[0] = px;
    pose.position[1] = py;
    pose.position[2] = pz;
    pose.target[0]   = rx * ox + ux * oy;
    pose.target[1]   = uy * oy;
    pose.target[2]   = rz * ox + uz * oy;
    pose.roll        = roll;
    pose.tanHalfFov  = kTanHalfFov;
    return pose;
}

}  // namespace

CameraPose ClassicCamera(double seconds) {
    const float t = static_cast<float>(seconds);
    return Pose(0.6f + t * kBaseOrbit, (7.0f + 4.0f * std::sin(t * 0.037f)) * kDegrees,
                29.0f + 4.0f * std::sin(t * 0.021f), (-6.0f + 3.0f * std::sin(t * 0.029f)) * kDegrees, 0.0f,
                0.0f);
}

float RoamingCamera::Drift::Value() const { return from + (to - from) * Smootherstep(elapsed / move); }

float RoamingCamera::Uniform(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng_); }

RoamingCamera::RoamingCamera(uint32_t seed) : rng_(seed), azimuth_(Uniform(0.0f, 6.2831853f)) {
    // Start where the classic camera does, so the first seconds look like the saver always has,
    // and let every quantity wander off from there.
    tempo_       = {1.0f, 1.0f, 0.0f, 1.0f, Uniform(20.0f, 40.0f)};
    distance_    = {29.0f, 29.0f, 0.0f, 1.0f, Uniform(10.0f, 25.0f)};
    inclination_ = {7.0f * kDegrees, 7.0f * kDegrees, 0.0f, 1.0f, Uniform(5.0f, 15.0f)};
    roll_        = {-6.0f * kDegrees, -6.0f * kDegrees, 0.0f, 1.0f, Uniform(5.0f, 15.0f)};
    aimX_        = {0.0f, 0.0f, 0.0f, 1.0f, Uniform(5.0f, 15.0f)};
    aimY_        = aimX_;
}

// Mostly the classic pace; now and then noticeably brisker.
void RoamingCamera::PickTempo() {
    const float roll = Uniform(0.0f, 1.0f);
    const float next = roll < 0.55f ? Uniform(0.8f, 1.2f) : roll < 0.85f ? Uniform(1.6f, 2.2f) : Uniform(2.6f, 3.4f);
    tempo_           = {tempo_.Value(), next, 0.0f, Uniform(5.0f, 10.0f), Uniform(20.0f, 50.0f)};
}

// Close enough that the disk overflows the frame, the classic middle distance, or far enough out
// that the hole and its disk sit small in the star field. Moves take longer the further they go.
void RoamingCamera::PickDistance() {
    const float roll = Uniform(0.0f, 1.0f);
    const float next = roll < 0.25f ? Uniform(12.5f, 18.0f) : roll < 0.75f ? Uniform(22.0f, 34.0f) : Uniform(42.0f, 64.0f);
    const float now  = distance_.Value();
    const float move = (10.0f + 0.5f * std::fabs(next - now)) * Uniform(0.8f, 1.2f);
    distance_        = {now, next, 0.0f, move, Uniform(10.0f, 35.0f)};
}

// Mostly just above the disk, as in the film; once in a while high enough to look down on it.
void RoamingCamera::PickHeight() {
    const float next = Uniform(0.0f, 1.0f) < 0.85f ? Uniform(4.0f, 15.0f) : Uniform(18.0f, 28.0f);
    inclination_     = {inclination_.Value(), next * kDegrees, 0.0f, Uniform(12.0f, 25.0f), Uniform(5.0f, 25.0f)};
}

void RoamingCamera::PickRoll() {
    roll_ = {roll_.Value(), Uniform(-11.0f, 5.0f) * kDegrees, 0.0f, Uniform(10.0f, 20.0f), Uniform(5.0f, 25.0f)};
}

// Where the hole sits in the frame. Centred at a distance; up close, pushed towards an edge so
// the disk runs off the screen on one side.
void RoamingCamera::PickAim() {
    const bool  close = distance_.to < 20.0f;
    const float side  = Uniform(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;
    const float x     = close ? side * Uniform(6.0f, 15.0f) : Uniform(-3.0f, 3.0f);
    const float y     = close ? Uniform(-6.0f, 4.0f) : Uniform(-2.0f, 2.0f);
    const float move  = Uniform(10.0f, 18.0f), hold = Uniform(8.0f, 25.0f);
    aimX_             = {aimX_.Value(), x * kDegrees, 0.0f, move, hold};
    aimY_             = {aimY_.Value(), y * kDegrees, 0.0f, move, hold};
}

CameraPose RoamingCamera::Advance(double dt) {
    const float realDt = static_cast<float>(dt);
    tempo_.elapsed += realDt;
    if (tempo_.Done()) PickTempo();

    // Everything but the tempo itself runs on tempo-scaled time, so a brisk stretch speeds up the
    // wandering as well as the orbit.
    const float tempo = tempo_.Value();
    const float step  = realDt * tempo;
    azimuth_ += step * kBaseOrbit;

    for (Drift* d : {&distance_, &inclination_, &roll_, &aimX_}) d->elapsed += step;
    aimY_.elapsed = aimX_.elapsed;
    if (distance_.Done()) PickDistance();
    if (inclination_.Done()) PickHeight();
    if (roll_.Done()) PickRoll();
    if (aimX_.Done()) PickAim();

    return Pose(azimuth_, inclination_.Value(), distance_.Value(), roll_.Value(), aimX_.Value(), aimY_.Value());
}

}  // namespace render
