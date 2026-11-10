#pragma once
#include "native_patch.h"
#include <cstdint>
#include <cstring>

namespace host_map {
inline constexpr uint8_t contract[] = {
    0x41,0x8b,0x86,0x88,0xe4,0x0c,0, 0x3b,0xc1, 0x0f,0x47,0xc8,
    0x41,0x89,0x8e,0x88,0xe4,0x0c,0, 0x44,0x2b,0xe1, 0x44,0x3b,0xe7};
inline constexpr size_t patch_offset = 12, patch_size = 10;
struct Call { uintptr_t game; bool allow, reached = false; uint32_t step = 16; };
inline thread_local Call* active = nullptr;
inline void gate(swml4::patch::Registers& r) {
    auto* call = active;
    if (!call || r.r14 != call->game || !r.rdi || r.rdi > 1000) return;
    auto* last = reinterpret_cast<uint32_t*>(r.r14 + 0xce488);
    const uint32_t now = *last + uint32_t(r.r12); // unsigned native clock rollover
    call->reached = true; call->step = uint32_t(r.rdi);
    // Keep the native clock close to real time for the handoff back to the
    // update thread. Only the decision to run this native tick is controlled.
    *last = call->allow ? now - call->step - 1 : now;
    r.r12 = call->allow ? call->step + 1 : 0;
}
inline uint8_t* find_site(uint8_t* fn, size_t size) {
    uint8_t* found = nullptr;
    if (!fn || size < sizeof(contract)) return nullptr;
    for (size_t i = 0; i + sizeof(contract) <= size; ++i) {
        if (memcmp(fn + i, contract, sizeof(contract))) continue;
        if (found) return nullptr;
        found = fn + i;
    }
    return found;
}
inline bool install(uint8_t* fn, size_t size) {
    auto* site = find_site(fn, size);
    return site && swml4::patch::mid_hook(site + patch_offset, contract + patch_offset, patch_size, gate);
}
}
