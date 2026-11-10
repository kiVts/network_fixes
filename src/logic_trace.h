#pragma once
#include <cstdint>
bool logic_trace_install();
void logic_trace_begin(uintptr_t simulation, uintptr_t player, bool host);
void logic_trace_end();
// Optional combined capture. Callers retain their existing native ABI.
void logic_trace_catalog_body(uintptr_t body, uintptr_t scene, uint32_t vehicle, uint32_t index);
void logic_trace_correction(uintptr_t message, uint32_t peer, bool sending, uint32_t tick);
void logic_trace_received(uintptr_t ring, uintptr_t player);
void logic_trace_target(uintptr_t body, uint32_t reach, const double* transform, uint32_t stage, uintptr_t player);
void logic_trace_scene(uintptr_t scene_object, const double* transform, bool suppressed);
