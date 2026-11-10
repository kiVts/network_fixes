#pragma once
#include <cstddef>
#include <cstdint>

namespace aimtrace {
// Checked against BOTH actual PE files by tools/check_aim_trace_native.py.
// These are instruction contracts, not unverified offsets from cached C.
inline constexpr uint8_t camera_angles[] = {
    0xf3,0x44,0x0f,0x11,0x9f,0x7c,0x06,0,0,
    0xf3,0x44,0x0f,0x11,0xa7,0x78,0x06,0,0};
inline constexpr uint8_t camera_modes[] = {0x80,0xbf,0xc1,0x06,0,0,0};
inline constexpr uint8_t camera_input[] = {0xf3,0x44,0x0f,0x10,0x8f,0x98,0x04,0,0};
inline constexpr uint8_t component_ring[] = {0x48,0x8b,0x83,0x50,0x04,0,0};
inline constexpr uint8_t body_scene[] = {0x48,0x83,0xbb,0x18,0x07,0,0,0};

inline bool contains(const uint8_t* code, size_t size, const uint8_t* pattern, size_t n)
{
    if (!code || size < n) return false;
    for (size_t i=0; i<=size-n; ++i) {
        size_t j=0;
        while (j<n && code[i+j]==pattern[j]) ++j;
        if (j==n) return true;
    }
    return false;
}
}
