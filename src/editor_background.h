#pragma once
#include <cstdint>
// Called only at the native main-loop world-update site, before any inline replay or cap.
// True means the editor was updated on the main thread and the native worker owns world replay.
bool editor_background_frame(uintptr_t game, uintptr_t application, uintptr_t data);
// The replay owner publishes queue depth after a native update; the render thread never reads the live queue.
void editor_background_world_tick(uintptr_t game);
