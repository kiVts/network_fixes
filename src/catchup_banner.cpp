
#include "game.h"
#include "gl_text.h"
#include "hooks.h"
#include "log.h"
#include "sdk.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace {

constexpr uintptr_t kCatchUp = 0x1408686c0;
constexpr uint32_t kQueueCount = 0xce300;
constexpr uint32_t kHideTicks = 14;
constexpr ULONGLONG kStaleMs = 2000;

using U = uint64_t;
using CatchUp_t = void(__fastcall*)(U, U, U, U, U);
CatchUp_t g_orig = nullptr;

uint32_t g_show_ticks = 30;
std::atomic<uint32_t> g_queued{0};
std::atomic<ULONGLONG> g_at{0};
bool g_shown = false; // render thread
bool g_failed_logged = false;

void __fastcall catchup_detour(U g, U a2, U a3, U a4, U a5)
{
    g_orig(g, a2, a3, a4, a5);
    g_queued.store(*reinterpret_cast<const uint32_t*>(g + kQueueCount), std::memory_order_relaxed);
    g_at.store(GetTickCount64(), std::memory_order_relaxed);
}

void on_present(void* hdc, void*)
{
    ULONGLONG at = g_at.load(std::memory_order_relaxed);
    uint32_t q = g_queued.load(std::memory_order_relaxed);
    bool fresh = at && GetTickCount64() - at < kStaleMs;
    bool show = fresh && (g_shown ? q > kHideTicks : q > g_show_ticks);
    if (show != g_shown) {
        g_shown = show;
        swlog("catch-up banner: %s (%u ticks queued)", show ? "shown" : "hidden", q);
    }
    if (!show) return;
    HDC dc = static_cast<HDC>(hdc);
    RECT rc{};
    HWND wnd = WindowFromDC(dc);
    int h = wnd && GetClientRect(wnd, &rc) ? rc.bottom - rc.top : 1080;
    int size = std::clamp(h / 24, 18, 96);
    if (!gltext::draw_top(dc, "CATCHING UP...", size, std::max(8, h / 40), {0.95f, 0.12f, 0.10f, 1.0f}) && !g_failed_logged) {
        g_failed_logged = true;
        swlog("catch-up banner: couldn't draw (see \"text overlay:\")");
    }
}

bool install()
{
    const wchar_t* ini = settings_ini();
    if (!GetPrivateProfileIntW(L"mp", L"catchup_banner", 1, ini)) {
        swlog("catch-up banner: off in settings.ini");
        return true;
    }
    g_show_ticks = std::clamp(int(GetPrivateProfileIntW(L"mp", L"banner_ticks", 30, ini)), 15, 100000);
    if (!hook_game(kCatchUp, reinterpret_cast<void*>(&catchup_detour), reinterpret_cast<void**>(&g_orig))) return false;
    swml()->on_present(swml_self(), &on_present, nullptr);
    swlog("catch-up banner: on (\"CATCHING UP...\" while more than %u host ticks are queued)", g_show_ticks);
    return true;
}

HookRegistrar reg{{"catch-up banner", /*needs_known_build=*/true, &install}};

} // namespace
