// Compatibility with saved background-editor settings. Cooperative replay stays in the native inline lane.
// The editor and simulation share the world and deferred-render queue (both actual supported PEs).
// Starting a second native worker here is unsafe; do not restore it by changing a feature flag.
#include "editor_background.h"
#include "hooks.h"
#include "log.h"
#include <windows.h>

namespace {
bool install()
{
    const int requested = GetPrivateProfileIntW(L"net", L"editor_background", 0, settings_ini());
    if (requested)
        swlog("editor background: editor_background=%d is no longer used (editor and simulation share mutable world/render queues); editors"
              " run on the main thread; cooperative replay is [net] editor_replay_ms", requested);
    return true;
}
HookRegistrar reg{{"editor replay safety", false, &install}};
} // namespace

bool editor_background_frame(uintptr_t, uintptr_t, uintptr_t)
{
    // Never suppress the normal update, including with legacy overrides or an unfamiliar game build.
    return false;
}

void editor_background_world_tick(uintptr_t) {}
