#pragma once
#include <cstdint>
// What the client's character does to a host-simulated vehicle body in one physics step, in the body's local frame.
// The host turns it into the forces its own character would have put there: it pushes the vehicle along dir until the
// contact point moves at the character's approach speed, presses down with the character's weight while it stands on
// the vehicle and adds the impact of a landing once.
struct VehiclePushSample2 {
    const void* body;
    float point[3]; // contact point, body-local, impulse-weighted
    float dir[3];   // horizontal push direction, body-local, unit (zero: no horizontal push)
    float approach; // character speed into the vehicle along dir before contact, m/s (relative to the ground)
    float support;  // downward force of the character standing on it, N
    float land;     // landing speed onto it, m/s (0: no landing this step)
    float kg;       // the character's mass
};
using VehicleSamples2_t = int(__cdecl*)(VehiclePushSample2* out, int max, uint32_t* seq);
