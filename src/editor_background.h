#pragma once
#include <cstdint>
// Compatibility shim for the retired unsafe worker prototype. Always false: the native inline
// update and editor cap must run. Never reads game objects, starts workers or installs hooks.
bool editor_background_frame(uintptr_t game, uintptr_t application, uintptr_t data);
// Retained no-op for existing callers; no background queue-depth observer remains.
void editor_background_world_tick(uintptr_t game);
