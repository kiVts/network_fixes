#pragma once
#include <cstdint>

// Read-only, opt-in observation at the existing mp_watch post-simulation boundary.
// No new native detour and no dependency on Physics Fixes being enabled.
bool aim_trace_install();
void aim_trace_sample(uintptr_t simulation, uintptr_t player, bool host);
